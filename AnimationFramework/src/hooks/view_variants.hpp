/**
 * @file hooks/view_variants.hpp
 * @brief Custom tag variants: options and transitions tagged with a custom tag play for Henry while the tag is on
 *        (custom_tags.hpp). `thirdPerson`, on while the camera shows him from outside, is the built-in one.
 *
 * The custom tags live in Henry's tag definition (kcd_male_tags.xml). Mannequin picks a fragment's option by matching
 * the scope's tags against each option's tags, the most specific match first, and CAnimationDatabase answers every such
 * query through GetBestEntry (the option a scope queues) and FindBestMatchingTag (the option CActionScope::IsDifferent
 * compares with it). Both are hooked and take the same decision for a query that carries the `player` bit (Henry's,
 * whose database male NPCs share) while custom tags are on: the game's own answer is looked up, then each combination
 * of the active tags in rank order (core/tag_rules.hpp), and the first answer that carries every tag of its combination
 * and covers every tag of the game's answer is taken. So `walk+player+thirdPerson` replaces the game's `walk+player`,
 * but never outranks an option the game prefers over it (`walk+backward+player`: without this rule, walking backward
 * plays the forward walk), and mods never need custom-tagged copies of the game's other options. The tags never enter
 * the game's own tag state, only the query copy.
 * Transitions follow the same rule through FindBestBlends: the variant chosen with custom tags must carry them and
 * cover the game's own variant in both tag states. That query's tag states get the bits for each call and lose them
 * again after.
 */
#ifndef ANIMFW_HOOKS_VIEW_VARIANTS_HPP
#define ANIMFW_HOOKS_VIEW_VARIANTS_HPP

#include "anchors.hpp"

#include <DetourModKit/hook.hpp>

namespace AnimationFramework
{
    /**
     * @brief Hooks GetBestEntry and FindBestMatchingTag (both or neither, so the two never disagree), then
     *        FindBestBlends and CActionScope::IsDifferent when they resolved.
     * @note Setup only. Best-effort: without both option anchors nothing is installed and every view plays the same
     *       options. Without FindBestBlends, transitions keep the game's pick. Without IsDifferent, a tag change while
     *       Henry stands still takes effect at his next move.
     * @throws std::bad_alloc If a hook name or a log line cannot allocate. The hooks armed by then stay armed, and the
     *         option detours pass every query through until both option hooks are armed.
     */
    void install_view_variant_hooks(const Anchors &anchors, DMK::hook::HookStack &hooks);

} // namespace AnimationFramework

#endif // ANIMFW_HOOKS_VIEW_VARIANTS_HPP
