/**
 * @file rtti_types.hpp
 * @brief Cached identity handles for the WHGame.dll class vtables the mod tests against.
 *
 * Setup resolves each primary vtable through TypeIdentity and publishes an immutable table. Game callbacks
 * compare pointers against that table. The main tick checks the captured PE identity once per frame and revokes
 * the table if the image changes. No callback performs a cold RTTI resolve.
 */
#ifndef HENRYSENSES_RTTI_TYPES_HPP
#define HENRYSENSES_RTTI_TYPES_HPP

#include <DetourModKit/region.hpp>

#include <cstdint>

namespace HenrySenses
{
    /**
     * @enum GameClass
     * @brief The engine classes the mod identifies by vtable.
     */
    enum class GameClass : std::uint8_t
    {
        /// wh::entitymodule::C_Player, the local actor.
        Player,
        /// CEntity.
        Entity,
        /// CRenderProxy (primary base is the IRenderNode).
        RenderProxy,
        /// CEntitySystem, the IEntitySystem implementation.
        EntitySystem,
        /// CAuxGeomCB, the IRenderAuxGeom command buffer.
        AuxGeom,
        /// wh::game::C_CameraCombatDelegate, the active camera during combat.
        CameraCombat,
        /// wh::game::C_CameraDialog, the active camera during dialogue.
        CameraDialog,
        /// C3DEngine, the I3DEngine implementation.
        ThreeDEngine,
        /// CD3D9Renderer, the IRenderer implementation.
        Renderer,
        /// CStandardGraphicsPipeline, the game's main graphics pipeline.
        StdPipeline,
        /// CSceneCustomStage, the silhouette stage the mod registers.
        CustomStage,
        /// CSceneForwardStage, the forward stage whose after-HDR pass hosts the mask draw.
        ForwardStage,
        /// CMergedMeshRenderNode, a 16 m cell of batched small vegetation (grass, pickable herbs).
        MergedMeshNode,
        /// CBrush, a static world mesh (IRenderNode type 1).
        Brush,
        /// CCharInstance, an animated character model.
        CharInstance,
        /// COwnedBrush, a brush spawned and owned by an entity (runtime prefabs, procedural objects).
        OwnedBrush,
        /// CMovableBrush.
        MovableBrush,
        /// wh::entitymodule::C_NPCActor, a human NPC.
        NpcActor,
        /// wh::entitymodule::C_Horse.
        Horse,
        /// wh::entitymodule::C_Dog.
        Dog,
        /// wh::entitymodule::C_Animal, every other animal.
        Animal,
        /// wh::xgenaimodule::C_NPC, the AI of an actor (human or animal).
        AiNpc,
        /// wh::rpgmodule::C_Soul, an actor's RPG state.
        Soul,
        /// wh::entitymodule::C_Inventory.
        Inventory,
        /// wh::entitymodule::C_ItemSlot, a world slot an item can sit in.
        ItemSlot,
        /// wh::entitymodule::C_ItemSlotPile, a world slot holding a stack (bowls on a shelf).
        ItemSlotPile,
        /// wh::entitymodule::C_ItemVectorBorrower, the items an NPC borrowed for a task (bread in hand, eggs).
        ItemVectorBorrower,
        /// wh::entitymodule::C_ItemWrapper, an item that is a part of another item (a crossbow's goat's foot).
        ItemWrapper,
        /// wh::entitymodule::C_WorldInventory, the owner of every item dropped loose into the world.
        WorldInventory,
        /// wh::entitymodule::C_PickableItem, a world item.
        PickableItem,
        /// wh::entitymodule::C_Item, the data of an item.
        Item,
        /// CActorSystem.
        ActorSystem,
        /// CItemSystem.
        ItemSystem,
        /// wh::game::C_Game.
        Game,
        /// wh::game::C_GameModel.
        GameModel,
        /// wh::game::C_ScriptContextManager.
        ScriptContextManager,
        /// wh::entitymodule::C_EntityModule.
        EntityModule,
        /// wh::entitymodule::C_InventoryManager.
        InventoryManager,
        /// wh::shopmodule::C_ShopModule.
        ShopModule,
        /// wh::shopmodule::C_Shop.
        Shop,
        /// wh::rpgmodule::C_RPGModule.
        RpgModule,
        /// wh::entitymodule::C_Stash, the stash game-object extension.
        Stash,
        /// CXmlNode, a node of a parsed XML document.
        XmlNode,
        /// CXMLReadOnlyNode, a node of a compact read-only XML document (the prefab templates).
        XmlReadOnlyNode,
        /// CMergedMeshesManager, the 3D engine's index of the merged-mesh cells.
        MergedMeshesManager,
        /// CVegetation, a vegetation instance the engine does not merge (IRenderNode type 2; the pickable mushrooms).
        Vegetation,
        /// CCryPak, the ICryPak implementation (file access and path aliases).
        CryPak,
        /// CParticleEmitter, a legacy particle emitter (a loot effect in an entity slot).
        ParticleEmitter,
        /// CXmlUtils, CSystem's XML parser front end (the mod's particle library).
        XmlUtils,
        /// CParticleManager (its primary base; gEnv holds the IParticleManager base 8 bytes further).
        ParticleManager,
        /// Enumerator count. Not a class.
        Count,
    };

    /**
     * @brief Resolves every class identity over the game image.
     * @param image The WHGame.dll range. DMK::Region::host() is the game executable, and every class lives in
     *              WHGame.dll.
     * @details Publishes one immutable vtable per class. A repeated init preserves the table and checks its image.
     * @note Setup and control plane only. Call from init(), after the module range resolves and before any hook
     *       arms; queries before that answer false.
     */
    void init_game_types(DMK::Region image);

    /**
     * @brief Revalidates the flat vtable table vtable_is() compares against.
     * @details Reads the captured image's PE identity through guarded memory without a loader query or RTTI sweep.
     *          An image change disables all identity checks until the mod restarts.
     * @note Main thread, once per tick.
     */
    void refresh_game_types() noexcept;

    /**
     * @brief Checks whether @p vtable is the primary vtable of @p klass.
     * @param klass The class to test against.
     * @param vtable Runtime vtable pointer read from an object. Zero answers false.
     * @return True when the vtable belongs to that class. An identity that did not resolve at init never matches.
     * @note Callback-safe: a qword compare against the published table. An invalid table always answers false.
     */
    [[nodiscard]] bool vtable_is(GameClass klass, std::uintptr_t vtable) noexcept;

    /**
     * @brief Checks whether the object at @p object is an instance of @p klass (guarded vtable read).
     * @param klass The class to test against.
     * @param object Object address. Zero or an unreadable address answers false.
     * @return True when the object's primary vtable belongs to that class.
     */
    [[nodiscard]] bool object_is(GameClass klass, std::uintptr_t object) noexcept;

    /**
     * @brief Returns the resolved primary vtable of @p klass, for diagnostics.
     * @param klass The class.
     * @return The vtable address, or 0 before init_game_types() or when the identity did not resolve.
     */
    [[nodiscard]] std::uintptr_t class_vtable(GameClass klass) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_RTTI_TYPES_HPP
