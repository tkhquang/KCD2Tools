/**
 * @file game/lock_movie.cpp
 * @brief The mod's Scaleform element through CFlashUI's element list and CFlashPlayer::Invoke.
 */

#include "game/lock_movie.hpp"
#include "game/lockpicking.hpp"

#include <windows.h>

#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>

namespace OblivionLockpicking::game
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
        static_assert(sim::MAX_TUMBLERS == 5, "the movie's update() takes exactly five tumbler heights");

        /// SFlashVarValue::eBool, held in the value's first byte.
        constexpr std::int32_t FLASH_BOOL = 2;
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
        using SetVariableFn = bool(__fastcall *)(void *player, const char *path, const FlashValue *value);

        /**
         * @brief The element's Flash player, held for the scope of one call.
         * @details GetFlashPlayer returns a std::shared_ptr through a hidden result pointer. It copy-constructs the
         *          element's own pointer into raw storage. The release of the copy goes through the game's control
         *          block, as the game drops its own copies.
         */
        class PlayerRef
        {
        public:
            explicit PlayerRef(void *element, const Anchors &anchors) noexcept
            {
                const auto get_player = reinterpret_cast<GetPlayerFn>(
                    slot(anchors.flash_element_vtable, constants::ELEMENT_GET_PLAYER_SLOT)
                );
                get_player(element, reinterpret_cast<std::shared_ptr<void> *>(m_storage));
                m_player = std::launder(reinterpret_cast<std::shared_ptr<void> *>(m_storage));
            }

            ~PlayerRef() { m_player->~shared_ptr(); }

            PlayerRef(const PlayerRef &) = delete;
            PlayerRef &operator=(const PlayerRef &) = delete;
            PlayerRef(PlayerRef &&) = delete;
            PlayerRef &operator=(PlayerRef &&) = delete;

            /** @brief The player, or nullptr when the element has none. */
            [[nodiscard]] void *get() const noexcept { return m_player->get(); }

        private:
            alignas(std::shared_ptr<void>) std::byte m_storage[sizeof(std::shared_ptr<void>)]{};
            std::shared_ptr<void> *m_player{nullptr};
        };

        [[nodiscard]] void *flash_ui(const Anchors &anchors) noexcept
        {
            if (anchors.genv == 0 || anchors.flash_ui_vtable == 0)
            {
                return nullptr;
            }
            void *const ui =
                read<void *>(reinterpret_cast<const void *>(anchors.genv), constants::GENV_FLASH_UI_OFFSET);
            return vtable_of(ui) == anchors.flash_ui_vtable ? ui : nullptr;
        }

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

        [[nodiscard]] FlashValue boolean(bool value) noexcept
        {
            return FlashValue{
                .type = FLASH_BOOL,
                .value = value ? 1u : 0u,
            };
        }

        /** @brief The loaded element named @p name, or nullptr. */
        [[nodiscard]] void *find_element(const char *name) noexcept
        {
            const Anchors *anchors = bound_anchors();
            void *const ui = anchors == nullptr || anchors->flash_element_vtable == 0 ? nullptr : flash_ui(*anchors);
            if (ui == nullptr)
            {
                return nullptr;
            }
            auto *const lock =
                reinterpret_cast<SRWLOCK *>(static_cast<std::byte *>(ui) + constants::FLASH_UI_LOCK_OFFSET);
            void *found = nullptr;
            AcquireSRWLockShared(lock);
            void *const *const elements = read<void *const *>(ui, constants::FLASH_UI_ELEMENTS_OFFSET);
            const int count = elements == nullptr
                                  ? 0
                                  : read<std::int32_t>(elements, -static_cast<std::ptrdiff_t>(sizeof(std::int32_t)));
            for (int i = 0; i < count && found == nullptr; ++i)
            {
                void *const element = elements[i];
                if (vtable_of(element) != anchors->flash_element_vtable)
                {
                    continue;
                }
                const char *const element_name = read<const char *>(element, constants::ELEMENT_NAME_OFFSET);
                if (element_name != nullptr && std::strcmp(element_name, name) == 0)
                {
                    found = element;
                }
            }
            ReleaseSRWLockShared(lock);
            return found;
        }

        /**
         * @brief True when @p element is still a CFlashUIElement.
         * @details A session keeps its element pointer across frames, so each call checks the vtable again before a
         *          slot call.
         */
        [[nodiscard]] bool is_element(const void *element, const Anchors &anchors) noexcept
        {
            return anchors.flash_element_vtable != 0 && vtable_of(element) == anchors.flash_element_vtable;
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
         *          game unloaded, and the game unloads hidden elements (wh_ui_ApseUnloadMode 2, its default). A call
         *          into a hidden element on every frame reloads its movie and textures on every frame and exhausts
         *          video memory.
         */
        [[nodiscard]] bool invoke(void *element, const char *method, std::span<const FlashValue> args) noexcept
        {
            const Anchors *anchors = bound_anchors();
            if (anchors == nullptr || anchors->flash_player_vtable == 0 || !is_element(element, *anchors) ||
                !element_visible(element, *anchors))
            {
                return false;
            }
            const PlayerRef player(element, *anchors);
            if (vtable_of(player.get()) != anchors->flash_player_vtable)
            {
                return false;
            }
            const auto call =
                reinterpret_cast<InvokeFn>(slot(anchors->flash_player_vtable, constants::PLAYER_INVOKE_SLOT));
            return call(player.get(), method, args.data(), static_cast<unsigned>(args.size()), nullptr);
        }
    } // namespace

    void *find_lock_movie() noexcept
    {
        return find_element(constants::LOCK_MOVIE_ELEMENT);
    }

    void show_lock_movie(void *element, bool visible) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || !is_element(element, *anchors))
        {
            return;
        }
        const auto set_visible =
            reinterpret_cast<SetVisibleFn>(slot(anchors->flash_element_vtable, constants::ELEMENT_SET_VISIBLE_SLOT));
        set_visible(element, visible);
    }

    bool lock_movie_visible(void *element) noexcept
    {
        const Anchors *anchors = bound_anchors();
        return anchors != nullptr && is_element(element, *anchors) && element_visible(element, *anchors);
    }

    bool update_lock_movie(void *element, const MovieFrame &frame) noexcept
    {
        const FlashValue args[] = {
            number(frame.pick),
            number(frame.kick),
            number(frame.heights[0]),
            number(frame.heights[1]),
            number(frame.heights[2]),
            number(frame.heights[3]),
            number(frame.heights[4]),
            number(frame.opening ? 1.0 : 0.0),
            number(frame.breaking ? 1.0 : 0.0),
            number(frame.scale),
            number(frame.fancy ? 1.0 : 0.0),
            number(frame.set_count),
            number(frame.total),
            number(frame.show_header ? 1.0 : 0.0),
            number(frame.show_footer ? 1.0 : 0.0),
        };
        return invoke(element, constants::LOCK_MOVIE_UPDATE, args);
    }

    void begin_lock_movie(void *element) noexcept
    {
        (void)invoke(element, constants::LOCK_MOVIE_BEGIN, {});
    }

    bool set_lock_movie_controls(void *element, const char *row) noexcept
    {
        const FlashValue args[] = {text(row)};
        return invoke(element, constants::LOCK_MOVIE_CONTROLS, args);
    }

    void show_movie_content(void *element, bool visible) noexcept
    {
        const Anchors *anchors = bound_anchors();
        if (anchors == nullptr || anchors->flash_player_vtable == 0 || !is_element(element, *anchors) ||
            !element_visible(element, *anchors))
        {
            return;
        }
        const PlayerRef player(element, *anchors);
        if (vtable_of(player.get()) != anchors->flash_player_vtable)
        {
            return;
        }
        const FlashValue value = boolean(visible);
        const auto set_variable =
            reinterpret_cast<SetVariableFn>(slot(anchors->flash_player_vtable, constants::PLAYER_SET_VARIABLE_SLOT));
        set_variable(player.get(), constants::MOVIE_ROOT_VISIBLE, &value);
    }

    void hide_lock_prompts() noexcept
    {
        void *const hud = find_element(constants::HUD_ELEMENT);
        for (const char *const action : constants::HUD_LOCK_PROMPTS)
        {
            const FlashValue args[] = {text(action), number(constants::HUD_REMOVE_AT_ONCE)};
            if (!invoke(hud, constants::HUD_REMOVE_PROMPT, args))
            {
                return;
            }
        }
    }
} // namespace OblivionLockpicking::game
