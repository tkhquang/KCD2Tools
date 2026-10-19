/**
 * @file anchors.hpp
 * @brief The WHGame.dll anchors and the features they enable.
 *
 * Audio itself needs no anchor: it goes through the FMOD DLLs' public exports. Everything the framework reads, calls or
 * hooks inside the game resolves at startup from byte signatures and RTTI type names, and each feature turns on only
 * when every anchor it uses resolved (anchor::evaluate_gate).
 */
#ifndef SNDFW_ANCHORS_HPP
#define SNDFW_ANCHORS_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace SoundFramework
{
    /** @brief The resolved anchors, 0 when unresolved, and the features they enable. */
    struct Anchors
    {
        /// SSystemGlobalEnvironment (gEnv), a static struct inside the game image.
        std::uintptr_t genv = 0;
        /// The slot that holds the game context once the game created it (null before).
        std::uintptr_t context_slot = 0;
        /// Primary vtable of CXConsole, checked against gEnv->pConsole before its slots are called.
        std::uintptr_t console_vtable = 0;
        /// Primary vtable of CSystem, checked against gEnv->pSystem.
        std::uintptr_t system_vtable = 0;
        /// CSystem::Update, slot 5 of that vtable: once per frame on the main thread.
        std::uintptr_t system_update = 0;
        /// Primary vtable of CEntitySystem, checked against gEnv->pEntitySystem.
        std::uintptr_t entity_system_vtable = 0;
        /// Primary vtable of CEntity, checked on every entity before its slots are called.
        std::uintptr_t entity_vtable = 0;
        /// Primary vtable of CActorSystem, checked against the game context's actor system.
        std::uintptr_t actor_system_vtable = 0;
        /// C_Actor's speak-manager getter (C_Actor vtable slot 143), checked on every actor before it is called.
        std::uintptr_t get_speak_man = 0;
        /// void Speak(C_ActorSpeakMan*, const SpeakRequest*): plays a voice file as the actor's speech.
        std::uintptr_t speak = 0;
        /// SpeakRequest* SpeakRequest(SpeakRequest*): the request's constructor (empty strings, default flags).
        std::uintptr_t request_ctor = 0;
        /// void ~SpeakRequest(SpeakRequest*): releases the request's three strings.
        std::uintptr_t request_dtor = 0;
        /// CryString& CryString::operator=(CryString*, const char*): copies text into a game-allocated string.
        std::uintptr_t string_assign = 0;
        /// void Stop(C_ActorSpeakMan*): stops the speaker's current line (its sound, lip sync and stop timer).
        std::uintptr_t speak_stop = 0;
        /// The return address of the dialog line player's call into Speak, which marks a conversation line.
        std::uintptr_t dialog_speak_return = 0;
        /// bool LoadCryEngineData(SaveGameManager*, chunk, save path): the engine-data stage of every save load.
        std::uintptr_t save_game_load = 0;
        /// Primary vtable of CTimer, checked on gEnv's timer before the voice clock reads it.
        std::uintptr_t timer_vtable = 0;
        /// Primary vtable of C_UISubtitles, checked before the subtitle interface is called.
        std::uintptr_t ui_subtitles_vtable = 0;
        /// C_JointPoseMorphPoseModifier::Execute (slot 5): turns face pose-bone weights into face joint motion.
        std::uintptr_t pose_morph_execute = 0;
        /// Primary vtable of CCharInstance, checked on every character before the morph hook uses it.
        std::uintptr_t character_vtable = 0;
        /// Primary vtable of CDefaultSkeleton, checked before a joint lookup.
        std::uintptr_t skeleton_vtable = 0;
        /// Primary vtable of Skeleton::CPoseData, checked before a pose weight is written.
        std::uintptr_t pose_data_vtable = 0;

        /// The console commands and the main-thread tick (gEnv, CXConsole, CSystem).
        bool commands = false;
        /// Entity lookups by name and id (ReplacesFor filters, speakers, faces).
        bool entities = false;
        /// Voice lines: Speak, its request helpers, actors and subtitles (needs entities).
        bool speech = false;
        /// A conversation line takes its speaker over from a framework line.
        bool dialog_handover = false;
        /// A save load stops the framework's lines and sounds (the save-load hook and the speaker's stop).
        bool load_stop = false;
        /// Voice-driven faces: the pose-morph hook and the character, skeleton and pose types (needs entities).
        bool lip_drive = false;
        /// Faces and subtitles pause with the game (otherwise they run on the wall clock).
        bool voice_clock = false;
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one, and gates each feature on its own anchors.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);
} // namespace SoundFramework

#endif // SNDFW_ANCHORS_HPP
