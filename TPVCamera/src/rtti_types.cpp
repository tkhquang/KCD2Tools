/**
 * @file rtti_types.cpp
 * @brief Construction and publication of the cached class vtable identities.
 */

#include "rtti_types.hpp"
#include "constants.hpp"
#include "game_state.hpp"

#include "dmk_aliases.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace TPVCamera
{
    namespace
    {
        constexpr std::size_t CLASS_COUNT = static_cast<std::size_t>(GameClass::Count);

        /// MSVC decorated names follow the GameClass enumerator order.
        constexpr std::array<std::string_view, CLASS_COUNT> CLASS_NAMES = {{
            Constants::C_PLAYER_RTTI_NAME,
            Constants::CVIEW_RTTI_NAME,
            Constants::ANIMATED_CHARACTER_RTTI_NAME,
            Constants::C_CAMERA_COMBAT_RTTI_NAME,
            Constants::C_CAMERA_DIALOG_RTTI_NAME,
            Constants::C_CAMERA_MANAGER_RTTI_NAME,
            Constants::C_MISSILE_CONTROLLER_RTTI_NAME,
            Constants::C_ACTOR_MODEL_RTTI_NAME,
            Constants::C_CAMERA_OBSERVER_RTTI_NAME,
            Constants::CTIMER_RTTI_NAME,
            Constants::SGAME_OBJECT_EVENT_RTTI_NAME,
            Constants::ANIMATION_SET_RTTI_NAME,
        }};

        // A TypeIdentity is pinned, so a std::vector cannot hold one: a reallocation has to move its
        // elements. A std::deque never moves an element it already holds.
        std::deque<DMK::rtti::TypeIdentity> s_class_types;
        std::deque<DMK::rtti::TypeIdentity> s_minigame_types;

        // Only initialization takes this lock. Published tables stay immutable for query threads.
        std::mutex s_init_mutex;
        Region s_image{};

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
        [[nodiscard]] bool answer(
            const std::deque<DMK::rtti::TypeIdentity> &table,
            std::size_t index,
            std::uintptr_t vtable,
            std::string_view mangled
        ) noexcept
        {
            if (vtable == 0)
            {
                return false;
            }
            if (s_ready.load(std::memory_order_acquire))
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
        const std::lock_guard init_lock(s_init_mutex);
        if (!image.base || image.size == 0)
        {
            throw std::invalid_argument("RTTI initialization requires a nonempty game image");
        }
        if (s_image.base && (s_image.base != image.base || s_image.size != image.size))
        {
            throw std::invalid_argument("RTTI identities are bound to a different game image range");
        }
        if (s_ready.load(std::memory_order_acquire))
        {
            return;
        }
        s_image = image;
        // An allocation failure can leave an unpublished prefix. Resume that prefix without replacement or duplication.
        for (std::size_t i = s_class_types.size(); i < CLASS_COUNT; ++i)
        {
            s_class_types.emplace_back(CLASS_NAMES[i], image);
        }
        for (std::size_t i = s_minigame_types.size(); i < k_minigames.size(); ++i)
        {
            s_minigame_types.emplace_back(k_minigames[i].rtti_name, image);
        }

        // Resolve identities before publication because a cold identity scans the whole image.
        // Each worker owns a distinct identity. The caller also drains work if thread creation fails.
        // Unresolved identities use direct RTTI and retry under the library's cooldown.
        std::vector<const DMK::rtti::TypeIdentity *> identities;
        std::vector<std::string_view> names;
        for (std::size_t i = 0; i < CLASS_COUNT; ++i)
        {
            identities.push_back(&s_class_types[i]);
            names.push_back(CLASS_NAMES[i]);
        }
        for (std::size_t i = 0; i < k_minigames.size(); ++i)
        {
            identities.push_back(&s_minigame_types[i]);
            names.push_back(k_minigames[i].rtti_name);
        }
        std::vector<char> resolved(identities.size(), 0);
        std::atomic<std::size_t> next{0};
        const auto drain = [&identities, &resolved, &next]() noexcept
        {
            for (std::size_t i = next.fetch_add(1); i < identities.size(); i = next.fetch_add(1))
            {
                resolved[i] = identities[i]->vtable().has_value() ? 1 : 0;
            }
        };
        {
            constexpr unsigned max_warm_threads = 8;
            const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, max_warm_threads) - 1;
            std::vector<std::jthread> pool;
            pool.reserve(workers);
            try
            {
                for (unsigned k = 0; k < workers; ++k)
                {
                    pool.emplace_back(drain);
                }
            }
            catch (const std::system_error &)
            {
                // Fewer workers than asked for: the calling thread drains whatever they leave.
            }
            drain();
        }

        std::size_t resolved_count = 0;
        for (std::size_t i = 0; i < identities.size(); ++i)
        {
            if (resolved[i] != 0)
            {
                ++resolved_count;
            }
            else
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "RTTI: {} did not resolve at init", names[i]);
            }
        }
        (void)DMK::log()
            .try_log(DMK::LogLevel::Info, "RTTI: {}/{} class identities resolved", resolved_count, identities.size());

        s_ready.store(true, std::memory_order_release);
    }

    bool vtable_is(GameClass klass, std::uintptr_t vtable) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(klass);
        if (index >= CLASS_COUNT)
        {
            return false;
        }
        return answer(s_class_types, index, vtable, CLASS_NAMES[index]);
    }

    std::optional<std::uintptr_t> class_vtable(GameClass klass) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(klass);
        if (index >= CLASS_COUNT || !s_ready.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }
        if (const std::optional<Address> primary = s_class_types[index].vtable(); primary.has_value())
        {
            return primary->raw();
        }
        return std::nullopt;
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
