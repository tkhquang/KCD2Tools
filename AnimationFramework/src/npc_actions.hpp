/**
 * @file npc_actions.hpp
 * @brief Mannequin fragments played on NPCs: the game's human:PlayAnim / StopAnim, opened to NPCs.
 *
 * The script bind entity.human:PlayAnim(fragment, tags) queues a Mannequin action for the fragment on the actor's own
 * action controller (priority 5, the fragment tags from `tags`, global tags set on the context), and StopAnim() ends
 * it. Both serve Henry only (the bind checks IActor::IsPlayer). Nothing past that check is player-specific, so the
 * framework runs the same play / stop for any other human actor and leaves Henry's call to the bind:
 *
 *   npc.human:PlayAnim("MyFragment", "")
 *   npc.human:StopAnim()
 *
 * PlayAnim plays on the NPC. A one-shot ends by itself, and a loop runs until StopAnim. No AI is involved: the NPC's
 * own schedule takes the controller back once the action ends. The fragment has to be in the NPC's animation database
 * (male NPCs share Henry's kcd_male_* files, and women have their own).
 */
#ifndef ANIMFW_NPC_ACTIONS_HPP
#define ANIMFW_NPC_ACTIONS_HPP

#include "anchors.hpp"

#include <DetourModKit/hook.hpp>

namespace AnimationFramework
{
    /**
     * @brief Hooks the PlayAnim / StopAnim binds so they also play on NPCs. Without both anchors (or with @p enabled
     *        false) the binds stay Henry's.
     * @note Setup only.
     */
    void install_npc_actions(const Anchors &anchors, bool enabled, DMK::hook::HookStack &hooks) noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_NPC_ACTIONS_HPP
