/**
 * @file render_occlusion.hpp
 * @brief Render-node overhead clamp: keep the third-person camera out of render-only roofs.
 *
 * Some KCD2 roofs (tent / awning canopy cloth) are CBrush render meshes with no ray-collidable
 * physics, so the physics camera collision (RWI fan / PWI sphere) glides through them and the cloth
 * buries the camera on a look-down. The renderer sees them via I3DEngine::GetObjectsInBox; this
 * module queries the render octree along the pivot->camera arm and reports the maximum camera
 * distance before an OVERHEAD brush, so the camera stays just below the roof. Every engine call is
 * SEH-guarded and degrades to "no clamp" so a layout or version drift can never crash the game.
 */
#ifndef TPVCAMERA_RENDER_OCCLUSION_HPP
#define TPVCAMERA_RENDER_OCCLUSION_HPP

#include "math_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace TPVCamera
{

    /**
     * @brief Resolves I3DEngine::GetObjectsInBox and the p3DEngine slot.
     * @details Best-effort: on a miss the render clamp simply no-ops (the camera still works), so callers
     *          treat a false return as "render occlusion unavailable".
     * @param module_base Base address of the scanned game module (WHGame.dll); the AOB cascade search range.
     * @param module_size Size of the scanned module image, in bytes.
     * @param g_env Resolved SSystemGlobalEnvironment base; p3DEngine = g_env + GENV_3DENGINE_OFFSET.
     * @return true if the query function was located.
     */
    [[nodiscard]] bool initialize_render_occlusion(uintptr_t module_base, size_t module_size, uintptr_t g_env);

    /**
     * @brief Maximum camera distance along @p to_camera before a render-only obstruction occludes the character.
     * @details Queries the render octree (GetObjectsInBox) in a box bounding the pivot->camera arm and, for each
     *          visible (not ERF_HIDDEN) compact CBrush, ray-marches its render-mesh vertices against the
     *          pivot->camera sightline to find the nearest distance at which its cloth lies on the view ray. A
     *          brush counts only when at least RENDER_OCCLUSION_MIN_COLUMN_VERTS of its vertices fall inside the
     *          sightline tube, so a thin beam / rope / rail does not jolt the camera while a canopy clamps. No
     *          sightline tube, so a thin beam / rope / rail does not jolt the camera while a canopy clamps.
     *          When @p cov_thresh > 0 AND the live player AABB is available (PlayerScreenBounds), a brush is
     *          additionally dropped when its measured coverage of the player's REAL on-screen silhouette is
     *          below @p cov_thresh -- so a thin prop the body is plainly visible past (a pole, a bird feeder)
     *          no longer clamps the camera, while a view-burying canopy (which covers the player on a look-down)
     *          still does. This is the render-side use of the coverage gate (UseRenderOcclusion respecting
     *          CoverageCollision). It is keyed on the REAL projected player extent, not the historical synthetic
     *          box (which sat below an overhead canopy and read ~0); an unreadable mesh keeps the clamp.
     *          Returns the nearest qualifying limit, or std::nullopt when the renderer is unavailable or nothing
     *          on the sightline occludes the view. SEH-guarded; a fault returns std::nullopt.
     * @param pivot Camera arm start (inside the player), world space.
     * @param to_camera Pivot->camera vector; its length is the desired follow distance (not normalized).
     * @param radius Standoff kept below the roof underside (the collision radius), meters.
     * @param cov_thresh Coverage gate threshold (0..1); 0 disables the gate (sightline test only, prior behavior).
     */
    [[nodiscard]] std::optional<float> render_occlusion_limit(const Vector3 &pivot, const Vector3 &to_camera,
                                                              float radius, float cov_thresh);

    /**
     * @brief Fraction (0..1) of the character occluded by the VISIBLE render brush at a physics hit point.
     * @details The physics camera-collision proxy can be far broader than the thin visible mesh (e.g. a
     *          clothes-line / tripod frame whose collision hull is a big box), so the physics ray-based coverage
     *          (@ref character_occluded_fraction) over-reports. This looks up the COMPACT, visible CBrush whose
     *          world AABB contains @p hit_point and returns its triangle-rasterized character coverage -- the
     *          true visible occlusion. Returns < 0 ("no opinion, use physics") when no compact, non-HLOD,
     *          readable-mesh brush yields a measurement: nothing compact contains the point (terrain / pure-
     *          physics body), the only candidates are HLOD proxies, or the hit brush is a compound building whose
     *          ROOT mesh is null. The caller then falls back to the physics rays so large solids still block.
     *          SEH-guarded; a fault returns < 0.
     * @param hit_point World-space physics hit position (use the fan/RWI hit, not the synthetic sphere point).
     * @param pivot Camera arm start (inside the player), world space.
     * @param to_camera Pivot->camera vector; the coverage is measured from the desired camera = pivot + to_camera.
     * @param out_node Optional; receives the resolved dominant occluder render node (or nullptr). The caller can
     *        cache it and re-measure later via @ref render_coverage_of_brush WITHOUT re-running the octree query
     *        (GetObjectsInBox, ~110 us) -- the node is stable for a given collider, so this removes the octree
     *        from the per-frame re-measure path. nullptr on no measurement or a fault.
     * @param out_head Optional; receives the HEAD band's silhouette fill (0..1) of the dominant occluder, or -1
     *        if unmeasurable, for the head-visible collision gate.
     */
    [[nodiscard]] float render_coverage_at(const Vector3 &hit_point, const Vector3 &pivot, const Vector3 &to_camera,
                                           void **out_node = nullptr, float *out_head = nullptr);

    /**
     * @brief Fraction (0..1) of the character the SPECIFIC render brush @p node occludes from @p camera.
     * @details Measures the brush the physics ray ACTUALLY hit (resolved via @ref static_brush_render_node), not
     *          whatever brush a box happens to contain, so a thin foreground prop is measured as itself rather
     *          than masking the solid behind it. Validates @p node is a non-HLOD CBrush with a readable mesh and
     *          triangle-rasterizes it onto the character silhouette. Returns < 0 ("unmeasurable") for a null /
     *          non-brush / HLOD / compound (null root mesh) node, which the caller treats as a SOLID occluder
     *          (a building / pure-physics body the camera must not see the body through). SEH-guarded.
     * @param node The IRenderNode (CBrush) the hit belongs to.
     * @param pivot Camera arm start (inside the player), world space.
     * @param camera The DESIRED camera position the coverage is measured from (pivot + to_camera).
     * @param out_head Optional; receives the HEAD band's silhouette fill (0..1), or -1 if unmeasurable, for the
     *        head-visible collision gate.
     */
    [[nodiscard]] float render_coverage_of_brush(void *node, const Vector3 &pivot, const Vector3 &camera,
                                                 float *out_head = nullptr) noexcept;

    /**
     * @brief Best-effort identity (.cgf name + world AABB extents) of the brush at a physics camera-collision hit.
     * @details For trace logging only: turns an opaque collider address into the object the camera blocked on, so a
     *          false positive (a thin prop, a wall, a roof) is identifiable from the log WITHOUT live debugging. When
     *          @p node is non-null (the collider's foreign render-node link, via static_brush_render_node) it is read
     *          directly; otherwise the render octree is queried at @p hit_point and the compact brush whose mesh is AT
     *          the hit (preferred) or that merely contains it (fallback, e.g. a compound building) is reported.
     *          SEH-guarded; returns false and leaves @p out_name empty / @p ext zeroed on a fault or miss.
     * @param hit_point World-space physics hit position (the fan / RWI hit, not the synthetic sphere point).
     * @param node The collider's foreign render node, or nullptr if it had none (merged / proxy collider).
     * @param out_name Receives the null-terminated .cgf path, truncated to @p out_sz.
     * @param out_sz Size of @p out_name in bytes.
     * @param ext Receives the brush world AABB extents (x, y, z) in meters, or zeros.
     * @param out_kind Receives which path identified the brush (nullptr to ignore): 0 none, 1 foreign-linked node,
     *        2 prop (a brush whose MESH is at the hit -- the coverage gate would measure it), 3 solid (a brush that
     *        only CONTAINS the hit, mesh not within COVERAGE_HIT_EPS -- the gate skips it), 4 hlod proxy. With the
     *        logged cov this pinpoints why a thin prop reads as an unmeasurable solid (kind=prop + cov<0 => the mesh
     *        read failed; kind=solid + cov<0 => the prop's mesh was not found near the physics hit point).
     */
    [[nodiscard]] bool render_hit_info(const Vector3 &hit_point, void *node, char *out_name, int out_sz, float ext[3],
                                       int *out_kind) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_RENDER_OCCLUSION_HPP
