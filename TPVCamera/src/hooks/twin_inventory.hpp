/**
 * @file hooks/twin_inventory.hpp
 * @brief The plant and frame materials of the loaded level, read from the engine's own level tables at a loading
 *        boundary, so the shader twins can prepare a twin for every plant and frame variant the level can draw.
 *
 * A level load creates every vegetation group, brush model and brush material of the level whatever the player's
 * distance (CTerrain::Load_T waits for its async loaders before it returns), and the engine keeps them in three tables
 * until the level unloads: CObjManager's vegetation groups, and C3DEngine's level statobj and brush material tables.
 * The shader twins (hooks/shader_twins.hpp) see a material only when it loads during a loading screen, so a
 * same-level save load, or the first load after a DEV hot reload, loads none of the level's cached materials, and
 * their plants stay solid without this inventory. This module walks those tables on the main thread inside the loading
 * finalizer (or, the first time the game stands still after a DEV hot reload or after a camera rules edit, in that
 * paused frame) and hands back every material of a plant or bare-frame asset with the scope the camera rules give its
 * asset path (camera_rules.hpp), so the finalizer can queue a twin for each of their shader items, with the gen masks
 * the engine itself computed (mods included).
 *
 * Reads only: no engine call, no engine lock, every read SEH-guarded and bounded. The tables are read fresh on every
 * call; a null table (the main menu, a level unloading) means there is no level.
 */
#ifndef TPVCAMERA_HOOKS_TWIN_INVENTORY_HPP
#define TPVCAMERA_HOOKS_TWIN_INVENTORY_HPP

#include <cstdint>
#include <vector>

namespace TPVCamera
{

    /**
     * @brief The scopes the camera rules in force give an asset by its path (a .cgf, or a material name, which follows
     *        its asset's folder): Rules::RuleSet::twin_scopes().
     * @details A preparation superset only: the union over every way a model with this path can be placed, while the
     *          exact render-node verdict and its check still gate each swap. The path is copied, lowered and given '/'
     *          slashes under SEH first (512 bytes at most), so a string the engine is freeing reads as no scope.
     * @return The camera rules' twin scope bits: Rules::TWIN_PLANTS (the trees, bushes and grass the camera lists),
     *         Rules::TWIN_FRAMES (tent beams and poles) and Rules::TWIN_CHARACTER (a broader preparation policy than
     *         the camera's own character test; only the player and his carried items ever swap). 0 for a path no rule
     *         prepares, a null one, a longer one, a fault, or no rule set.
     */
    [[nodiscard]] std::uintptr_t twin_asset_scopes(const char *path) noexcept;

    /**
     * @brief The scopes of @p material (a CMatInfo, or 0) by its name (Constants::MATERIAL_NAME_OFFSET).
     * @note Read only while the caller's callback owns the material, or on the main thread in the loading finalizer for
     *       a material the level tables hold.
     */
    [[nodiscard]] std::uintptr_t twin_material_scopes(std::uintptr_t material) noexcept;

    /** @brief One drawn material of a plant or frame asset: a CMatInfo with no sub-materials, and its scopes. */
    struct TwinInventoryMaterial
    {
        std::uintptr_t material = 0;
        std::uintptr_t scopes = 0; // Rules::TWIN_PLANTS and/or Rules::TWIN_FRAMES, never 0
    };

    /** @brief What one walk of the level tables found, for the loading epoch's log line. */
    struct TwinInventoryStats
    {
        // False when there is no level (a table is null) or the walk could not start; reason then says why.
        bool level = false;
        const char *reason = "";
        int groups = 0;               // vegetation groups read
        int statobjs = 0;             // level statobj table entries read
        int children = 0;             // compound sub-objects read
        int brush_materials = 0;      // brush material table entries read
        int scoped_assets = 0;        // groups, statobjs, children and brush materials of a plant or frame asset
        int distinct_materials = 0;   // distinct (material, scopes) expanded
        int drawn_materials = 0;      // materials handed back
        int skipped_submaterials = 0; // sub-materials past Constants::TWIN_INVENTORY_MAX_SUBMATERIALS
        int not_materials = 0;        // pointers whose object is not a CMatInfo
        int faults = 0;               // guarded reads that faulted or found an implausible table
        bool dedupe_full = false;     // the dedupe set filled; later materials were expanded without it
        std::uint64_t micros = 0;     // the walk's duration
    };

    /**
     * @brief Walks the loaded level's tables and appends every drawn material of a plant or frame asset to @p out.
     * @details In order: each vegetation group (its statobj's material and the group's override material, scoped by
     *          the statobj's .cgf path or the group's file name), each statobj of the level statobj table and its
     *          compound sub-objects (scoped by their .cgf paths), and each material of the brush material table (scoped
     *          by its own name). A multi-material is expanded into its sub-materials (at most
     *          Constants::TWIN_INVENTORY_MAX_SUBMATERIALS). Only the plant and frame scopes are handed back: the
     *          character's twins come from his own material loads, and a path no rule prepares (an HLOD's levels/...)
     *          drops out on its own. The walk scopes every path with the one rule set in force when it starts. A
     *          (material, scopes) pair is expanded once per walk. Every table is
     *          checked against its class (C3DEngine, CObjManager by RTTI; each material as a CMatInfo) and bounded by
     *          the Constants::TWIN_INVENTORY_MAX_* limits; a table that fails is skipped and counted as a fault.
     * @param genv The SSystemGlobalEnvironment base (its p3DEngine at Constants::GENV_3DENGINE_OFFSET).
     * @param objman_global The global that holds the CObjManager pointer (AnchorId::VegetationGroups).
     * @param out Receives the materials; existing entries are kept.
     * @return What the walk found and how long it took.
     * @throws std::bad_alloc If @p out or the walk's dedupe set cannot allocate.
     * @note Main thread only, inside the loading finalizer or, once after a DEV hot reload or a camera rules edit, in a
     *       frame the game stands still in (service_shader_twins_paused()), where nothing writes the tables (they are
     *       written only while the level loads and released when it unloads). It makes no engine call and takes no
     *       engine lock; it loads the camera rules once (Rules::current()).
     */
    [[nodiscard]] TwinInventoryStats enumerate_level_twin_materials(std::uintptr_t genv, std::uintptr_t objman_global,
                                                                    std::vector<TwinInventoryMaterial> &out);

} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_TWIN_INVENTORY_HPP
