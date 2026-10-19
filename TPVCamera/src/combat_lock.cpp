/**
 * @file combat_lock.cpp
 * @brief Reads and logs the player's combat lock-on (see combat_lock.hpp).
 */

#include "combat_lock.hpp"
#include "constants.hpp"
#include "game_structures.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>

namespace TPVCamera
{
    namespace
    {
        /**
         * @brief Reads the object pointer at @p slot and confirms its class by its vtable.
         * @return The object, or 0 when the slot does not read, holds an implausible pointer, or names another class.
         */
        [[nodiscard]] std::uintptr_t read_object(std::uintptr_t slot, GameClass klass) noexcept
        {
            const auto object = DMK::memory::read<std::uintptr_t>(DMK::Address{slot});
            if (!object || !DMK::memory::is_plausible_ptr(DMK::Address{*object}))
            {
                return 0;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*object});
            if (!vtable || !vtable_is(klass, *vtable))
            {
                return 0;
            }
            return *object;
        }

        /**
         * @brief Reads an entity-module actor's health the way C_Actor::IsDead does.
         * @details The soul's health when the actor has a soul (its class confirmed), the actor's own health otherwise.
         * @return The health, or std::nullopt when it does not read.
         */
        [[nodiscard]] std::optional<float> read_actor_health(std::uintptr_t human) noexcept
        {
            const auto soul_slot =
                DMK::memory::read<std::uintptr_t>(DMK::Address{human + Constants::HUMAN_SOUL_OFFSET});
            if (!soul_slot)
            {
                return std::nullopt;
            }
            std::uintptr_t health_address = human + Constants::HUMAN_HEALTH_OFFSET;
            if (*soul_slot != 0)
            {
                const std::uintptr_t soul = read_object(human + Constants::HUMAN_SOUL_OFFSET, GameClass::Soul);
                if (soul == 0)
                {
                    return std::nullopt;
                }
                health_address = soul + Constants::SOUL_HEALTH_OFFSET;
            }
            const auto health = DMK::memory::read<float>(DMK::Address{health_address});
            if (!health || !std::isfinite(*health))
            {
                return std::nullopt;
            }
            return *health;
        }

        /** @brief An entity's world position and its forward's heading. */
        struct EntityPose
        {
            float x;
            float y;
            float z;
            float heading;
        };

        /**
         * @brief Reads an entity-module actor's CEntity world transform.
         * @details The CEntity is the C_Human member the player chain heals (C_Player inherits it at the same offset).
         *          Forward is the matrix's column 1, the translation its column 3.
         */
        [[nodiscard]] std::optional<EntityPose> read_entity_pose(std::uintptr_t human) noexcept
        {
            const auto entity = DMK::memory::read<std::uintptr_t>(
                DMK::Address{human + static_cast<std::uintptr_t>(runtime_offsets().c_player_entity.load().value)});
            if (!entity || !DMK::memory::is_plausible_ptr(DMK::Address{*entity}))
            {
                return std::nullopt;
            }
            const auto world = DMK::memory::read<GameStructures::Matrix34f>(
                DMK::Address{*entity}.offset(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER));
            if (!world)
            {
                return std::nullopt;
            }
            const float x = world->m[0][3];
            const float y = world->m[1][3];
            const float z = world->m[2][3];
            const float fx = world->m[0][1];
            const float fy = world->m[1][1];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(fx) ||
                !std::isfinite(fy) || fx * fx + fy * fy < 1e-6f)
            {
                return std::nullopt;
            }
            return EntityPose{x, y, z, std::atan2(-fx, fy)};
        }

        /// The angle from @p from to @p to, wrapped into [-pi, pi], in degrees.
        [[nodiscard]] float heading_gap_degrees(float from, float to) noexcept
        {
            return std::remainder(to - from, 2.0f * std::numbers::pi_v<float>) * (180.0f / std::numbers::pi_v<float>);
        }

        /// Formats a timer as the seconds it has left on the game clock, or "-" when unarmed.
        [[nodiscard]] std::string format_timer(std::int64_t deadline, std::int64_t game_clock)
        {
            if (deadline <= Constants::COMBAT_TIMER_UNARMED)
            {
                return "-";
            }
            if (game_clock <= 0)
            {
                return "armed";
            }
            const double left = static_cast<double>(deadline - game_clock) / Constants::COMBAT_TIMER_TICKS_PER_SECOND;
            return std::format("{:.2f} s", left);
        }

        /// Formats the free-look bytes.
        [[nodiscard]] const char *format_free(bool mouse, bool pad) noexcept
        {
            if (mouse && pad)
            {
                return "mouse+pad";
            }
            if (mouse)
            {
                return "mouse";
            }
            return pad ? "pad" : "off";
        }

        /// Formats where the opponent is: distance, and how far the body and the camera are turned off him.
        [[nodiscard]] std::string format_geometry(const CombatLock &lock, std::optional<float> camera_heading)
        {
            if (lock.opponent == 0)
            {
                return "";
            }
            if (!lock.geometry)
            {
                return ", position unread";
            }
            std::string text = std::format(", {:.1f} m, body off {:.0f} deg", lock.opponent_distance,
                                           heading_gap_degrees(lock.body_heading, lock.opponent_heading));
            if (camera_heading.has_value())
            {
                text +=
                    std::format(", camera off {:.0f} deg", heading_gap_degrees(*camera_heading, lock.opponent_heading));
            }
            return text;
        }

        /**
         * @brief True when two readings differ in anything but the time a timer has left and the geometry.
         * @details The end-of-stance timer counts only as armed or not: a retarget holds it off by moving it every
         *          update (defer_kill_end_timer).
         */
        [[nodiscard]] bool differs(const CombatLock &a, const CombatLock &b) noexcept
        {
            const auto armed = [](std::int64_t deadline) { return deadline > Constants::COMBAT_TIMER_UNARMED; };
            return a.resolved != b.resolved || a.stance != b.stance || a.opponent != b.opponent ||
                   a.opponent_id != b.opponent_id || a.opponent_dead != b.opponent_dead ||
                   a.free_mouse != b.free_mouse || a.free_pad != b.free_pad || a.unlock_deadline != b.unlock_deadline ||
                   armed(a.end_deadline) != armed(b.end_deadline) || a.hard_lock != b.hard_lock;
        }

        /** @brief The player's combat objects, each class checked. */
        struct CombatChain
        {
            std::uintptr_t player;
            std::uintptr_t human;
            std::uintptr_t controller;
            std::uintptr_t input;
            std::uintptr_t state;
        };

        /**
         * @brief Walks context -> combat module -> combat player -> controller -> input, and the player's state block.
         * @param c_player The local C_Player, or 0. When set, the combat player must belong to it.
         */
        [[nodiscard]] std::optional<CombatChain> resolve_combat_chain(std::uintptr_t c_player) noexcept
        {
            const auto context_slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            if (!context_slot)
            {
                return std::nullopt;
            }
            // Context slot -> context object -> combat module (self-healed offset). The leaf is the module's address.
            const std::array<std::ptrdiff_t, 3> module_chain{0, runtime_offsets().context_combat_module.load().value,
                                                             0};
            const auto module_address =
                DMK::memory::walk(DMK::Address{reinterpret_cast<std::uintptr_t>(context_slot)}, module_chain);
            if (!module_address)
            {
                return std::nullopt;
            }
            const auto module_vtable = DMK::memory::read<std::uintptr_t>(*module_address);
            if (!module_vtable || !vtable_is(GameClass::CombatModule, *module_vtable))
            {
                return std::nullopt;
            }
            const std::uintptr_t module = module_address->raw();

            const std::uintptr_t player =
                read_object(module + Constants::COMBAT_MODULE_PLAYER_OFFSET, GameClass::CombatPlayer);
            if (player == 0)
            {
                return std::nullopt;
            }
            // The combat actor names its entity-module actor. It must be the local player the camera follows, so a
            // module that briefly holds another actor (a load, a possession) never steers the camera's decisions.
            const auto human =
                DMK::memory::read<std::uintptr_t>(DMK::Address{player + Constants::COMBAT_ACTOR_HUMAN_OFFSET});
            if (!human || !DMK::memory::is_plausible_ptr(DMK::Address{*human}) || (c_player != 0 && *human != c_player))
            {
                return std::nullopt;
            }
            const std::uintptr_t controller =
                read_object(player + Constants::COMBAT_PLAYER_CONTROLLER_OFFSET, GameClass::CombatPlayerController);
            if (controller == 0)
            {
                return std::nullopt;
            }
            const std::uintptr_t input =
                read_object(controller + Constants::COMBAT_CONTROLLER_INPUT_OFFSET, GameClass::CombatPlayerInput);
            if (input == 0)
            {
                return std::nullopt;
            }
            const auto state =
                DMK::memory::read<std::uintptr_t>(DMK::Address{player + Constants::COMBAT_ACTOR_STATE_OFFSET});
            if (!state || !DMK::memory::is_plausible_ptr(DMK::Address{*state}))
            {
                return std::nullopt;
            }
            return CombatChain{player, *human, controller, input, *state};
        }

        /** @brief Ang3 (radians) to Quat (x y z w), the engine's Quat::SetRotationXYZ (sub_18053F6C0). */
        [[nodiscard]] std::array<float, 4> quat_from_angles_xyz(float x, float y, float z) noexcept
        {
            const float sx = std::sin(x * 0.5f);
            const float cx = std::cos(x * 0.5f);
            const float sy = std::sin(y * 0.5f);
            const float cy = std::cos(y * 0.5f);
            const float sz = std::sin(z * 0.5f);
            const float cz = std::cos(z * 0.5f);
            return {cz * cy * sx - sz * sy * cx, cz * sy * cx + sz * cy * sx, sz * cy * cx - cz * sy * sx,
                    cx * cy * cz + sx * sy * sz};
        }

        /**
         * @brief The lock opponent's C_Human while the stance holds a dead one: the moment a retarget is for.
         * @return std::nullopt when the stance is off, the opponent is alive, or a read failed.
         */
        [[nodiscard]] std::optional<std::uintptr_t> dead_opponent_human(const CombatChain &chain) noexcept
        {
            const auto stance =
                DMK::memory::read<std::uint8_t>(DMK::Address{chain.state + Constants::COMBAT_STATE_STANCE_OFFSET});
            const auto opponent =
                DMK::memory::read<std::uintptr_t>(DMK::Address{chain.state + Constants::COMBAT_STATE_OPPONENT_OFFSET});
            if (!stance || *stance == 0 || !opponent || !DMK::memory::is_plausible_ptr(DMK::Address{*opponent}))
            {
                return std::nullopt;
            }
            const auto human =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*opponent + Constants::COMBAT_ACTOR_HUMAN_OFFSET});
            if (!human || !DMK::memory::is_plausible_ptr(DMK::Address{*human}))
            {
                return std::nullopt;
            }
            const std::optional<float> health = read_actor_health(*human);
            if (!health.has_value() || *health > 0.0f)
            {
                return std::nullopt;
            }
            return *human;
        }

        /** @brief A C_Human's CEntity, class-checked, or std::nullopt when it does not read as one. */
        [[nodiscard]] std::optional<std::uintptr_t> human_entity(std::uintptr_t human) noexcept
        {
            const auto entity = DMK::memory::read<std::uintptr_t>(
                DMK::Address{human + static_cast<std::uintptr_t>(runtime_offsets().c_player_entity.load().value)});
            if (!entity || !DMK::memory::is_plausible_ptr(DMK::Address{*entity}))
            {
                return std::nullopt;
            }
            const auto entity_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*entity});
            if (!entity_vtable || !vtable_is(GameClass::Entity, *entity_vtable))
            {
                return std::nullopt;
            }
            return *entity;
        }

        /**
         * @brief The entity of the combat target the stance holds, 0 when it holds none.
         * @return std::nullopt when the slot does not read or holds something other than a C_CombatTarget.
         */
        [[nodiscard]] std::optional<std::uintptr_t> combat_target_entity(const CombatChain &chain) noexcept
        {
            const auto target =
                DMK::memory::read<std::uintptr_t>(DMK::Address{chain.state + Constants::COMBAT_STATE_TARGET_OFFSET});
            if (!target)
            {
                return std::nullopt;
            }
            if (*target == 0)
            {
                return std::uintptr_t{0};
            }
            if (!DMK::memory::is_plausible_ptr(DMK::Address{*target}))
            {
                return std::nullopt;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*target});
            if (!vtable || !vtable_is(GameClass::CombatTarget, *vtable))
            {
                return std::nullopt;
            }
            const auto entity =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*target + Constants::COMBAT_TARGET_ENTITY_OFFSET});
            if (!entity)
            {
                return std::nullopt;
            }
            return *entity;
        }

        /** @brief A combat actor as the player at @p me sees him, or std::nullopt when his pose does not read. */
        [[nodiscard]] std::optional<CombatCandidate> read_candidate(const EntityPose &me, std::uintptr_t human) noexcept
        {
            const std::optional<EntityPose> them = read_entity_pose(human);
            if (!them.has_value())
            {
                return std::nullopt;
            }
            const float dx = them->x - me.x;
            const float dy = them->y - me.y;
            const auto id = DMK::memory::read<std::uint32_t>(DMK::Address{human + Constants::HUMAN_ENTITY_ID_OFFSET});
            const std::optional<float> health = read_actor_health(human);
            return CombatCandidate{
                .id = id ? *id : 0u,
                .human = human,
                .dead = health.has_value() && *health <= 0.0f,
                .distance = std::sqrt(dx * dx + dy * dy),
                .heading = std::atan2(-dx, dy),
            };
        }
    } // namespace

    CombatLock read_combat_lock(std::uintptr_t c_player) noexcept
    {
        CombatLock lock{};
        const std::optional<CombatChain> chain = resolve_combat_chain(c_player);
        if (!chain.has_value())
        {
            return lock;
        }
        const std::uintptr_t controller = chain->controller;
        const std::uintptr_t input = chain->input;
        const std::uintptr_t human = chain->human;

        const auto stance =
            DMK::memory::read<std::uint8_t>(DMK::Address{chain->state + Constants::COMBAT_STATE_STANCE_OFFSET});
        const auto opponent =
            DMK::memory::read<std::uintptr_t>(DMK::Address{chain->state + Constants::COMBAT_STATE_OPPONENT_OFFSET});
        const auto free_mouse =
            DMK::memory::read<std::uint8_t>(DMK::Address{input + Constants::COMBAT_INPUT_FREE_MOUSE_OFFSET});
        const auto free_pad =
            DMK::memory::read<std::uint8_t>(DMK::Address{input + Constants::COMBAT_INPUT_FREE_PAD_OFFSET});
        const auto unlock_deadline = DMK::memory::read<std::int64_t>(
            DMK::Address{controller + Constants::COMBAT_CONTROLLER_DELAYED_UNLOCK_OFFSET});
        const auto end_deadline =
            DMK::memory::read<std::int64_t>(DMK::Address{controller + Constants::COMBAT_CONTROLLER_DELAYED_END_OFFSET});
        if (!stance || !opponent || !free_mouse || !free_pad || !unlock_deadline || !end_deadline)
        {
            return lock;
        }
        // An opponent slot holding anything but null or a plausible pointer means the layout moved: report nothing.
        if (*opponent != 0 && !DMK::memory::is_plausible_ptr(DMK::Address{*opponent}))
        {
            return lock;
        }

        lock.resolved = true;
        lock.stance = *stance != 0;
        lock.opponent = *opponent;
        lock.free_mouse = *free_mouse != 0;
        lock.free_pad = *free_pad != 0;
        lock.unlock_deadline = *unlock_deadline;
        lock.end_deadline = *end_deadline;
        const auto hard =
            DMK::memory::read<std::uint8_t>(DMK::Address{controller + Constants::COMBAT_CONTROLLER_HARD_LOCK_OFFSET});
        lock.hard_lock = hard && *hard != 0;
        if (lock.opponent == 0)
        {
            return lock;
        }
        const auto opponent_human =
            DMK::memory::read<std::uintptr_t>(DMK::Address{lock.opponent + Constants::COMBAT_ACTOR_HUMAN_OFFSET});
        if (!opponent_human || !DMK::memory::is_plausible_ptr(DMK::Address{*opponent_human}))
        {
            return lock;
        }
        const auto id =
            DMK::memory::read<std::uint32_t>(DMK::Address{*opponent_human + Constants::HUMAN_ENTITY_ID_OFFSET});
        lock.opponent_id = id ? *id : 0u;
        if (const std::optional<float> health = read_actor_health(*opponent_human); health.has_value())
        {
            lock.opponent_health = *health;
            lock.opponent_dead = *health <= 0.0f;
        }
        const std::optional<EntityPose> me = read_entity_pose(human);
        const std::optional<EntityPose> them = read_entity_pose(*opponent_human);
        if (me.has_value() && them.has_value())
        {
            const float dx = them->x - me->x;
            const float dy = them->y - me->y;
            lock.opponent_distance = std::sqrt(dx * dx + dy * dy);
            if (lock.opponent_distance > 1e-3f)
            {
                lock.geometry = true;
                lock.opponent_position = {them->x, them->y, them->z};
                lock.opponent_heading = std::atan2(-dx, dy);
                lock.body_heading = me->heading;
            }
        }
        return lock;
    }

    bool set_combat_view_heading(std::uintptr_t c_player, float heading) noexcept
    {
        if (!std::isfinite(heading))
        {
            return false;
        }
        const std::optional<CombatChain> chain = resolve_combat_chain(c_player);
        if (!chain.has_value())
        {
            return false;
        }
        // Keep the input's pitch and roll: only the heading is handed over, the game raises or lowers the look itself.
        // A pitch outside the engine's look range means the layout moved, so nothing is written.
        const DMK::Address angles_address{chain->input + Constants::COMBAT_INPUT_VIEW_ANGLES_OFFSET};
        const auto angles = DMK::memory::read<std::array<float, 3>>(angles_address);
        if (!angles || !std::isfinite((*angles)[0]) || !std::isfinite((*angles)[1]) || std::fabs((*angles)[0]) > 1.6f)
        {
            return false;
        }
        const std::array<float, 3> turned{(*angles)[0], (*angles)[1], heading};
        const std::array<float, 4> quat = quat_from_angles_xyz(turned[0], turned[1], turned[2]);
        // write_in_place: a guarded per-frame data write that changes no page protection and fails closed on a stale
        // chain, the same primitive the look-controller writes use.
        return DMK::memory::write_in_place(angles_address, turned) &&
               DMK::memory::write_in_place(
                   DMK::Address{chain->input + Constants::COMBAT_INPUT_VIEW_ANGLES_RESET_OFFSET}, turned) &&
               DMK::memory::write_in_place(DMK::Address{chain->input + Constants::COMBAT_INPUT_VIEW_BASE_OFFSET},
                                           quat) &&
               DMK::memory::write_in_place(DMK::Address{chain->input + Constants::COMBAT_INPUT_VIEW_OFFSET}, quat);
    }

    bool clear_kill_end_timer(std::uintptr_t c_player) noexcept
    {
        const std::optional<CombatChain> chain = resolve_combat_chain(c_player);
        if (!chain.has_value())
        {
            return false;
        }
        const DMK::Address timer{chain->controller + Constants::COMBAT_CONTROLLER_DELAYED_END_OFFSET};
        const auto deadline = DMK::memory::read<std::int64_t>(timer);
        if (!deadline || *deadline <= Constants::COMBAT_TIMER_UNARMED)
        {
            return false;
        }
        return static_cast<bool>(
            DMK::memory::write_in_place(timer, static_cast<std::int64_t>(Constants::COMBAT_TIMER_UNARMED)));
    }

    std::size_t read_combat_candidates(std::uintptr_t c_player, std::span<CombatCandidate> out) noexcept
    {
        const std::optional<CombatChain> chain = resolve_combat_chain(c_player);
        if (!chain.has_value() || out.empty())
        {
            return 0;
        }
        const std::optional<EntityPose> me = read_entity_pose(chain->human);
        const auto selector = DMK::memory::read<std::uintptr_t>(
            DMK::Address{chain->controller + Constants::COMBAT_CONTROLLER_SELECTOR_OFFSET});
        if (!me.has_value() || !selector || !DMK::memory::is_plausible_ptr(DMK::Address{*selector}))
        {
            return 0;
        }
        const auto begin = DMK::memory::read<std::uintptr_t>(
            DMK::Address{*selector + Constants::COMBAT_SELECTOR_CANDIDATES_BEGIN_OFFSET});
        const auto end = DMK::memory::read<std::uintptr_t>(
            DMK::Address{*selector + Constants::COMBAT_SELECTOR_CANDIDATES_END_OFFSET});
        // A vector whose ends do not bracket a small, pointer-aligned span is not one: the layout moved.
        constexpr std::uintptr_t k_max_candidates = 64;
        if (!begin || !end || *end < *begin || (*end - *begin) % sizeof(std::uintptr_t) != 0 ||
            (*end - *begin) / sizeof(std::uintptr_t) > k_max_candidates)
        {
            return 0;
        }
        std::size_t count = 0;
        for (std::uintptr_t slot = *begin; slot < *end && count < out.size(); slot += sizeof(std::uintptr_t))
        {
            const auto actor = DMK::memory::read<std::uintptr_t>(DMK::Address{slot});
            if (!actor || !DMK::memory::is_plausible_ptr(DMK::Address{*actor}))
            {
                continue;
            }
            const auto human =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*actor + Constants::COMBAT_ACTOR_HUMAN_OFFSET});
            if (!human || !DMK::memory::is_plausible_ptr(DMK::Address{*human}))
            {
                continue;
            }
            if (const std::optional<CombatCandidate> candidate = read_candidate(*me, *human); candidate.has_value())
            {
                out[count++] = *candidate;
            }
        }
        return count;
    }

    std::optional<CombatCandidate> read_combat_actor(std::uintptr_t c_player, std::uintptr_t human,
                                                     std::uint32_t id) noexcept
    {
        if (id == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{human}) || !human_entity(human).has_value())
        {
            return std::nullopt;
        }
        const std::optional<CombatChain> chain = resolve_combat_chain(c_player);
        if (!chain.has_value())
        {
            return std::nullopt;
        }
        const std::optional<EntityPose> me = read_entity_pose(chain->human);
        if (!me.has_value())
        {
            return std::nullopt;
        }
        const std::optional<CombatCandidate> actor = read_candidate(*me, human);
        if (!actor.has_value() || actor->id != id)
        {
            return std::nullopt;
        }
        return actor;
    }

    std::optional<CombatRetarget> read_combat_retarget(std::uintptr_t human, std::uint32_t id) noexcept
    {
        // The combat update runs on a game thread where the local C_Player is not at hand. The chain's own class
        // checks stand in for it.
        const std::optional<CombatChain> chain = resolve_combat_chain(0);
        if (!chain.has_value())
        {
            return std::nullopt;
        }
        const std::optional<std::uintptr_t> held_human = dead_opponent_human(*chain);
        if (!held_human.has_value())
        {
            return std::nullopt;
        }
        const std::optional<std::uintptr_t> held = human_entity(*held_human);
        const std::optional<std::uintptr_t> target = combat_target_entity(*chain);
        if (!held.has_value() || !target.has_value())
        {
            return std::nullopt;
        }
        CombatRetarget retarget{
            .combat_player = chain->player,
            .held = *held,
            .target = *target,
        };
        if (id == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{human}))
        {
            return retarget;
        }
        // Still the actor picked from the lock-on's list, alive.
        const auto actor_id = DMK::memory::read<std::uint32_t>(DMK::Address{human + Constants::HUMAN_ENTITY_ID_OFFSET});
        const std::optional<float> health = read_actor_health(human);
        if (actor_id && *actor_id == id && health.has_value() && *health > 0.0f)
        {
            retarget.next = human_entity(human).value_or(0);
        }
        return retarget;
    }

    bool defer_kill_end_timer(std::int64_t until) noexcept
    {
        if (until <= Constants::COMBAT_TIMER_UNARMED)
        {
            return false;
        }
        const std::optional<CombatChain> chain = resolve_combat_chain(0);
        if (!chain.has_value())
        {
            return false;
        }
        const DMK::Address timer{chain->controller + Constants::COMBAT_CONTROLLER_DELAYED_END_OFFSET};
        const auto deadline = DMK::memory::read<std::int64_t>(timer);
        if (!deadline || *deadline <= Constants::COMBAT_TIMER_UNARMED || *deadline >= until)
        {
            return false;
        }
        return static_cast<bool>(DMK::memory::write_in_place(timer, until));
    }

    void log_combat_candidates(const CombatLock &lock, std::span<const CombatCandidate> candidates,
                               float camera_heading) noexcept
    {
        if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
        {
            return;
        }
        try
        {
            std::string line;
            for (const CombatCandidate &candidate : candidates)
            {
                if (candidate.dead || candidate.id == lock.opponent_id)
                {
                    continue;
                }
                line += std::format("{}id {} {:.1f} m camera off {:.0f} deg", line.empty() ? "" : "; ", candidate.id,
                                    candidate.distance, heading_gap_degrees(camera_heading, candidate.heading));
            }
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "Combat lock: living hostiles while it holds the dead opponent: {}, hard lock {}",
                                     line.empty() ? std::string{"none"} : line, lock.hard_lock ? "on" : "off");
        }
        catch (...)
        {
            // Formatting can throw std::bad_alloc. A diagnostic line is not worth an unwind of the frustum detour.
        }
    }

    void log_combat_lock(const CombatLock &lock, std::int64_t game_clock, std::optional<float> camera_heading) noexcept
    {
        static CombatLock s_last{};
        static bool s_logged = false;
        if (s_logged && !differs(lock, s_last))
        {
            return;
        }
        const bool was_resolved = s_last.resolved;
        s_last = lock;
        s_logged = true;
        if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
        {
            return;
        }
        try
        {
            if (!lock.resolved)
            {
                // Only the transition is worth a line: the chain is unresolved at the main menu and while loading.
                if (was_resolved)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Debug, "Combat lock: the combat module is not readable");
                }
                return;
            }
            const std::string opponent = lock.opponent == 0
                                             ? std::string{"none"}
                                             : std::format("{:#x} (id {}, {}, health {:.1f}{})", lock.opponent,
                                                           lock.opponent_id, lock.opponent_dead ? "dead" : "alive",
                                                           lock.opponent_health, format_geometry(lock, camera_heading));
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Combat lock: stance {}, opponent {}, hard lock {}, free look {}, delayed unlock {}, end after kill {}",
                lock.stance ? "on" : "off", opponent, lock.hard_lock ? "on" : "off",
                format_free(lock.free_mouse, lock.free_pad), format_timer(lock.unlock_deadline, game_clock),
                format_timer(lock.end_deadline, game_clock));
        }
        catch (...)
        {
            // Formatting can throw std::bad_alloc. A diagnostic line is not worth an unwind of the frustum detour.
        }
    }
} // namespace TPVCamera
