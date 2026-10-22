/**
 * @file render_occlusion.hpp
 * @brief Render-octree occluder scan: the visible objects between the third-person camera and the character.
 *
 * The camera fades objects that hide the character instead of pulling in front of them (see hooks/occluder_fade.hpp).
 * This module finds them in the render octree (I3DEngine::GetObjectsInBox, octree nodes only: a node flagged
 * ERF_RENDER_ALWAYS sits in the engine's always-visible list and is never returned). It keeps a cached list of the
 * brushes, vegetation and entities around the camera arm, each classed as a structure (stops the camera) or a prop
 * (passed through and faded) by the camera rules (core/rules/camera_rules.hpp), matches a physics collider to the node
 * it belongs to (so the camera collision can step past a fence post instead of stopping at it), tests sightlines from
 * the camera to the character against each prop's render mesh, and names the trees, bushes and grass in front of the
 * character for the tree see-through (see hooks/shader_twins.hpp). Every engine call is SEH-guarded and degrades to
 * "nothing found", so a layout or version drift can never crash the game.
 */
#ifndef TPVCAMERA_RENDER_OCCLUSION_HPP
#define TPVCAMERA_RENDER_OCCLUSION_HPP

#include "core/collision/probe_types.hpp"
#include "core/rules/camera_rules.hpp"
#include "core/view/view_box.hpp"
#include "math_utils.hpp"
#include "query_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace TPVCamera
{

    /**
     * @brief Supplies the current player's render nodes (his own and his attachments'), which the scan classes as the
     *        character. The span is stored as given, so its storage must remain valid until the next call.
     * @note Main camera thread only, before the occluder scan / collision queries.
     */
    void set_occluder_player_nodes(std::span<const std::uintptr_t> nodes) noexcept;

    /**
     * @brief Resolves I3DEngine::GetObjectsInBox and the p3DEngine slot.
     * @details Best-effort: on a miss the scan finds nothing (the camera still works and collides as plain physics),
     *          so callers treat a false return as "render octree unavailable".
     * @param module_base Base address of the scanned game module (WHGame.dll); the AOB cascade search range.
     * @param module_size Size of the scanned module image, in bytes.
     * @param g_env Resolved SSystemGlobalEnvironment base, or 0 when it did not resolve; p3DEngine =
     *              g_env + GENV_3DENGINE_OFFSET.
     * @return true if the query function and g_env were located.
     */
    [[nodiscard]] bool initialize_render_occlusion(uintptr_t module_base, size_t module_size, uintptr_t g_env);

    /**
     * @brief Frees the vector the occluder scan's octree queries collect into, through the engine's deallocator that
     *        matches the allocator it was grown with, and returns the scan to counting and filling.
     * @note Call once no camera frame can run any more (shutdown, after the hooks retired).
     */
    void release_render_occlusion() noexcept;

    /**
     * @brief Refreshes the cached list of visible brushes, vegetation and entity proxies around @p points.
     * @details Queries the octree in the box around the points plus OCCLUDER_SCAN_MARGIN, keeping the nodes that are
     *          drawn (not ERF_HIDDEN) and that no camera rule excludes, each with its rule's verdict and Rules::Behaviour.
     *          The list is reused while every point stays inside the scanned box, it is younger than
     *          OCCLUDER_SCAN_MAX_AGE of real time, the rule set in force is the one it was classified with and no
     *          scanned brush changed its model, model revision or material. A reuse refreshes the boxes of entities and
     *          movable brushes and the entities' verdicts, drops the nodes freed or hidden since, and re-runs each
     *          rule's check. A scan that held more nodes than it keeps is never reused. Without a rule set, or with a
     *          non-finite point, the list is empty. Main thread only.
     * @param points The camera arm and the character points (pivot, desired camera, sightline targets).
     * @param n_points Count of @p points.
     */
    void refresh_occluder_scan(const Vector3 *points, int n_points);

    /**
     * @brief The scanned render node a physics collider belongs to, or nullptr.
     * @details A runtime-physicalized brush or tree names its node in the collider's foreign data, which is used as is.
     *          Otherwise baked level geometry (the static foreign id with no link) is matched by its world box
     *          (CPhysicalEntity m_BBox): it belongs to the smallest scanned node that holds at least half of it, so a
     *          wall hit never matches a small window frame on the wall. Trees, bushes, grass and characters never take
     *          a box match. An entity's physics names its CEntity, and matches the scanned entity proxy that entity
     *          owns. Any other collider returns nullptr.
     * @param collider The RayHit::m_collider of a hit (the IPhysicalEntity pointer).
     * @param cls Receives the node's class; left untouched when nullptr is returned. May be nullptr.
     */
    [[nodiscard]] void *occluder_node_for_collider(uintptr_t collider, Rules::Behaviour *cls) noexcept;

    /**
     * @brief Whether a man-made structure is drawn where the physics of scanned structure @p node was hit.
     * @details False only when @p node is a man-made structure whose drawn mesh can be read and none of @p lines (short
     *          segments through the hit along the probe's lines) crosses it or the drawn mesh of another scanned
     *          man-made structure: its collision stands there with nothing drawn, like the canvas walls of a tent model
     *          that draws only its frame. A house whose interior model owns the collision of the walls its exterior
     *          model draws is drawn there. Hidden and shadow-proxy sub-meshes are not drawn. A natural structure, a
     *          mesh that cannot be read, a test that runs out of triangle budget and a node the scan does not hold
     *          count as drawn, so their physics keeps stopping the camera. Main thread only.
     * @param tag Why the test runs, for the query ledger (each mesh test counts as one query).
     */
    [[nodiscard]] bool structure_drawn_on(QueryLedger::Tag tag, void *node, const Sightline *lines,
                                          int n_lines) noexcept;

    /**
     * @brief Whether the drawn mesh of render node @p node spans a tube of five parallel lines both ways: one of its
     *        two side lines and one of its two lift lines cross it, so a post or a rail thinner than the tube does not.
     *        True when the mesh cannot be read. Hidden and shadow-proxy sub-meshes are not drawn.
     * @param tube The centre line, the lines offset to either side, then the lines offset up and down (the order of
     *        the camera arm's lines).
     * @note Main thread only.
     */
    [[nodiscard]] bool spans_tube(void *node, const Sightline *tube) noexcept;

    /**
     * @brief Collects the scanned bare frames (Rules::Behaviour::Frame) whose world box comes within @p reach meters of
     *        @p point, for the near cut (see hooks/shader_twins.hpp).
     * @details A broad phase like trees_in_window(): the shader decides per pixel, so no mesh is read. Main thread
     *          only.
     * @param point The camera.
     * @param reach How close a frame's box must come (meters).
     * @param out Receives the frame render nodes, in scan order.
     * @param cap Size of @p out.
     * @return The number of nodes written.
     */
    [[nodiscard]] int frames_near(const Vector3 &point, float reach, void **out, int cap) noexcept;

    /** @brief A drawn part of a person or an animal in front of the lens: its body, or something it carries. */
    struct LensActorPart
    {
        void *node = nullptr;       // the part's render proxy
        std::uintptr_t entity = 0;  // the part's CEntity, which its render proxy names
        std::uintptr_t actor = 0;   // the CEntity of the actor it is or belongs to
        std::uint32_t actor_id = 0; // that actor's EntityId
        float depth = 0.0f;         // view depth of the nearest part of its box inside the view (box_depth_in_view())
        float bbox[6] = {};         // its world AABB {min.xyz, max.xyz}
    };

    /**
     * @brief The scanned parts of people and animals whose boxes show in @p view within @p reach meters of depth.
     * @details A part is an entity render proxy the scan classes as an actor (the actor itself or an item bound to one)
     *          that the camera rules leave to the camera as a character, and each is tied to its actor through the
     *          actor registry and the attachment parents. The player's own parts (@p player_entity and what it
     *          carries) are left out. The depth is exact for the box (box_depth_in_view()); a box is looser than the
     *          body it holds, by up to a limb's reach. Main thread only (uses the occluder scan).
     * @param cap Size of @p out.
     * @return The number of parts written, in scan order.
     */
    [[nodiscard]] int actors_before_lens(const ViewAxes &view, float reach, std::uintptr_t player_entity,
                                         LensActorPart *out, int cap) noexcept;

    /**
     * @brief Whether a scanned node that stops a camera probe (Rules::Behaviour::Collide, as classify_probe_hit()
     *        reads it) has a box within @p pad of box @p box: the broad phase the lens asks before its rays.
     * @details True without a valid scan, so the rays then run. The scan covers the arm and the camera with its margin,
     *          so a collider near the camera that matches no node lies in the scanned box as well. Main thread only.
     * @param box {min.xyz, max.xyz}.
     */
    [[nodiscard]] bool stopping_node_near(const float *box, float pad) noexcept;

    /**
     * @brief Where @p lines meet the drawn mesh of a man-made structure.
     * @details A building's drawn mesh can stand inside its physics: a thatched roof's physics is its outer surface
     *          while the drawn thatch hangs below it, so a camera the physics lets through can still sit inside the
     *          thatch, and the cloth canopies over market stalls have no physics at all. The lines are tested
     *          two-sided against the render meshes of the man-made structures (houses, sheds, tents, canopies, their
     *          wall pieces) whose box at least @p min_lines of them enter, the first short of @p max_fraction, and a
     *          structure counts only when at least @p min_lines of the lines cross it. The ground, banks and rocks are
     *          drawn where their physics is and are left to it, as is a mesh that cannot be read. Main thread only.
     * @param tag Why the test runs, for the query ledger (each mesh test counts as one query).
     * @param lines The camera arm, as parallel lines of equal length (the centre and its offsets by the radius), or
     *        rays from the camera of the same reach in view depth.
     * @param n_lines Count of @p lines (up to 9).
     * @param min_lines How many lines must cross a structure for it to count: 3 for the arm, so a guy rope or a chain
     *        with no physics, which crosses one line at a time, does not stop the camera.
     * @param max_fraction How far along the lines a crossing can still matter (the physics already stops the camera
     *        there): the lines are tested only that far, so a structure they enter only past it is not tested.
     * @param node_out Receives the structure that stops the arm, or nullptr; may be nullptr.
     * @param trace Whether to log the structures tested at Trace when that set changes (the camera arm's call only,
     *        so other callers do not make the line alternate).
     * @return Where the @p min_lines-th line meets the nearest structure that counts (the line that made it count),
     *         as a fraction of the line length (0..1), or a value above 1 when none does.
     */
    [[nodiscard]] float structure_on_arm(QueryLedger::Tag tag, const Sightline *lines, int n_lines, int min_lines,
                                         float max_fraction, void **node_out, bool trace = false) noexcept;

    /**
     * @brief Measures ahead the render mesh of a scanned man-made structure, prop, frame or fence that @p lines are
     *        about to reach.
     * @details The first test of a mesh reads all of its positions (the engine decodes its whole vertex stream) and
     *          records its triangle-run boxes, which costs milliseconds for a house. Of those nodes whose box, grown by
     *          OCCLUDER_MESH_PREFETCH_MARGIN, @p lines pass through, the one they enter first with a mesh not measured
     *          yet is measured now, one mesh per call, and only when no test measured a mesh since the previous call,
     *          so the arm and sightline tests find it measured and two first reads never fall in one frame. The result
     *          of every camera test stays the same; only the frame the first read lands in changes. A fence (a rule
     *          with if-wall=) is measured whatever its class, since its wall check reads only measured meshes. Main
     *          thread only, once per camera frame, after that frame's tests.
     * @param lines The camera arm (the centre and its offsets), as for structure_on_arm.
     * @param n_lines Count of @p lines (up to 9).
     */
    void prefetch_occluder_meshes(const Sightline *lines, int n_lines) noexcept;

    /**
     * @brief Collects the scanned props whose render mesh crosses any of @p lines, and the props and bushes the camera
     *        is buried in.
     * @details Structures are never collected: the camera collision keeps the camera out of them, and a building
     *          corner that hides part of the character is left as it is. Nor are plants for crossing a line: a tree, a
     *          bush or a grass patch in front of the character opens the tree see-through window instead (see
     *          trees_in_window), so a leaf on a sightline never fades a whole plant. A prop is tested only when a line
     *          passes through its world box; then each line is moved into the mesh's local space and tested against
     *          its triangles (the root render mesh, or each sub-mesh of a compound statobj). Crossings within
     *          @p end_margin of a line's end (the character) do not count. A crossed prop is collected only where it is
     *          wide: a tube of 0.15 m half-width around the crossed line must meet it both across and up and down, so a
     *          post or a rail thinner than the tube stays. A prop brush whose mesh cannot be read
     *          counts on its box alone; an entity counts only on an exact crossing, and not at all when its box comes
     *          within 0.6 m of a line's end (what the character carries or stands at). A prop or a bush (a plant
     *          outside the trees folder) placed as a brush or vegetation that the camera is buried in is collected
     *          too, marked buried, whether or not it hides the character: its box holds the camera and its mesh meets
     *          at least five of six 2 m probes along the world axes. What only touches the lens is left to the near
     *          clip plane, so a branch at the lens never fades a whole tree. Main thread only.
     * @param lines The sightlines, each from the camera. Only the first Constants::OCCLUDER_FADE_SIGHTLINES are read.
     * @param n_lines Count of @p lines.
     * @param end_margin Meters at each line's character end where a crossing does not count (OccluderFadeBodyMargin):
     *        whatever the character holds, wears or stands in is not between him and the camera.
     * @param skip Nodes the caller already fades at once because the camera is inside their physics. Their meshes are
     *        not read: from inside, every sightline and probe meets nearly every triangle, and the answer would not
     *        change how they fade.
     * @param out Receives the nodes.
     * @param cap Size of @p out.
     * @return The number of nodes written.
     */
    [[nodiscard]] int occluders_on_sightlines(const Sightline *lines, int n_lines, float end_margin,
                                              std::span<void *const> skip, SightlineOccluder *out, int cap) noexcept;

    /**
     * @brief Collects the scanned trees, bushes and grass patches whose world box meets the solid between the quads
     *        @p near_end and @p far_end.
     * @details The broad phase of the tree see-through (see hooks/shader_twins.hpp). @p far_end is the window on the
     *          character's vertical plane facing the camera and @p near_end the same rectangle at the camera (a
     *          tube), or four copies of the camera (a pyramid). A node is listed unless its box lies wholly outside the
     *          solid's own box or outside one of its planes (the four sides and the two ends). The test is
     *          conservative: a box beside an edge can be listed without meeting the solid, which shows nothing, since
     *          the shader opens only the pixels inside the window and nearer than the character. No mesh is read and
     *          nothing is measured for coverage. Only plants are listed (Rules::Behaviour::Tree,
     *          Rules::Behaviour::Bush and Rules::Behaviour::Grass): props keep the per-object fade. The nodes whose box
     *          the centre line from the near end to the window passes through come first, so when more than @p cap
     *          meet the solid, the ones straight between the camera and the character are kept. Main thread only.
     * @param near_end The near end's corners, in the same order as @p far_end.
     * @param far_end The window's corners, in order around it.
     * @param out Receives the plant render nodes: those on the centre line first, each group in scan order.
     * @param cap Size of @p out.
     * @return The number of nodes written.
     */
    [[nodiscard]] int trees_in_window(const Vector3 (&near_end)[4], const Vector3 (&far_end)[4], void **out,
                                      int cap) noexcept;

    /**
     * @brief Reads a physics entity's world box (CPhysicalEntity m_BBox) into @p out {min.xyz, max.xyz}.
     * @return false on a fault or an implausible box. SEH-guarded.
     */
    [[nodiscard]] bool read_collider_box(uintptr_t collider, float *out) noexcept;

    /**
     * @brief Copies the .cgf path of a render node's mesh, for logging. Empty when it cannot be read.
     * @param node The IRenderNode.
     * @param out Receives the null-terminated path, truncated to @p cap.
     * @param cap Size of @p out in bytes.
     */
    void render_node_name(void *node, char *out, int cap) noexcept;

    /**
     * @brief What @p node is beyond its address: its type with its model (a brush), its entity id (an entity proxy) or
     *        where it stands (vegetation). A node freed and built again at the same address for another object reads
     *        a different value; one that moves or changes its mesh keeps its own.
     * @return The identity, or 0 for a null or unreadable node. SEH-guarded.
     */
    [[nodiscard]] std::uint64_t render_node_identity(void *node) noexcept;

    /**
     * @brief Advances at most one cached-mesh fence coverage row. Call once per game frame, never per view build.
     * @return True when a scanned fence changed class, or the scan took a new rule set, and the camera's cached
     *         collision/fade must be recomputed.
     */
    [[nodiscard]] bool advance_fence_classification() noexcept;

    /**
     * @brief Where the camera rule that classed scanned node @p node is written ("file:line"), for the log.
     * @details Empty when @p node is not in the current scan. Main thread only (reads the scan).
     * @param out Receives the null-terminated location, truncated to @p cap.
     */
    void occluder_rule_where(void *node, char *out, int cap) noexcept;

    /**
     * @brief The generation of the rule set the current scan was classified with, or 0 without one.
     * @note Main thread only (reads the scan).
     */
    [[nodiscard]] std::uint64_t occluder_rules_generation() noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_RENDER_OCCLUSION_HPP
