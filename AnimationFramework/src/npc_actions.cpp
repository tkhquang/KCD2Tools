/**
 * @file npc_actions.cpp
 * @brief NPC actions (see npc_actions.hpp).
 *
 * The binds (C_ScriptBindHuman PlayAnim and StopAnim, consecutive slots) share one shape: the script's actor
 * (GameFramework -> IActorSystem -> GetActor), IActor::IsPlayer, then the work for the player only. The framework takes
 * those three calls from the binds' own code, so it reaches the actor exactly as the bind does, and runs the work for a
 * non-player actor before the bind returns as it always does. Play (Steam 0x182AAA040): fragment CRC -> the actor's
 * controller definition, tags split on '+' (fragment tags, else global tags set on the context), the previous action
 * stopped, new TAction(priority 5) kept at actor+0xBA0 and queued on the actor's action controller. Stop (Steam
 * 0x180BC69B8): that action gets Stopping | BlendOut and the actor lets it go. A marked action still plays to its end
 * and the next one waits behind it, so a new PlayAnim first finishes the previous action, on Henry too, and StopAnim
 * finishes it as well, which ends a one-shot at once instead of at its end. The play code also forces its action onto
 * every scope, which puts a fragment made for the face or the upper body on the base layer and leaves the body in its
 * bind pose. A script that plays one adds OWN_SCOPES_TAG to the tags, which the play code skips as a name it does not
 * know, and the action then gets the scopes its fragment defines. Not every action can: a
 * fragment with no option for the character's tags then empties its scopes, which is the bind pose again, where the
 * forced scopes leave it playing nothing. tag_rules::OPTION_TAG_PREFIX picks one option of the matched tag set instead
 * of a random one, by the action's option index, HOLD_TAG keeps a one-shot on its last pose until it is stopped, and
 * QUEUE_TAG leaves the running action alone, so the new one waits behind it as in the game's own play.
 *
 * The play code returns at once for a fragment the character's controller definition lacks, before it stops the running
 * action, so the framework then leaves that action alone too.
 *
 * A fragment with no option for the character's tags shows the bind pose for a moment under the forced scopes too, so a
 * script plays it with the tags of one of its options. Those stay on for good, as the play code never clears a global
 * tag. TEMPORARY_TAGS_TAG in the tags makes the framework note the bits the play changed, and the next PlayAnim on that
 * character puts those bits back, also a play of `animfw_restore_tags`, a fragment no character has. A tag the
 * character already had changes nothing, so it stays. The option is chosen when the action installs and a play
 * action (flags 0, so no AutoReinstall) never asks again, so the tags can go as soon as the fragment shows. The game
 * rebuilds Henry's tags on every tag update, though, and may switch a tag off before then (MoveSpeed follows his
 * speed), so his temporary tags leave the tag state at once and go into his option queries instead (forced_options).
 *
 * +0xBA0 is a C_Player member: on an NPC that offset is something else, so the play and stop code must never see an NPC
 * itself. They get a stand-in instead. Of an actor they use only that member and one virtual, the animated character,
 * which holds the action controller, so the stand-in forwards the virtual to the real actor and holds the member
 * itself. The binds run on the main thread, so one stand-in serves every NPC: s_npc_actions keeps each NPC's action
 * between calls. Nothing in the game keeps a pointer to the stand-in or to this image, and a reload leaves running
 * actions to the game.
 */

#include "npc_actions.hpp"
#include "constants.hpp"
#include "core/mod_maker.hpp"
#include "core/tag_rules.hpp"
#include "forced_options.hpp"
#include "game_memory.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace AnimationFramework
{
    namespace
    {
        using PlayAnimBindFunc = void *(__fastcall *)(void *bind, void *args, const char *fragment, const char *tags);
        using StopAnimBindFunc = void *(__fastcall *)(void *bind, void *args);
        using ScriptActorFunc = void *(__fastcall *)(void *bind, void *args);
        using IsPlayerFunc = bool(__fastcall *)(void *actor);
        using PlayFunc = void(__fastcall *)(void *actor, const char *fragment, const char *tags);
        using StopFunc = void(__fastcall *)(void *actor);

        /**
         * @brief Call sites inside the binds (anchors.cpp): PlayAnim's actor, IsPlayer and play, then StopAnim's actor,
         *        IsPlayer and stop.
         */
        constexpr std::uintptr_t PLAY_BIND_ACTOR_CALL = 0x1D;
        constexpr std::uintptr_t PLAY_BIND_IS_PLAYER_CALL = 0x2D;
        constexpr std::uintptr_t PLAY_BIND_PLAY_CALL = 0x3F;
        constexpr std::uintptr_t STOP_BIND_ACTOR_CALL = 0xD;
        constexpr std::uintptr_t STOP_BIND_IS_PLAYER_CALL = 0x1D;
        constexpr std::uintptr_t STOP_BIND_STOP_CALL = 0x29;
        constexpr std::uint8_t CALL_OPCODE = 0xE8;

        /**
         * @brief The two things the play and stop code use of an actor, the same on the Steam, GOG and Game Pass
         *        builds.
         * @details decode_binds checks them in the code: the play's first `call [rax+disp32]` (FF 90 disp32) and the
         *          stop's first `lea r8, [rcx+disp32]`.
         */
        constexpr std::size_t ACTOR_ACTION_OFFSET = 0xBA0;
        constexpr std::size_t ACTOR_ANIMATED_CHARACTER_SLOT = 0x2D0 / sizeof(void *);
        constexpr std::uintptr_t PLAY_VIRTUAL_CALL = 0xA9;
        constexpr std::uintptr_t STOP_ACTION_LEA = 4;
        constexpr std::array<std::uint8_t, 2> VIRTUAL_CALL_OPCODE{0xFF, 0x90};
        constexpr std::array<std::uint8_t, 3> ACTION_LEA_OPCODE{0x4C, 0x8D, 0x81};

        /**
         * @brief How the play code goes from the animated character to the context, the same on the Steam, GOG and
         *        Game Pass builds.
         * @details decode_binds checks both slots where the play code loads them: `mov rdx, [rcx+disp32]` and
         *          `mov r8, [rcx+disp32]`.
         */
        constexpr std::size_t CHARACTER_ACTION_CONTROLLER_SLOT = 0x130 / sizeof(void *);
        constexpr std::size_t ACTION_CONTROLLER_CONTEXT_SLOT = 0xB8 / sizeof(void *);
        constexpr std::uintptr_t PLAY_CONTROLLER_SLOT_LOAD = 0x46;
        constexpr std::uintptr_t PLAY_CONTEXT_SLOT_LOAD = 0x55;
        constexpr std::array<std::uint8_t, 3> RDX_SLOT_LOAD_OPCODE{0x48, 0x8B, 0x91};
        constexpr std::array<std::uint8_t, 3> R8_SLOT_LOAD_OPCODE{0x4C, 0x8B, 0x81};
        /**
         * @brief Then from the context to the fragment names: `mov rcx, [rax]` (the controller definition at +0) and
         *        `mov rcx, [rcx+18h]` (its fragment IDs), which forced_options reads the same way.
         */
        constexpr std::uintptr_t PLAY_FRAGMENT_IDS_LOAD = 0x64;
        constexpr std::array<std::uint8_t, 7> FRAGMENT_IDS_LOAD_BYTES{0x48, 0x8B, 0x08, 0x48, 0x8B, 0x49, 0x18};

        /**
         * @brief IAction::m_forcedScopeMask, m_eStatus and m_flags, the same on the Steam, GOG and Game Pass builds
         *        (the TAction constructor).
         * @details A forced mask of 0 leaves the scopes to the fragment's definition. Finished is EStatus 4, and
         *          Interruptable is flag bit 2.
         */
        constexpr std::size_t ACTION_FORCED_SCOPES_OFFSET = 0x18;
        constexpr std::size_t ACTION_STATUS_OFFSET = 0x28;
        constexpr std::size_t ACTION_FLAGS_OFFSET = 0x2C;
        constexpr std::uint32_t ACTION_STATUS_FINISHED = 4;
        constexpr std::uint32_t ACTION_FLAG_INTERRUPTABLE = 1u << 2;
        /**
         * @brief IAction::NoAutoBlendOut: a one-shot action that reaches its end stays installed, its clips on their
         *        last key (Mannequin plays every non-looping clip with CA_REPEAT_LAST_KEY), until it is stopped.
         */
        constexpr std::uint32_t ACTION_FLAG_NO_AUTO_BLEND_OUT = 1u << 1;
        /**
         * @brief IAction::m_optionIdx (OPTION_IDX_RANDOM until set), the same on the Steam, GOG and Game Pass builds
         *        (the TAction constructor). The database answers option n % count of the tag set the query matches.
         */
        constexpr std::size_t ACTION_OPTION_INDEX_OFFSET = 0x50;
        /// The tag marks a play's tags as temporary (see the file comment).
        constexpr std::string_view TEMPORARY_TAGS_TAG = "animfw_temporary_tags";
        constexpr std::string_view HOLD_TAG = "animfw_hold";
        /**
         * @brief The play's action waits behind the running one, as the game's own play does, instead of cutting it:
         *        the next part of a sequence starts where the one before ends.
         */
        constexpr std::string_view QUEUE_TAG = "animfw_queue";

        struct BindCalls
        {
            ScriptActorFunc script_actor = nullptr;
            IsPlayerFunc is_player = nullptr;
            PlayFunc play = nullptr;
            StopFunc stop = nullptr;
        };
        // Written by setup before the hooks arm, read by the detours after.
        BindCalls s_calls{};
        std::atomic<PlayAnimBindFunc> s_play_anim_original{nullptr};
        std::atomic<StopAnimBindFunc> s_stop_anim_original{nullptr};

        /// What the play and stop code get in place of an NPC (see the file comment).
        struct ActorStandIn
        {
            const void *const *vtable;
            void *actor;
            std::array<std::byte, ACTOR_ACTION_OFFSET - 2 * sizeof(void *)> unused;
            void *action;
        };
        static_assert(offsetof(ActorStandIn, action) == ACTOR_ACTION_OFFSET);

        /// The stand-in's animated-character virtual: the real NPC's.
        void *__fastcall stand_in_animated_character(ActorStandIn *self) noexcept
        {
            return vcall<void *>(self->actor, ACTOR_ANIMATED_CHARACTER_SLOT);
        }

        const std::array<const void *, ACTOR_ANIMATED_CHARACTER_SLOT + 1> STAND_IN_VTABLE = []
        {
            std::array<const void *, ACTOR_ANIMATED_CHARACTER_SLOT + 1> slots{};
            slots[ACTOR_ANIMATED_CHARACTER_SLOT] = reinterpret_cast<const void *>(&stand_in_animated_character);
            return slots;
        }();
        // Main thread only, like everything below that touches it.
        ActorStandIn s_stand_in{STAND_IN_VTABLE.data(), nullptr, {}, nullptr};
        /// Each NPC's running or last action, which the stand-in holds a reference to while it plays.
        std::unordered_map<void *, void *> s_npc_actions;

        using TagState = forced_options::TagState;
        /// A character's global tags before and after a play that marked them temporary. Main thread only.
        struct TemporaryTags
        {
            TagState before{};
            TagState after{};
        };
        std::unordered_map<void *, TemporaryTags> s_temporary_tags;

        /** @brief The target of the call at @p site, or 0 when no E8 call is there. */
        [[nodiscard]] std::uintptr_t call_target(std::uintptr_t site) noexcept
        {
            if (site == 0 || read_value<std::uint8_t>(site).value_or(0) != CALL_OPCODE)
            {
                return 0;
            }
            const auto displacement = read_value<std::int32_t>(site + 1);
            return displacement ? site + 5 + static_cast<std::intptr_t>(*displacement) : 0;
        }

        /** @brief Runs @p work with the stand-in standing for @p actor and holding its action. */
        template <typename Work> void with_stand_in(void *actor, Work work) noexcept
        {
            try
            {
                void *&action = s_npc_actions[actor];
                s_stand_in.actor = actor;
                s_stand_in.action = action;
                work();
                action = s_stand_in.action;
                if (action == nullptr)
                {
                    s_npc_actions.erase(actor);
                }
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "NpcActions: no memory for the NPC's action");
            }
        }

        /**
         * @brief What IAction::ForceFinish does for @p action, which may be null.
         * @details The stop in the play code only marks the action, which plays to its end while the new one, of the
         *          same priority, waits behind it. The controller replaces a finished action at once, and drops it from
         *          its queue if it never started.
         */
        void finish_action(void *action) noexcept
        {
            if (action != nullptr)
            {
                auto *const bytes = static_cast<std::byte *>(action);
                *reinterpret_cast<std::uint32_t *>(bytes + ACTION_STATUS_OFFSET) = ACTION_STATUS_FINISHED;
                *reinterpret_cast<std::uint32_t *>(bytes + ACTION_FLAGS_OFFSET) &= ~ACTION_FLAG_INTERRUPTABLE;
            }
        }

        /**
         * @brief Leaves @p action, which may be null, on the scopes of its fragment.
         * @note The action is queued but not yet installed.
         */
        void use_fragment_scopes(void *action) noexcept
        {
            if (action != nullptr)
            {
                *reinterpret_cast<std::uint32_t *>(static_cast<std::byte *>(action) + ACTION_FORCED_SCOPES_OFFSET) = 0;
            }
        }

        /**
         * @brief Has @p action, which may be null, play option @p option (none for -1).
         * @note The action is queued but not yet installed.
         */
        void use_option(void *action, std::int64_t option) noexcept
        {
            if (action != nullptr && option >= 0)
            {
                *reinterpret_cast<std::uint32_t *>(static_cast<std::byte *>(action) + ACTION_OPTION_INDEX_OFFSET) =
                    static_cast<std::uint32_t>(option);
            }
        }

        /** @brief Has @p action, which may be null, hold its last pose at its end until it is stopped. */
        void hold_at_end(void *action) noexcept
        {
            if (action != nullptr)
            {
                *reinterpret_cast<std::uint32_t *>(static_cast<std::byte *>(action) + ACTION_FLAGS_OFFSET) |=
                    ACTION_FLAG_NO_AUTO_BLEND_OUT;
            }
        }

        /// The action the player's play code keeps at @p actor (the C_Player member), or null.
        [[nodiscard]] void *player_action(void *actor) noexcept
        {
            return reinterpret_cast<void *>(read_ptr(reinterpret_cast<std::uintptr_t>(actor) + ACTOR_ACTION_OFFSET));
        }

        /** @brief The SAnimationContext of @p actor's action controller, or 0 when it has none. */
        [[nodiscard]] std::uintptr_t animation_context(void *actor) noexcept
        {
            void *const character = vcall<void *>(actor, ACTOR_ANIMATED_CHARACTER_SLOT);
            void *const controller =
                character != nullptr ? vcall<void *>(character, CHARACTER_ACTION_CONTROLLER_SLOT) : nullptr;
            return controller != nullptr
                       ? reinterpret_cast<std::uintptr_t>(vcall<void *>(controller, ACTION_CONTROLLER_CONTEXT_SLOT))
                       : 0;
        }

        /** @brief The global tags of the context at @p context, or null for none. */
        [[nodiscard]] TagState *context_tags(std::uintptr_t context) noexcept
        {
            return context != 0 ? reinterpret_cast<TagState *>(context + constants::ANIMATION_CONTEXT_TAG_STATE_OFFSET)
                                : nullptr;
        }

        /** @brief Undoes what @p actor's last temporary play changed in its tags, if there is one to undo. */
        void restore_tags(void *actor) noexcept
        {
            const auto entry = s_temporary_tags.find(actor);
            if (entry == s_temporary_tags.end())
            {
                return;
            }
            if (TagState *live = context_tags(animation_context(actor)))
            {
                tag_rules::take_changed_bits(*live, entry->second.before, entry->second.after);
            }
            s_temporary_tags.erase(entry);
        }

        /// Notes @p actor's tags before and after a temporary play for restore_tags.
        void remember_tags(void *actor, const TagState &before, const TagState &after) noexcept
        {
            if (before == after)
            {
                return;
            }
            try
            {
                s_temporary_tags[actor] = TemporaryTags{before, after};
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "NpcActions: no memory for the temporary tags");
            }
        }

        /**
         * @brief Keeps what a temporary play changed in @p live, the tags of the context at @p context: Henry's go back
         *        at once and into his option queries for @p fragment_id (until it starts, for a @p queued one), an
         *        NPC's stay until restore_tags.
         */
        void keep_temporary(
            void *actor,
            bool is_henry,
            std::uintptr_t context,
            std::int32_t fragment_id,
            const TagState &before,
            TagState &live,
            bool queued
        ) noexcept
        {
            const TagState after = live;
            if (is_henry && before != after && forced_options::available() &&
                forced_options::force(context, fragment_id, before, after, queued))
            {
                tag_rules::take_changed_bits(live, before, after);
                return;
            }
            remember_tags(actor, before, after);
        }

        /**
         * @brief PlayAnim bind detour: the play for an NPC through the stand-in, the framework's play tags for Henry
         *        around the bind's own play, then the temporary tags of either.
         * @note Main thread (the script bind). No-throw.
         */
        void *__fastcall detour_play_anim(void *bind, void *args, const char *fragment, const char *tags) noexcept
        {
            // The acquire load pairs with the install's store of the trampoline, which follows the s_calls write.
            const PlayAnimBindFunc original = s_play_anim_original.load(std::memory_order_acquire);
            void *actor = s_calls.script_actor(bind, args);
            const bool is_henry = actor != nullptr && s_calls.is_player(actor);
            const std::string_view tag_list = tags != nullptr ? tags : "";
            const bool own_scopes = tag_list.find(OWN_SCOPES_TAG) != std::string_view::npos;
            const bool temporary = tag_list.find(TEMPORARY_TAGS_TAG) != std::string_view::npos;
            const std::int64_t option = tag_rules::requested_option(tag_list);
            const bool hold = tag_list.find(HOLD_TAG) != std::string_view::npos;
            const bool queued = tag_list.find(QUEUE_TAG) != std::string_view::npos;
            if (actor != nullptr && fragment != nullptr)
            {
                restore_tags(actor);
                if (is_henry)
                {
                    forced_options::clear();
                }
            }
            // The play code faults on an actor without a context, and it plays nothing for a fragment it cannot
            // resolve, so the framework touches no action then.
            const std::uintptr_t context = actor != nullptr && fragment != nullptr ? animation_context(actor) : 0;
            const std::int32_t fragment_id = forced_options::resolve_fragment(context, fragment);
            const bool plays = fragment_id >= 0;
            TagState *const live_tags = plays && temporary ? context_tags(context) : nullptr;
            const TagState tags_before = live_tags != nullptr ? *live_tags : TagState{};
            if (plays && is_henry && !queued)
            {
                finish_action(player_action(actor));
            }
            else if (plays && !is_henry)
            {
                with_stand_in(
                    actor,
                    [&]
                    {
                        if (!queued)
                        {
                            finish_action(s_stand_in.action);
                        }
                        s_calls.play(&s_stand_in, fragment, tags != nullptr ? tags : "");
                        if (own_scopes)
                        {
                            use_fragment_scopes(s_stand_in.action);
                        }
                        use_option(s_stand_in.action, option);
                        if (hold)
                        {
                            hold_at_end(s_stand_in.action);
                        }
                    }
                );
            }
            void *result = original(bind, args, fragment, tags);
            if (plays && is_henry)
            {
                void *const action = player_action(actor);
                if (own_scopes)
                {
                    use_fragment_scopes(action);
                }
                use_option(action, option);
                if (hold)
                {
                    hold_at_end(action);
                }
            }
            if (live_tags != nullptr)
            {
                keep_temporary(actor, is_henry, context, fragment_id, tags_before, *live_tags, queued);
            }
            return result;
        }

        /**
         * @brief StopAnim bind detour: finishes the action first, on Henry and on NPCs, then runs the bind.
         * @details The game's stop only marks the action, so a one-shot plays to its end. Finishing it first ends it at
         *          once, the way a new PlayAnim cuts the one before.
         * @note Main thread (the script bind). No-throw.
         */
        void *__fastcall detour_stop_anim(void *bind, void *args) noexcept
        {
            const StopAnimBindFunc original = s_stop_anim_original.load(std::memory_order_acquire);
            void *actor = s_calls.script_actor(bind, args);
            if (actor != nullptr && s_calls.is_player(actor))
            {
                finish_action(player_action(actor));
            }
            else if (actor != nullptr)
            {
                with_stand_in(
                    actor,
                    []
                    {
                        finish_action(s_stand_in.action);
                        s_calls.stop(&s_stand_in);
                    }
                );
            }
            return original(bind, args);
        }

        /** @brief True when the code at @p address is @p bytes. */
        template <std::size_t N>
        [[nodiscard]] bool has_bytes(std::uintptr_t address, const std::array<std::uint8_t, N> &bytes) noexcept
        {
            for (std::size_t i = 0; i < N; ++i)
            {
                if (read_value<std::uint8_t>(address + i).value_or(0) != bytes[i])
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief True when the code at @p address is @p opcode followed by the 32-bit @p operand. */
        template <std::size_t N>
        [[nodiscard]] bool
        has_instruction(std::uintptr_t address, const std::array<std::uint8_t, N> &opcode, std::size_t operand) noexcept
        {
            return has_bytes(address, opcode) && read_value<std::uint32_t>(address + N).value_or(0) == operand;
        }

        /** @brief Reads the binds' calls into @p calls, or returns false when either bind has another shape. */
        [[nodiscard]] bool decode_binds(std::uintptr_t play_bind, std::uintptr_t stop_bind, BindCalls &calls) noexcept
        {
            const std::uintptr_t actor = call_target(play_bind + PLAY_BIND_ACTOR_CALL);
            const std::uintptr_t is_player = call_target(play_bind + PLAY_BIND_IS_PLAYER_CALL);
            const std::uintptr_t play = call_target(play_bind + PLAY_BIND_PLAY_CALL);
            const std::uintptr_t stop = call_target(stop_bind + STOP_BIND_STOP_CALL);
            // StopAnim has to reach the actor the same way PlayAnim does.
            if (actor == 0 || is_player == 0 || play == 0 || stop == 0 ||
                call_target(stop_bind + STOP_BIND_ACTOR_CALL) != actor ||
                call_target(stop_bind + STOP_BIND_IS_PLAYER_CALL) != is_player)
            {
                return false;
            }
            // The stand-in has to match what the play and stop code use of an actor.
            if (!has_instruction(
                    play + PLAY_VIRTUAL_CALL,
                    VIRTUAL_CALL_OPCODE,
                    ACTOR_ANIMATED_CHARACTER_SLOT * sizeof(void *)
                ) ||
                !has_instruction(stop + STOP_ACTION_LEA, ACTION_LEA_OPCODE, ACTOR_ACTION_OFFSET))
            {
                return false;
            }
            // Temporary tags go through the context the same way the play code reaches it.
            if (!has_instruction(
                    play + PLAY_CONTROLLER_SLOT_LOAD,
                    RDX_SLOT_LOAD_OPCODE,
                    CHARACTER_ACTION_CONTROLLER_SLOT * sizeof(void *)
                ) ||
                !has_instruction(
                    play + PLAY_CONTEXT_SLOT_LOAD,
                    R8_SLOT_LOAD_OPCODE,
                    ACTION_CONTROLLER_CONTEXT_SLOT * sizeof(void *)
                ) ||
                !has_bytes(play + PLAY_FRAGMENT_IDS_LOAD, FRAGMENT_IDS_LOAD_BYTES))
            {
                return false;
            }
            calls = BindCalls{
                .script_actor = reinterpret_cast<ScriptActorFunc>(actor),
                .is_player = reinterpret_cast<IsPlayerFunc>(is_player),
                .play = reinterpret_cast<PlayFunc>(play),
                .stop = reinterpret_cast<StopFunc>(stop),
            };
            return true;
        }
    } // namespace

    void install_npc_actions(const Anchors &anchors, bool enabled, DMK::hook::HookStack &hooks) noexcept
    {
        DMK::Logger &logger = DMK::log();
        if (!enabled)
        {
            (void)logger.log_noexcept(DMK::LogLevel::Info, "NpcActions: off in the INI");
            return;
        }
        if (anchors.play_anim_bind == 0 || anchors.stop_anim_bind == 0 ||
            !decode_binds(anchors.play_anim_bind, anchors.stop_anim_bind, s_calls))
        {
            (void)logger.log_noexcept(
                DMK::LogLevel::Warning,
                "NpcActions: the PlayAnim / StopAnim binds were not found; they stay Henry's"
            );
            return;
        }
        try
        {
            auto played = install_inline_hook<&detour_play_anim>(
                hooks,
                "PlayAnimBind",
                anchors.play_anim_bind,
                s_play_anim_original
            );
            if (!played.has_value())
            {
                logger.warning(
                    "NpcActions: PlayAnim hook failed ({}); the binds stay Henry's",
                    played.error().message()
                );
                return;
            }
            auto stopped = install_inline_hook<&detour_stop_anim>(
                hooks,
                "StopAnimBind",
                anchors.stop_anim_bind,
                s_stop_anim_original
            );
            if (!stopped.has_value())
            {
                logger.warning(
                    "NpcActions: StopAnim hook failed ({}); NPCs play but StopAnim stays Henry's",
                    stopped.error().message()
                );
                return;
            }
            logger.info("NpcActions: human:PlayAnim / StopAnim play on NPCs too");
        }
        catch (...)
        {
            (void)logger.log_noexcept(
                DMK::LogLevel::Error,
                "NpcActions: installing the PlayAnim / StopAnim hooks failed"
            );
        }
    }
} // namespace AnimationFramework
