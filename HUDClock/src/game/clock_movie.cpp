/**
 * @file game/clock_movie.cpp
 * @brief The mod's Scaleform element through CFlashUI's element list and CFlashPlayer::Invoke.
 */

#include "game/clock_movie.hpp"
#include "constants.hpp"
#include "game/game_memory.hpp"

#include <windows.h>

#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>

namespace HUDClock::game
{
    namespace
    {
        /** @brief SFlashVarValue: a type tag, then an 8-byte value. Invoke converts each one to a GFx value. */
        struct FlashValue
        {
            /// The SFlashVarValue type tag.
            std::int32_t type{0};
            /// The value, read as the tag says.
            std::uint64_t value{0};
        };
        static_assert(sizeof(FlashValue) == 16 && offsetof(FlashValue, value) == 8);

        /// SFlashVarValue::eDouble.
        constexpr std::int32_t FLASH_DOUBLE = 5;
        /// SFlashVarValue::eConstStrPtr, which Invoke copies into the GFx value.
        constexpr std::int32_t FLASH_STRING = 7;

        using SetVisibleFn = void(__fastcall *)(void *element, bool visible);
        using IsVisibleFn = bool(__fastcall *)(void *element);
        using GetPlayerFn = void *(__fastcall *)(void *element, std::shared_ptr<void> *out);
        using InvokeFn = bool(__fastcall *)(
            void *player,
            const char *method,
            const FlashValue *args,
            unsigned count,
            FlashValue *result
        );

        /**
         * @brief The element's Flash player, held for the scope of one call.
         * @details GetFlashPlayer returns a std::shared_ptr through a hidden result pointer. It copy-constructs the
         *          element's own pointer into raw storage. The release of the copy goes through the game's control
         *          block, as the game drops its own copies.
         */
        class PlayerRef
        {
        public:
            PlayerRef(void *element, const Anchors &anchors) noexcept
            {
                const auto get_player = reinterpret_cast<GetPlayerFn>(
                    slot(anchors.flash_element_vtable, constants::ELEMENT_GET_PLAYER_SLOT)
                );
                get_player(element, reinterpret_cast<std::shared_ptr<void> *>(m_storage));
                m_player = std::launder(reinterpret_cast<std::shared_ptr<void> *>(m_storage));
                m_checked = checked(m_player->get(), anchors.flash_player_vtable);
            }

            ~PlayerRef() { m_player->~shared_ptr(); }

            PlayerRef(const PlayerRef &) = delete;
            PlayerRef &operator=(const PlayerRef &) = delete;
            PlayerRef(PlayerRef &&) = delete;
            PlayerRef &operator=(PlayerRef &&) = delete;

            /** @brief The player when it is a CFlashPlayer, or nullptr. */
            [[nodiscard]] void *get() const noexcept { return m_checked; }

        private:
            alignas(std::shared_ptr<void>) std::byte m_storage[sizeof(std::shared_ptr<void>)]{};
            std::shared_ptr<void> *m_player{nullptr};
            void *m_checked{nullptr};
        };

        [[nodiscard]] FlashValue number(double value) noexcept
        {
            return FlashValue{
                .type = FLASH_DOUBLE,
                .value = std::bit_cast<std::uint64_t>(value),
            };
        }

        [[nodiscard]] FlashValue text(const char *value) noexcept
        {
            return FlashValue{
                .type = FLASH_STRING,
                .value = reinterpret_cast<std::uintptr_t>(value),
            };
        }

        /** @brief True when @p element is still the mod's element: a CFlashUIElement with the mod's name. */
        [[nodiscard]] bool is_clock_element(void *element, const Anchors &anchors) noexcept
        {
            if (checked(element, anchors.flash_element_vtable) == nullptr)
            {
                return false;
            }
            const char *const name = read<const char *>(element, constants::ELEMENT_NAME_OFFSET);
            return name != nullptr && std::strcmp(name, constants::CLOCK_ELEMENT) == 0;
        }

        /** @brief True when @p element, a checked CFlashUIElement, is visible. */
        [[nodiscard]] bool element_visible(void *element, const Anchors &anchors) noexcept
        {
            const auto is_visible =
                reinterpret_cast<IsVisibleFn>(slot(anchors.flash_element_vtable, constants::ELEMENT_IS_VISIBLE_SLOT));
            return is_visible(element);
        }

        /**
         * @brief Calls the movie function @p method of @p element's player with @p args.
         * @details The call runs only while the element is visible. GetFlashPlayer loads the movie of an element the
         *          game unloaded, so a call into a hidden element would load it again.
         */
        [[nodiscard]] bool invoke(void *element, const char *method, std::span<const FlashValue> args) noexcept
        {
            const Anchors *anchors = bound_anchors();
            if (anchors == nullptr || !is_clock_element(element, *anchors) || !element_visible(element, *anchors))
            {
                return false;
            }
            const PlayerRef player(element, *anchors);
            if (player.get() == nullptr)
            {
                return false;
            }
            const auto call =
                reinterpret_cast<InvokeFn>(slot(anchors->flash_player_vtable, constants::PLAYER_INVOKE_SLOT));
            return call(player.get(), method, args.data(), static_cast<unsigned>(args.size()), nullptr);
        }
    } // namespace

    void *find_clock_element(void *cached) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr)
        {
            return nullptr;
        }
        if (cached != nullptr && is_clock_element(cached, *anchors))
        {
            return cached;
        }
        void *const ui = checked(
            read<void *>(reinterpret_cast<const void *>(anchors->genv), constants::GENV_FLASH_UI_OFFSET),
            anchors->flash_ui_vtable
        );
        if (ui == nullptr)
        {
            return nullptr;
        }
        auto *const lock = reinterpret_cast<SRWLOCK *>(static_cast<std::byte *>(ui) + constants::FLASH_UI_LOCK_OFFSET);
        void *found = nullptr;
        AcquireSRWLockShared(lock);
        void *const *const elements = read<void *const *>(ui, constants::FLASH_UI_ELEMENTS_OFFSET);
        const int count =
            elements == nullptr ? 0 : read<std::int32_t>(elements, -static_cast<std::ptrdiff_t>(sizeof(std::int32_t)));
        for (int i = 0; i < count && found == nullptr; ++i)
        {
            if (is_clock_element(elements[i], *anchors))
            {
                found = elements[i];
            }
        }
        ReleaseSRWLockShared(lock);
        return found;
    }

    bool clock_visible(void *element) noexcept
    {
        const Anchors *anchors = bound_anchors();
        return anchors != nullptr && is_clock_element(element, *anchors) && element_visible(element, *anchors);
    }

    void show_clock(void *element, bool visible) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !is_clock_element(element, *anchors))
        {
            return;
        }
        const auto set_visible =
            reinterpret_cast<SetVisibleFn>(slot(anchors->flash_element_vtable, constants::ELEMENT_SET_VISIBLE_SLOT));
        set_visible(element, visible);
    }

    std::uintptr_t clock_player(void *element) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !is_clock_element(element, *anchors) || !element_visible(element, *anchors))
        {
            return 0;
        }
        const PlayerRef player(element, *anchors);
        return reinterpret_cast<std::uintptr_t>(player.get());
    }

    bool set_line_style(void *element, int part, const LineStyle &style) noexcept
    {
        const FlashValue args[] = {
            number(part),
            number(style.font),
            number(style.size),
            number(static_cast<double>(style.color)),
            number(style.opacity),
            number(style.shadow ? 1.0 : 0.0),
        };
        return invoke(element, constants::MOVIE_SET_LINE_STYLE, args);
    }

    bool set_dial_style(void *element, const DialStyle &style) noexcept
    {
        const FlashValue args[] = {
            number(style.show ? 1.0 : 0.0),
            number(style.size),
            number(style.opacity),
            number(style.shadow ? 1.0 : 0.0),
        };
        return invoke(element, constants::MOVIE_SET_DIAL_STYLE, args);
    }

    bool set_part_place(void *element, int part, const PartPlace &place) noexcept
    {
        const FlashValue args[] = {
            number(part),
            number(place.target),
            number(place.point),
            number(place.pivot),
            number(place.x),
            number(place.y),
        };
        return invoke(element, constants::MOVIE_SET_PLACE, args);
    }

    bool set_line_texts(void *element, std::span<const char *const, 4> texts) noexcept
    {
        const FlashValue args[] = {text(texts[0]), text(texts[1]), text(texts[2]), text(texts[3])};
        return invoke(element, constants::MOVIE_SET_TEXTS, args);
    }

    bool set_clock_hour(void *element, double hour) noexcept
    {
        const FlashValue args[] = {number(hour)};
        return invoke(element, constants::MOVIE_SET_HOUR, args);
    }
} // namespace HUDClock::game
