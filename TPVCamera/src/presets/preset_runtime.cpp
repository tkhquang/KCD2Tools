/**
 * @file preset_runtime.cpp
 * @brief Main-thread preset resolver: state -> target preset -> eased apply to live settings.
 */

#include "preset_runtime.hpp"
#include "camera/blend.hpp"
#include "config.hpp"
#include "game_state.hpp"
#include "switch_log.hpp"

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

        // Written only by compare-exchange (publish_table, republish_picks), read by the main thread via an atomic
        // shared_ptr load each frame (a spinlock-guarded refcount bump on MSVC, no heap allocation).
        std::atomic<std::shared_ptr<const StateBindingTable>> s_table{nullptr};

        // Main-thread-only transition state (resolve_and_apply / reset_transition run only there): the two-stage
        // preset blend, whose applied stage is the live preset.
        Spring<CameraPreset> s_blend;
        bool s_have_applied = false;
        bool s_snap_next = false;

        // Main-thread switch tracking for the switch ring: the target resolved last, and how much of the framing
        // change of the last switch the blend has still to close.
        std::array<char, 48> s_target_name{};
        std::uint32_t s_target_mask = 0;
        bool s_target_pinned = false;
        // The address of the table the target came from, compared only to label a republish (a cycle pick or an
        // edit), never dereferenced.
        std::uintptr_t s_target_table = 0;
        float s_settle_from = 0.0f;
        float s_settle_age = 0.0f;
        bool s_settle_pending = false;

        // A framing change smaller than this (meters, or tens of degrees) settles at once.
        constexpr float k_settle_min_change = 1e-3f;
        // Collision turns off only once the blend into a preset without it is this close to its framing (meters, or
        // tens of degrees, as framing_error() counts them), and s_collision_latched holds it on until then: a quick
        // switch away and back never releases the camera from a wall only to pull it in again. Main thread only.
        constexpr float k_collision_latch_settle = 1e-3f;
        bool s_collision_latched = true;
        // Follow angles count in units of this many degrees against the meters of the distance and offsets.
        constexpr float k_settle_degrees_per_meter = 10.0f;

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

        /** @brief The preset the blend eases toward, the bind mask that selected it, and whether it is the pin. */
        struct Target
        {
            const CameraPreset *preset = nullptr;
            std::uint32_t mask = 0;
            bool pinned = false;
        };

        /**
         * @brief Resolves the active preset target for @p state: the editing pin wins, otherwise the entry
         *        resolve_active_entry picks.
         * @details Falls back to a neutral preset only if the table is empty, which cannot happen once the store
         *          has published since DEFAULT is always bindable.
         */
        [[nodiscard]] Target select_target(const StateBindingTable &table, std::uint32_t state) noexcept
        {
            if (table.has_pin)
            {
                return Target{
                    .preset = &table.pinned,
                    .pinned = true,
                };
            }
            const int index = resolve_active_entry(table, state);
            if (index >= 0)
            {
                return Target{
                    .preset = &table.presets[static_cast<std::size_t>(index)],
                    .mask = table.masks[static_cast<std::size_t>(index)],
                };
            }
            static const CameraPreset s_fallback{};
            return Target{
                .preset = &s_fallback,
            };
        }

        /**
         * @brief How far @p applied frames the view from @p target: the follow distance, offsets and eye height in
         *        meters, plus the follow angles in tens of degrees.
         */
        [[nodiscard]] float framing_error(const CameraPreset &applied, const CameraPreset &target) noexcept
        {
            return std::fabs(applied.follow_distance - target.follow_distance) +
                   std::fabs(applied.offset_up - target.offset_up) +
                   std::fabs(applied.offset_right - target.offset_right) +
                   std::fabs(applied.eye_height - target.eye_height) +
                   (std::fabs(applied.follow_yaw - target.follow_yaw) +
                    std::fabs(applied.follow_pitch - target.follow_pitch)) /
                       k_settle_degrees_per_meter;
        }

        /**
         * @brief Records a switch when the target preset differs from the one resolved last, with its cause, and
         *        starts measuring how long the blend takes to settle on it.
         * @param snap Whether this frame snaps to the target instead of easing.
         */
        void note_target(const StateBindingTable &table, const Target &target, bool snap) noexcept
        {
            const std::string_view name = target.preset->name;
            const std::string_view last(s_target_name.data());
            const bool renamed = name.substr(0, s_target_name.size() - 1) != last;
            if (s_have_applied && !renamed && target.pinned == s_target_pinned)
            {
                s_target_mask = target.mask;
                s_target_table = reinterpret_cast<std::uintptr_t>(&table);
                return;
            }
            const char *cause = !s_have_applied                    ? "start"
                                : target.pinned != s_target_pinned ? (target.pinned ? "pin" : "unpin")
                                : s_snap_next                      ? "resume"
                                : target.mask != s_target_mask     ? "state"
                                : reinterpret_cast<std::uintptr_t>(&table) != s_target_table ? "republish"
                                                                                             : "state";
            SwitchEvent event{
                .kind = SwitchKind::Preset,
                .mask = target.mask,
                .hold_s = target.mask != s_target_mask ? last_debounce_wait_seconds() : 0.0f,
                .expected_settle_s = snap ? 0.0f : target.preset->blend_in_seconds * (k_spring_t95 / k_spring_t99),
            };
            copy_switch_text(event.from, s_have_applied ? last : std::string_view("-"));
            copy_switch_text(event.to, name);
            copy_switch_text(event.reason, cause);
            record_switch(event);

            copy_switch_text(s_target_name, name);
            s_target_mask = target.mask;
            s_target_pinned = target.pinned;
            s_target_table = reinterpret_cast<std::uintptr_t>(&table);
            s_settle_from = s_have_applied ? framing_error(s_blend.applied, *target.preset) : 0.0f;
            s_settle_age = 0.0f;
            s_settle_pending = true;
            if (snap || s_settle_from < k_settle_min_change)
            {
                s_settle_pending = false;
                note_switch_settled(0.0f);
            }
        }

        /** @brief Advances the settle measurement of the last switch by @p dt and reports it once 95% is closed. */
        void track_settle(const CameraPreset &target, float dt) noexcept
        {
            if (!s_settle_pending)
            {
                return;
            }
            s_settle_age += dt;
            if (framing_error(s_blend.applied, target) <= 0.05f * s_settle_from)
            {
                s_settle_pending = false;
                note_switch_settled(s_settle_age);
            }
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

        const Target resolved = select_target(*table, state);
        const CameraPreset &target = *resolved.preset;

        // Frame-rate-independent preset blend through the two-stage filter (see Spring), settling into the target
        // in its own blend_in_seconds. Snap on the first frame and after a suppression gap so the view does not ease
        // across stale state, and into a preset whose blend time is 0.
        const bool snap = !s_have_applied || s_snap_next || target.blend_in_seconds <= 0.0f;
        const float dt = (delta_seconds > 0.0f && delta_seconds < 1.0f) ? delta_seconds : 0.0f;
        if (dt > 0.0f)
        {
            count_switch_frame();
        }
        note_target(*table, resolved, snap);
        if (snap)
        {
            s_blend.snap(target);
            s_have_applied = true;
            s_snap_next = false;
        }
        else
        {
            s_blend.step(target, spring_alpha(target.blend_in_seconds, dt));
        }
        track_settle(target, dt);

        apply_to_live(s_blend.applied, cfg);
        s_collision_latched =
            target.enable_collision ||
            (s_collision_latched && framing_error(s_blend.applied, target) > k_collision_latch_settle);
        cfg.enable_collision.store(s_collision_latched, std::memory_order_relaxed);
    }

    void ease_out_zoom_offset(float offset) noexcept
    {
        if (!s_have_applied)
        {
            return;
        }
        // Both stages move by the same amount, so the two-stage ease keeps its shape and glides from the zoomed
        // distance.
        s_blend.applied.follow_distance += offset;
        s_blend.stage1.follow_distance += offset;
    }

    void reset_transition() noexcept
    {
        s_snap_next = true;
    }

} // namespace TPVCamera::Presets
