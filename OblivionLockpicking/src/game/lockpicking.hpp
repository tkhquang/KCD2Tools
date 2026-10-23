/**
 * @file game/lockpicking.hpp
 * @brief Typed access to a live C_LockPicking session, the vanilla functions the mod calls on it, and the console
 *        services of the cues and the hints.
 *
 * A session function takes the session pointer the game passed to C_LockPicking::Update and is valid only inside that
 * call, or on a session the caller proved is still the live one. A virtual call goes through a vtable matched to its
 * RTTI type first, so a shifted layout on an unknown build fails the call instead of a jump through the wrong table.
 * The one exception is the console variable that the checked console returns, called through its own vtable. Fields
 * of an object reached from a checked object are trusted.
 */
#ifndef OBLIVIONLP_GAME_LOCKPICKING_HPP
#define OBLIVIONLP_GAME_LOCKPICKING_HPP

#include "anchors.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace OblivionLockpicking::game
{
    /** @brief Stores the resolved anchors. Call once before any other function here. */
    void bind(const Anchors &anchors) noexcept;

    /** @brief Forgets the anchors. Every later call is a no-op that reports failure. */
    void unbind() noexcept;

    /** @brief The bound anchors, or nullptr before bind() and after unbind(). */
    [[nodiscard]] const Anchors *bound_anchors() noexcept;

    /**
     * @brief Reads the field at @p offset of @p object without a guard.
     * @details @p object must be live for the current frame: the session the game handed to the detour, gEnv, or an
     *          object whose vtable passed its RTTI check.
     */
    template <class T> [[nodiscard]] T read(const void *object, std::ptrdiff_t offset) noexcept
    {
        return DMK::memory::unchecked::read<T>(DMK::Address{object}.offset(offset));
    }

    /** @brief Writes the field at @p offset of @p object, under the same liveness rule as read(). */
    template <class T> void write(void *object, std::ptrdiff_t offset, const T &value) noexcept
    {
        std::memcpy(static_cast<std::byte *>(object) + offset, &value, sizeof(T));
    }

    /**
     * @brief The vtable pointer of @p object, or 0 when the pointer is null, implausible or unreadable.
     * @details The read is guarded, so a stale or foreign pointer fails its RTTI check and never faults the game.
     */
    [[nodiscard]] inline std::uintptr_t vtable_of(const void *object) noexcept
    {
        const DMK::Address address{object};
        if (!DMK::memory::is_plausible_ptr(address))
        {
            return 0;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(address);
        return vtable ? *vtable : 0;
    }

    /** @brief Slot @p index of @p vtable, a vtable that passed its RTTI check and its slot pin. */
    [[nodiscard]] inline std::uintptr_t slot(std::uintptr_t vtable, std::size_t index) noexcept
    {
        return DMK::memory::unchecked::read<std::uintptr_t>(
            DMK::Address{vtable}.offset(static_cast<std::ptrdiff_t>(index * sizeof(std::uintptr_t)))
        );
    }

    /** @brief The session's state byte. */
    [[nodiscard]] inline constants::LockState state(const void *session) noexcept
    {
        return static_cast<constants::LockState>(read<std::uint8_t>(session, constants::LP_STATE_OFFSET));
    }

    /** @brief True in the states whose Update body searches and turns the lock (3, 4, 5). */
    [[nodiscard]] inline bool is_play_state(constants::LockState value) noexcept
    {
        return value == constants::LockState::SpotSearching || value == constants::LockState::LockRotating ||
               value == constants::LockState::DryRun;
    }

    /** @brief True when @p session is a C_LockPicking whose owner is the player. */
    [[nodiscard]] bool is_player_session(const void *session) noexcept;

    /** @brief The owner's C_Soul, or nullptr. Requires is_player_session(). */
    [[nodiscard]] void *player_soul(const void *session) noexcept;

    /**
     * @brief @p soul's Thievery level with its buffs (0 to 30), as the lockpicking XP code reads it.
     * @return The level, or std::nullopt when @p soul is not a C_Soul, its anchor did not resolve or the level is not
     *         finite.
     */
    [[nodiscard]] std::optional<float> thievery_level(void *soul) noexcept;

    /**
     * @brief The game's lockpicking noise level for a cursor distance, with every noise buff applied.
     * @details The call makes the same C_RPGMinigames slot 29 query as UpdateLock for a lock that turns:
     *          min + (1 - min) * clamp(distance * k)^2, passed through the soul's buff modifiers.
     * @return The level, or 0 when the RPG object or the soul is unavailable.
     */
    [[nodiscard]] float lockpick_noise_level(void *soul, float distance) noexcept;

    /**
     * @brief Emits one frame of lockpicking noise at the lock through the session's hearing emitter.
     * @details The call writes @p level into the emitter and runs the emitter step with @p dt, as Update does after
     *          UpdateLock. Vanilla resets the level to zero on its next frame without a turn, so no noise lingers.
     */
    void emit_noise(void *session, float level, float dt) noexcept;

    /**
     * @brief Ends the failure @p session plays now instead of at the end of its hand animation.
     * @details The call runs what the end of the animation runs: fail XP, the fail-open roll, the reset and the return
     *          to play, or the end of the minigame without a pick. The game does the same for an empty hand. The call
     *          then finishes the animation, or drops it before it starts, so the hands stay in or go back to idle. Call
     *          only in the failure state.
     * @return False when the anchors are absent. Nothing changes then.
     */
    bool finish_failure(void *session) noexcept;

    /** @brief The vanilla lockpicking Flash element (LockPicking.gfx) of @p session, or nullptr when unavailable. */
    [[nodiscard]] void *vanilla_view_element(const void *session) noexcept;

    /** @brief The lock's sounds, one per tumbler event. */
    enum class Cue : std::uint8_t
    {
        /// The pick pushes a tumbler up.
        Push,
        /// A slow tumbler reaches the top: the lower click that announces a long pause.
        TopSlow,
        /// A fast tumbler reaches the top: a higher, quicker click.
        TopFast,
        /// A slow tumbler leaves the top and slides down.
        Fall,
        /// A tumbler lands back at rest.
        Land,
        /// A tumbler is set.
        Set,
    };

    /**
     * @brief Looks for SoundFramework's sfw_play console command, which decides where the cues go until the next call.
     * @param main_thread True when the caller runs on the game's main thread, the only thread for the console. False
     *        skips the lookup and leaves the cues on the vanilla click.
     * @return True when SoundFramework plays the cues.
     */
    bool detect_sound_framework(bool main_thread) noexcept;

    /**
     * @brief The id of the game's main thread, the thread that owns the game window.
     * @return The id, or 0 while the window does not exist yet. Each call looks again until a call finds it.
     * @note Any thread.
     */
    [[nodiscard]] std::uint32_t main_thread_id() noexcept;

    /**
     * @brief The device the game's own button hints show, from its wh_ui_InputDevice console variable.
     * @return 1 for keyboard and mouse, 2 for an Xbox controller, 16 for a PlayStation controller, or 0 when the
     *         console or the variable is unavailable.
     * @note Main thread only, as every console call.
     */
    [[nodiscard]] int hint_device_code() noexcept;

    /**
     * @brief Plays @p cue.
     * @details With SoundFramework, the cue plays its pack sound kcd2_oblivionlockpicking_<cue> through sfw_play.
     *          Without it, a tumbler at the top plays the vanilla sweet-spot click at the lock through the session's
     *          own PlaySound, and the other cues are silent.
     */
    void play_cue(void *session, Cue cue) noexcept;

    /** @brief Plays the game's pick break at the lock, the sound its failure animation plays. */
    void play_break_sound(void *session) noexcept;
} // namespace OblivionLockpicking::game

#endif // OBLIVIONLP_GAME_LOCKPICKING_HPP
