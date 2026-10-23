/**
 * @file game/input_listener.cpp
 * @brief The listener object the engine calls, built by hand to KCD2's IInputEventListener layout.
 */

#include "game/input_listener.hpp"
#include "game/lockpicking.hpp"

namespace OblivionLockpicking::game
{
    namespace
    {
        using AddListenerFn = void(__fastcall *)(void *input, void *listener, const char *name, bool flag);
        using RemoveListenerFn = void(__fastcall *)(void *input, void *listener);

        struct Listener;

        /**
         * @brief Slot 0 of KCD2's IInputEventListener, which has four slots in a shuffled order and no virtual
         *        destructor.
         * @details The engine's own listeners return false from OnInputEventUI, 0 from GetPriority and true from
         *          slot 3.
         */
        bool __fastcall on_input_event(Listener *self, const void *event) noexcept;
        bool __fastcall on_input_event_ui(Listener *, const void *) noexcept
        {
            return false;
        }
        int __fastcall get_priority(const Listener *) noexcept
        {
            return 0;
        }
        /// Slot 3, which CryEngine 5's interface does not have. The engine's own listeners return true from it.
        bool __fastcall slot_3(const Listener *) noexcept
        {
            return true;
        }

        /// The table the engine calls through. A hand-built table keeps the order independent of the compiler's ABI.
        const void *const LISTENER_VTABLE[] = {
            reinterpret_cast<const void *>(&on_input_event),
            reinterpret_cast<const void *>(&on_input_event_ui),
            reinterpret_cast<const void *>(&get_priority),
            reinterpret_cast<const void *>(&slot_3),
        };

        /** @brief The object the engine holds: only its table pointer. */
        struct Listener
        {
            /// The hand-built table.
            const void *const *vtable{LISTENER_VTABLE};
        };

        // Main thread only, as the engine's dispatch that calls the listener.
        Listener s_listener;
        InputHandler s_handler = nullptr;
        /// The input system the listener is attached to, or nullptr.
        void *s_input = nullptr;
        /// The input system's RemoveConsoleEventListener, kept from the attach for the detach.
        RemoveListenerFn s_remove = nullptr;

        bool __fastcall on_input_event(Listener *, const void *event) noexcept
        {
            if (s_handler == nullptr)
            {
                return false;
            }
            const char *const name = read<const char *>(event, constants::INPUT_EVENT_NAME_OFFSET);
            const InputEvent decoded{
                .state = static_cast<InputState>(read<std::uint8_t>(event, constants::INPUT_EVENT_STATE_OFFSET)),
                .name = name == nullptr ? std::string_view{} : std::string_view(name),
                .value = read<float>(event, constants::INPUT_EVENT_VALUE_OFFSET),
            };
            return s_handler(decoded);
        }

        /** @brief gEnv's input system, or nullptr when it is not a CDXInput with the expected layout. */
        [[nodiscard]] void *input_system(const Anchors &anchors) noexcept
        {
            if (anchors.genv == 0 || anchors.input_vtable == 0)
            {
                return nullptr;
            }
            void *const object =
                read<void *>(reinterpret_cast<const void *>(anchors.genv), constants::GENV_INPUT_OFFSET);
            return vtable_of(object) == anchors.input_vtable ? object : nullptr;
        }
    } // namespace

    bool attach_input_listener(InputHandler handler) noexcept
    {
        s_handler = handler;
        if (s_input != nullptr)
        {
            return true;
        }
        const Anchors *anchors = bound_anchors();
        void *const input = anchors == nullptr ? nullptr : input_system(*anchors);
        if (input == nullptr)
        {
            return false;
        }
        // The game's own callers pass a name and true, and the listener set ignores both.
        reinterpret_cast<AddListenerFn>(slot(anchors->input_vtable, constants::INPUT_ADD_CONSOLE_LISTENER_SLOT))(
            input,
            &s_listener,
            constants::MOD_NAME,
            true
        );
        s_remove = reinterpret_cast<RemoveListenerFn>(
            slot(anchors->input_vtable, constants::INPUT_REMOVE_CONSOLE_LISTENER_SLOT)
        );
        s_input = input;
        return true;
    }

    void detach_input_listener() noexcept
    {
        if (s_input != nullptr && s_remove != nullptr)
        {
            s_remove(s_input, &s_listener);
        }
        s_input = nullptr;
        s_remove = nullptr;
        s_handler = nullptr;
    }
} // namespace OblivionLockpicking::game
