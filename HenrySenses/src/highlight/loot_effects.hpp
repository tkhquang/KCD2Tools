/**
 * @file highlight/loot_effects.hpp
 * @brief Particle effects shown on highlighted objects (a highlight group's Effect).
 *
 * An effect is a legacy (pfx1) particle effect: one of the game's own (Libs/Particles/<library>.xml) or one of the
 * optional particle library beside the ASI (KCD2_HenrySenses.particles.xml, effects "HenrySenses.<name>", parsed by the
 * game's XML parser and registered with IParticleManager::LoadLibrary before every batch that loads one of them, since
 * a level unload drops it; without the file those effects are off).
 *
 * An entity (every loot-scan target: bodies, items, containers, creatures, class and name patterns) gets its emitter
 * in a new slot of its render proxy, the way the game's Particle.SpawnEffect attaches one to an entity:
 * CRenderProxy::LoadParticleEmitter (primed, never serialized) and CRenderProxy::SetSlotLocalTM, which sits it on top
 * of the entity's bounds, emitting upwards. The entity owns the emitter, so it follows a walking NPC, and a
 * streamed-out proxy, a destroyed entity or a level unload takes it along; the mod frees only a slot it can prove is
 * still its own (the same entity, the same render proxy, its emitter still in the slot). Slot commands run at once or
 * later from the engine's deferred queue, in order, so a slot is freed only after its emitter has shown up (or a grace
 * period has passed), never ahead of its own pending load.
 *
 * An object without an entity slot (interactables, herb clusters, static objects a Model pattern matches) gets a
 * free-standing emitter on top of its bounds (CParticleManager::CreateEmitter). The particle manager owns it, and a
 * level unload destroys it without notice, so the mod kills one (IParticleEmitter::Kill) only after finding it in the
 * manager's live emitter list with the location it was created at.
 *
 * Every function here runs on the game main thread.
 */
#ifndef HENRYSENSES_LOOT_EFFECTS_HPP
#define HENRYSENSES_LOOT_EFFECTS_HPP

#include "game_structures.hpp"
#include "highlight/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace HenrySenses
{
    /**
     * @enum EffectChannel
     * @brief Which source a set of effects follows; each is replaced on its own.
     */
    enum class EffectChannel : std::uint8_t
    {
        /// The loot scan's entities (sync_loot_effects).
        Loot,
        /// Interactables, herb clusters and static objects (sync_world_effects).
        World,
    };

    /**
     * @struct EffectTarget
     * @brief One object of the world channel that should show an effect.
     */
    struct EffectTarget
    {
        /// Identity of the object within the channel, stable from one refresh to the next.
        std::uint64_t key{0};
        /// World bounds the effect sits on top of.
        game_structures::Aabb bounds{};
        /// intern_effect_name id.
        std::uint16_t effect{0};
        float scale{1.0f};
    };

    /**
     * @brief Brings the loot channel in line with the latest highlighted set.
     * @details Every record whose group has an Effect and still shows at least 30 percent of its fade gets one
     *          emitter in its entity's slot (a free-standing one on the entity's bounds when it has no render proxy);
     *          an attached one no longer wanted (gone from the set, faded, or its Effect or EffectScale changed) is
     *          released.
     * @param batch The set apply_batch() just applied.
     */
    void sync_loot_effects(std::span<const HighlightRequest> batch) noexcept;

    /**
     * @brief Brings the world channel in line with @p targets (free-standing emitters).
     * @param targets The objects that should show an effect now (the caller applies the 30 percent fade cut).
     */
    void sync_world_effects(std::span<const EffectTarget> targets) noexcept;

    /** @brief The share of its group's fade below which an object's effect is released. */
    [[nodiscard]] float effect_min_intensity() noexcept;

    /**
     * @brief Frees the released slots whose load has since shown up or whose grace period ended.
     * @note Every tick; returns at once when nothing waits.
     */
    void maintain_loot_effects() noexcept;

    /**
     * @brief Releases the effects of one channel.
     * @param channel The channel.
     * @param immediate Free every slot now, a load still pending included (shutdown); otherwise a slot whose load is
     *                  still pending is freed by maintain_loot_effects() once it has run.
     */
    void clear_loot_effects(EffectChannel channel, bool immediate) noexcept;

    /** @brief Releases the effects of every channel (a level change, a refused teardown). */
    void clear_all_loot_effects(bool immediate) noexcept;

    /**
     * @brief Frees every effect now and drops the mod's reference to its particle library (the particle manager keeps
     *        its own until a level unload).
     * @note At shutdown.
     */
    void shutdown_loot_effects() noexcept;

    /**
     * @brief Returns the number of emitters the mod still holds (attached or waiting to be freed).
     * @return The count; non-zero keeps the controller's full tick running.
     */
    [[nodiscard]] std::size_t loot_effect_count() noexcept;

    /** @brief Logs every attached effect and the counters (the state report). */
    void log_loot_effects_state();

} // namespace HenrySenses

#endif // HENRYSENSES_LOOT_EFFECTS_HPP
