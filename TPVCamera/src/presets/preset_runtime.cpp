/**
 * @file preset_runtime.cpp
 * @brief Render-thread preset resolver: state -> target preset -> eased apply to live settings.
 */

#include "preset_runtime.hpp"
#include "config.hpp"
#include "game_state.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <concepts>
#include <span>
#include <utility>

namespace TPVCamera::Presets
{
    namespace
    {

        // Written only by compare-exchange (publish_table, republish_picks), read by the render thread via an atomic
        // shared_ptr load each frame (a spinlock-guarded refcount bump on MSVC, no heap allocation).
        std::atomic<std::shared_ptr<const StateBindingTable>> s_table{nullptr};

        // Render-thread-only transition state (resolve_and_apply / reset_transition run only there).
        CameraPreset s_applied;
        CameraPreset s_stage1; // intermediate state of the 2-stage critically-damped preset blend (see below)
        bool s_have_applied = false;
        bool s_snap_next = false;

        /// Trims leading and trailing ASCII whitespace from a view (no allocation).
        [[nodiscard]] std::string_view trim_token(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace = " \t\r\n";
            const std::size_t begin = text.find_first_not_of(whitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
        }

        /**
         * @brief Equal-specificity tiebreak weight for a bind mask.
         * @details Each framing bit gets a distinct power of two, so any mask's summed weight is unique
         *          (a bitset) and a tie resolves by the highest-priority bit present. Priority, high to
         *          low: Aiming > Crouch > Combat > Mount > Lying > Sitting > Kneel > Cart. The stance bits
         *          (Mount, Crouch, Lying, Sitting, Kneel, Cart) are mutually exclusive in practice, so their
         *          order only matters versus the orthogonal Combat / Aiming bits. winning_mask scales
         *          popcount above this sum (which is < 256), so specificity still dominates.
         */
        [[nodiscard]] std::uint32_t bind_priority_weight(std::uint32_t mask) noexcept
        {
            std::uint32_t weight = 0;
            if ((mask & state_bit(GameState::Aiming)) != 0)
                weight += 128;
            if ((mask & state_bit(GameState::Crouch)) != 0)
                weight += 64;
            if ((mask & state_bit(GameState::Combat)) != 0)
                weight += 32;
            if ((mask & state_bit(GameState::Mount)) != 0)
                weight += 16;
            if ((mask & state_bit(GameState::Lying)) != 0)
                weight += 8;
            if ((mask & state_bit(GameState::Sitting)) != 0)
                weight += 4;
            if ((mask & state_bit(GameState::Kneel)) != 0)
                weight += 2;
            if ((mask & state_bit(GameState::Cart)) != 0)
                weight += 1;
            return weight;
        }

        /// Number of entries in @p table, whose preset and mask vectors the store fills in step.
        [[nodiscard]] std::size_t entry_count(const StateBindingTable &table) noexcept
        {
            return std::min(table.presets.size(), table.masks.size());
        }

        /**
         * @brief The mask of the most-specific binding in @p masks that is a subset of @p active_state.
         * @details Specificity (set-bit count) dominates and bind_priority_weight breaks equal-specificity ties.
         *          Two masks that match together with an equal score are identical: the weight is unique per
         *          framing-bit set, the only unweighted bits are the minigame umbrella and its children, and at most
         *          one child is active at a time. A zero mask always matches (the floor).
         * @return The winning mask, or std::nullopt when no mask is a subset of @p active_state.
         */
        [[nodiscard]] std::optional<std::uint32_t> winning_mask(std::uint32_t active_state,
                                                                std::span<const std::uint32_t> masks) noexcept
        {
            std::optional<std::uint32_t> best;
            std::uint32_t best_score = 0;
            for (const std::uint32_t mask : masks)
            {
                // The preset matches only when EVERY bit it requires is active.
                if ((mask & active_state) != mask)
                {
                    continue;
                }
                const std::uint32_t score =
                    static_cast<std::uint32_t>(std::popcount(mask)) * 256u + bind_priority_weight(mask);
                if (!best || score > best_score)
                {
                    best = mask;
                    best_score = score;
                }
            }
            return best;
        }

        /// The pick of cycle group @p mask, or nullptr when the group has none.
        [[nodiscard]] const CyclePick *find_pick(std::span<const CyclePick> picks, std::uint32_t mask) noexcept
        {
            for (const CyclePick &pick : picks)
            {
                if (pick.mask == mask)
                {
                    return &pick;
                }
            }
            return nullptr;
        }

        /// Points cycle group @p mask at @p preset, adding the group's pick when it has none.
        void assign_pick(std::vector<CyclePick> &picks, std::uint32_t mask, std::string_view preset)
        {
            for (CyclePick &pick : picks)
            {
                if (pick.mask == mask)
                {
                    pick.preset = preset;
                    return;
                }
            }
            picks.push_back(CyclePick{
                .mask = mask,
                .preset = std::string(preset),
            });
        }

        /**
         * @brief Republishes the live snapshot with its cycle picks rewritten by @p edit.
         * @details @p edit receives the live snapshot and a copy of its picks, and returns false to publish nothing.
         *          A lost compare-exchange runs it again on the newer snapshot, so it must derive its result from its
         *          arguments alone. Nothing is published before the store's first publish.
         */
        template <typename Edit>
            requires std::predicate<Edit &, const StateBindingTable &, std::vector<CyclePick> &>
        void republish_picks(Edit &&edit)
        {
            std::shared_ptr<const StateBindingTable> current = s_table.load(std::memory_order_acquire);
            while (current)
            {
                auto next = std::make_shared<StateBindingTable>(*current);
                if (!edit(*current, next->cycle_picks))
                {
                    return;
                }
                if (s_table.compare_exchange_weak(current, std::shared_ptr<const StateBindingTable>(std::move(next)),
                                                  std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return;
                }
            }
        }

        /**
         * @brief Resolves the active preset target for @p state: the editing pin wins, otherwise the entry
         *        resolve_active_entry picks.
         * @details Falls back to a neutral preset only if the table is empty, which cannot happen once the store
         *          has published since DEFAULT is always bindable.
         */
        [[nodiscard]] const CameraPreset &select_target(const StateBindingTable &table, std::uint32_t state) noexcept
        {
            if (table.has_pin)
            {
                return table.pinned;
            }
            const int index = resolve_active_entry(table, state);
            if (index >= 0)
            {
                return table.presets[static_cast<std::size_t>(index)];
            }
            static const CameraPreset s_fallback{};
            return s_fallback;
        }

    } // namespace

    std::optional<std::uint32_t> parse_bind_mask(std::string_view bind_state)
    {
        std::uint32_t mask = 0;
        bool bound = false;

        std::size_t start = 0;
        while (start <= bind_state.size())
        {
            const std::size_t comma = bind_state.find(',', start);
            const std::size_t end = (comma == std::string_view::npos) ? bind_state.size() : comma;
            const std::string_view token = trim_token(bind_state.substr(start, end - start));
            if (!token.empty() && token != "none")
            {
                // "default" binds to the empty-mask floor (contributes no bits but marks the preset bound);
                // the framing tokens add their bit. Unknown tokens are ignored.
                if (token == "default")
                {
                    bound = true;
                }
                else if (token == "combat")
                {
                    mask |= state_bit(GameState::Combat);
                    bound = true;
                }
                else if (token == "aiming")
                {
                    mask |= state_bit(GameState::Aiming);
                    bound = true;
                }
                else if (token == "mount")
                {
                    mask |= state_bit(GameState::Mount);
                    bound = true;
                }
                else if (token == "crouch" || token == "stealth")
                {
                    mask |= state_bit(GameState::Crouch);
                    bound = true;
                }
                else if (token == "lying")
                {
                    mask |= state_bit(GameState::Lying);
                    bound = true;
                }
                else if (token == "sitting")
                {
                    mask |= state_bit(GameState::Sitting);
                    bound = true;
                }
                else if (token == "kneel")
                {
                    mask |= state_bit(GameState::Kneel);
                    bound = true;
                }
                else if (token == "cart")
                {
                    mask |= state_bit(GameState::Cart);
                    bound = true;
                }
                else if (token == "minigame")
                {
                    mask |= state_bit(GameState::Minigame);
                    bound = true;
                }
                else
                {
                    // A per-minigame token binds {umbrella Minigame | child}, so a minigame-specific preset
                    // (2 bits) wins over a generic "minigame" preset (1 bit) by specificity while both still
                    // match (poll_active_minigame sets the umbrella and the child together). Unknown tokens
                    // fall through and are ignored.
                    for (const MinigameInfo &mg : k_minigames)
                    {
                        if (token == mg.token)
                        {
                            mask |= state_bit(GameState::Minigame) | state_bit(mg.bit);
                            bound = true;
                            break;
                        }
                    }
                }
            }
            if (comma == std::string_view::npos)
            {
                break;
            }
            start = comma + 1;
        }

        if (!bound)
        {
            return std::nullopt;
        }
        return mask;
    }

    std::string bind_mask_to_tokens(std::uint32_t mask)
    {
        std::string out;
        const auto append = [&](GameState bit, const char *token)
        {
            if ((mask & state_bit(bit)) != 0)
            {
                if (!out.empty())
                {
                    out += ',';
                }
                out += token;
            }
        };
        append(GameState::Aiming, "aiming");
        append(GameState::Crouch, "crouch");
        append(GameState::Combat, "combat");
        append(GameState::Mount, "mount");
        append(GameState::Lying, "lying");
        append(GameState::Sitting, "sitting");
        append(GameState::Kneel, "kneel");
        append(GameState::Cart, "cart");
        // Minigame: emit the per-minigame token when a child bit is set (the umbrella is then implied), else
        // the generic "minigame" token when only the umbrella bit is set. At most one child bit is ever set.
        bool minigame_done = false;
        for (const MinigameInfo &mg : k_minigames)
        {
            if ((mask & state_bit(mg.bit)) != 0)
            {
                if (!out.empty())
                {
                    out += ',';
                }
                out += mg.token;
                minigame_done = true;
                break;
            }
        }
        if (!minigame_done)
        {
            append(GameState::Minigame, "minigame");
        }
        return out.empty() ? std::string("none") : out;
    }

    std::string cycle_group_tokens(std::uint32_t mask)
    {
        // bind_mask_to_tokens spells the empty mask "none" (an unbound user preset), but as a group the empty mask
        // is the DEFAULT floor, which parse_bind_mask reads from "default".
        return (mask == 0) ? std::string("default") : bind_mask_to_tokens(mask);
    }

    int resolve_active_entry(const StateBindingTable &table, std::uint32_t active_state) noexcept
    {
        const std::optional<std::uint32_t> mask =
            winning_mask(active_state, std::span<const std::uint32_t>(table.masks.data(), entry_count(table)));
        return mask ? cycle_group_entry(table, *mask) : -1;
    }

    int cycle_group_entry(const StateBindingTable &table, std::uint32_t mask) noexcept
    {
        const CyclePick *pick = find_pick(table.cycle_picks, mask);
        int last = -1;
        for (std::size_t i = 0; i < entry_count(table); ++i)
        {
            if (table.masks[i] != mask)
            {
                continue;
            }
            if (pick != nullptr && table.presets[i].name == pick->preset)
            {
                return static_cast<int>(i);
            }
            last = static_cast<int>(i);
        }
        return last;
    }

    std::vector<std::size_t> cycle_group_members(const StateBindingTable &table, std::uint32_t mask)
    {
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < entry_count(table); ++i)
        {
            if (table.masks[i] == mask)
            {
                members.push_back(i);
            }
        }
        return members;
    }

    void publish_table(StateBindingTable table, const std::function<void(std::vector<CyclePick> &)> &edit_picks)
    {
        // Only the carried-over picks depend on the live snapshot, so a lost compare-exchange redoes just them. The
        // exchange takes a separate owner of `next`, and a failed one drops it unpublished, which leaves `next`
        // owned here alone and safe to rewrite.
        const auto next = std::make_shared<StateBindingTable>(std::move(table));
        std::shared_ptr<const StateBindingTable> current = s_table.load(std::memory_order_acquire);
        do
        {
            next->cycle_picks = current ? current->cycle_picks : std::vector<CyclePick>{};
            edit_picks(next->cycle_picks);
        } while (!s_table.compare_exchange_weak(current, std::shared_ptr<const StateBindingTable>(next),
                                                std::memory_order_acq_rel, std::memory_order_acquire));
    }

    std::shared_ptr<const StateBindingTable> published_table() noexcept
    {
        return s_table.load(std::memory_order_acquire);
    }

    void set_cycle_pick(std::uint32_t mask, std::string_view preset)
    {
        republish_picks(
            [mask, preset](const StateBindingTable &, std::vector<CyclePick> &picks) -> bool
            {
                assign_pick(picks, mask, preset);
                return true;
            });
    }

    std::optional<CycleStep> step_cycle(int step, std::uint32_t state)
    {
        std::optional<CycleStep> outcome;
        republish_picks(
            [&outcome, step, state](const StateBindingTable &table, std::vector<CyclePick> &picks) -> bool
            {
                const int in_use = resolve_active_entry(table, state);
                if (in_use < 0)
                {
                    outcome.reset();
                    return false;
                }
                const std::uint32_t mask = table.masks[static_cast<std::size_t>(in_use)];
                const std::vector<std::size_t> members = cycle_group_members(table, mask);
                // The entry in use is a member of its own group, so the search always finds it.
                const int position = static_cast<int>(
                    std::find(members.begin(), members.end(), static_cast<std::size_t>(in_use)) - members.begin());
                const int count = static_cast<int>(members.size());
                if (count < 2)
                {
                    outcome = CycleStep{
                        .mask = mask,
                        .preset = table.presets[static_cast<std::size_t>(in_use)].name,
                        .position = 1,
                        .count = 1,
                    };
                    return false;
                }

                // Wrap in both directions: the double modulo keeps a backward step from the first member positive.
                const int target = ((position + step) % count + count) % count;
                const std::string &name = table.presets[members[static_cast<std::size_t>(target)]].name;
                assign_pick(picks, mask, name);
                outcome = CycleStep{
                    .mask = mask,
                    .preset = name,
                    .position = target + 1,
                    .count = count,
                };
                return true;
            });
        return outcome;
    }

    void resolve_and_apply(uint32_t state, float delta_seconds) noexcept
    {
        LiveSettings &cfg = settings();

        const std::shared_ptr<const StateBindingTable> table = s_table.load(std::memory_order_acquire);
        if (!table)
            return;

        const CameraPreset &target = select_target(*table, state);

        // Frame-rate-independent preset blend. A SINGLE exponential low-pass starts at full speed and decays,
        // so transitions lurch off the mark (no ease-in). Cascading TWO low-passes makes a critically-damped
        // (no-overshoot) S-curve: the second stage smooths the sharp start of the first, so the camera eases IN
        // and out. Snap on the first frame and after a suppression gap so the view does not ease across stale
        // state. The 1.6x rate compensates for the 2-stage's slower rise so PresetBlendSpeed keeps its feel.
        const float speed = cfg.preset_blend_speed.load(std::memory_order_relaxed);
        if (!s_have_applied || s_snap_next || speed <= 0.0f)
        {
            s_applied = target;
            s_stage1 = target;
            s_have_applied = true;
            s_snap_next = false;
        }
        else
        {
            const float dt = (delta_seconds > 0.0f && delta_seconds < 1.0f) ? delta_seconds : 0.0f;
            const float alpha = 1.0f - std::exp(-speed * 1.6f * dt);
            ease_toward(s_stage1, target, alpha);    // stage 1: low-pass the target
            ease_toward(s_applied, s_stage1, alpha); // stage 2: low-pass stage 1 -> ease-in and out
        }

        apply_to_live(s_applied, cfg);
    }

    void ease_out_zoom_offset(float offset) noexcept
    {
        if (!s_have_applied)
        {
            return;
        }
        // Both stages move by the same amount, so the two-stage ease keeps its shape and glides from the zoomed
        // distance.
        s_applied.follow_distance += offset;
        s_stage1.follow_distance += offset;
    }

    void reset_transition() noexcept
    {
        s_snap_next = true;
    }

} // namespace TPVCamera::Presets
