/**
 * @file preset_runtime.cpp
 * @brief Render-thread preset resolver: state -> target preset -> eased apply to live settings.
 */

#include "preset_runtime.hpp"
#include "config.hpp"
#include "game_state.hpp"

#include <atomic>
#include <bit>
#include <cmath>

namespace TPVCamera::Presets
{
namespace
{

// Published by the UI/init thread, read by the render thread via an atomic shared_ptr load each frame
// (a spinlock-guarded refcount bump on MSVC, no heap allocation).
std::atomic<std::shared_ptr<const StateBindingTable>> g_table{nullptr};

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

/// Equal-specificity tiebreak weight. Each framing bit gets a distinct power of two, so any mask's summed
/// weight is unique (a bitset) and a tie resolves by the highest-priority bit present. Priority, high ->
/// low: Aiming > Crouch > Combat > Mount > Lying > Sitting > Kneel > Cart. The stance bits (Mount, Crouch,
/// Lying, Sitting, Kneel, Cart) are mutually exclusive in practice, so their order only matters versus the
/// orthogonal Combat / Aiming bits. resolve_active_binding scales popcount above this sum (which is < 256),
/// so specificity still dominates.
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

/// Resolves the active preset target for @p state: the editing pin wins, otherwise the most-specific
/// bound preset (see resolve_active_binding). Falls back to a neutral preset only if the table is empty
/// (pre-publish), which cannot happen once a table is published since DEFAULT is always bindable.
[[nodiscard]] const CameraPreset &select_target(const StateBindingTable &table, std::uint32_t state) noexcept
{
    if (table.has_pin)
    {
        return table.pinned;
    }
    const int index = resolve_active_binding(state, table.masks);
    if (index >= 0 && static_cast<std::size_t>(index) < table.presets.size())
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
    const auto append = [&](GameState bit, const char *token) {
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

int resolve_active_binding(std::uint32_t active_state, std::span<const std::uint32_t> masks) noexcept
{
    int best = -1;
    std::uint32_t best_score = 0;
    for (std::size_t i = 0; i < masks.size(); ++i)
    {
        const std::uint32_t mask = masks[i];
        // The preset matches only when EVERY bit it requires is active.
        if ((mask & active_state) != mask)
        {
            continue;
        }
        // Specificity (bit count) dominates; the priority weight breaks equal-specificity ties between
        // DIFFERENT masks. On IDENTICAL masks the '>=' keeps the LATER entry, so a user preset (published
        // after the built-ins) overrides a built-in bound to the same states.
        const std::uint32_t score = static_cast<std::uint32_t>(std::popcount(mask)) * 256u + bind_priority_weight(mask);
        if (best < 0 || score >= best_score)
        {
            best = static_cast<int>(i);
            best_score = score;
        }
    }
    return best;
}

void publish_table(std::shared_ptr<const StateBindingTable> table) noexcept
{
    g_table.store(std::move(table), std::memory_order_release);
}

void resolve_and_apply(uint32_t state, float delta_seconds) noexcept
{
    LiveSettings &cfg = settings();

    const std::shared_ptr<const StateBindingTable> table = g_table.load(std::memory_order_acquire);
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

void reset_transition() noexcept
{
    s_snap_next = true;
}

} // namespace TPVCamera::Presets
