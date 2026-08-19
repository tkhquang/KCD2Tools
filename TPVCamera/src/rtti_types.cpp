/**
 * @file rtti_types.cpp
 * @brief Construction and publication of the cached class vtable identities.
 */

#include "rtti_types.hpp"
#include "constants.hpp"
#include "game_state.hpp"

#include "dmk_aliases.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>

namespace TPVCamera
{
    namespace
    {
        constexpr std::size_t k_class_count = static_cast<std::size_t>(GameClass::Count);

        /// MSVC decorated names, indexed by GameClass. The enumerator order IS this order.
        constexpr std::array<std::string_view, k_class_count> k_class_names = {{
            Constants::C_PLAYER_RTTI_NAME,
            Constants::CVIEW_RTTI_NAME,
            Constants::ANIMATED_CHARACTER_RTTI_NAME,
            Constants::C_CAMERA_COMBAT_RTTI_NAME,
            Constants::C_CAMERA_DIALOG_RTTI_NAME,
            Constants::C_MISSILE_CONTROLLER_RTTI_NAME,
            Constants::C_ACTOR_MODEL_RTTI_NAME,
        }};

        // A TypeIdentity is pinned, so a std::vector cannot hold one: a reallocation has to move its
        // elements. A std::deque never moves an element it already holds.
        std::deque<DMK::rtti::TypeIdentity> s_class_types;
        std::deque<DMK::rtti::TypeIdentity> s_minigame_types;

        // Published with release once both deques are complete, and read with acquire, so a render thread
        // either sees no table and takes the direct RTTI walk, or sees a complete one.
        std::atomic<bool> s_ready{false};

        /**
         * @brief Answers from the cached identity when it resolved, and from the RTTI walk otherwise.
         * @details Reads vtable() rather than calling TypeIdentity::matches(), because matches() reports a
         *          resolve failure and a genuine mismatch both as false. Telling them apart is what selects
         *          the fallback, and one vtable() call answers both questions for a single generation check.
         *          An unresolved sweep is not cached, but the library throttles the retry, so a class that is
         *          absent does not re-scan the image every frame.
         */
        [[nodiscard]] bool answer(const std::deque<DMK::rtti::TypeIdentity> &table, std::size_t index,
                                  std::uintptr_t vtable, std::string_view mangled) noexcept
        {
            if (vtable == 0)
            {
                return false;
            }
            if (s_ready.load(std::memory_order_acquire) && index < table.size())
            {
                if (const std::optional<Address> primary = table[index].vtable(); primary.has_value())
                {
                    return Address{vtable} == *primary;
                }
            }
            return DMK::rtti::vtable_is_type(Address{vtable}, mangled);
        }
    } // namespace

    void init_game_types(Region image)
    {
        for (const std::string_view mangled : k_class_names)
        {
            s_class_types.emplace_back(mangled, image);
        }
        for (const MinigameInfo &def : k_minigames)
        {
            s_minigame_types.emplace_back(def.rtti_name, image);
        }

        s_ready.store(true, std::memory_order_release);
    }

    bool vtable_is(GameClass klass, std::uintptr_t vtable) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(klass);
        if (index >= k_class_count)
        {
            return false;
        }
        return answer(s_class_types, index, vtable, k_class_names[index]);
    }

    bool minigame_vtable_is(std::size_t index, std::uintptr_t vtable) noexcept
    {
        if (index >= k_minigames.size())
        {
            return false;
        }
        return answer(s_minigame_types, index, vtable, k_minigames[index].rtti_name);
    }
} // namespace TPVCamera
