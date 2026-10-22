/**
 * @file tuning.hpp
 * @brief The mod's own limits and timings: caps, budgets, margins and seconds for the collision walk, the occluder
 *        scan and fade, the character and NPC fades, the shader twins and the tree see-through.
 *
 * @details Chosen by the mod, not read from the game; each states why it has its value. Part of constants.hpp, which
 *          includes it; the engine layout they work with is in layout_offsets.hpp.
 */
#ifndef TPVCAMERA_TUNING_HPP
#define TPVCAMERA_TUNING_HPP

#include <cstddef>
#include <cstdint>

namespace Constants
{
    // The camera collision walk (core/collision/arm.cpp).
    // Props and trees the camera collision steps past (skip + re-cast) before it stops on the next hit, a cap that a
    // cluttered yard (fence sections, a lamp, a cart, invisible designer boxes) stays well under.
    constexpr int COLLISION_FADE_SKIP_MAX = 16;
    // RayWorldIntersection does not always honour the skip list: a baked level collider can come back on the re-cast
    // after it was listed. The walk then resumes this far past the hit instead, at most COLLISION_WALK_RESUME_MAX
    // times.
    constexpr float COLLISION_WALK_RESUME_STEP = 0.05f;
    constexpr int COLLISION_WALK_RESUME_MAX = 32;
    // How far before and past a man-made structure's physics hit its drawn mesh is looked for on the probe's lines. A
    // hit with none of the structure's drawn mesh that close is on collision that draws nothing there (the canvas walls
    // and roof of a tent model that draws only its wooden frame) and is walked past.
    constexpr float COLLISION_DRAWN_REACH = 0.25f;

    // The render-octree occluder scan (render_occlusion.cpp).
    // Safety cap on the per-query node count: if the box overlaps more nodes than this the query returns nothing
    // for that call. Also sizes the stack list buffer for the count-then-fill query (uncapped memcpy).
    constexpr int RENDER_OCCLUSION_MAX_NODES = 1024;
    // Headroom kept free in that buffer between the counting query and the filling one. The fill copies whatever
    // the octree holds at that moment, so a node registered between the two calls would otherwise land past the
    // end of the buffer. A count above RENDER_OCCLUSION_MAX_NODES - RENDER_OCCLUSION_NODE_SLACK is treated like a
    // count above the cap.
    constexpr int RENDER_OCCLUSION_NODE_SLACK = 64;

    // The character fade (hooks/character_fade.cpp).
    // A fade value older than this is ignored, so a faded object comes back on its own when the camera stops setting
    // it (first person, a menu, a reload).
    constexpr uint64_t FADE_STALE_MS = 250;

    // The occluder fade and its scan (hooks/occluder_fade.cpp, render_occlusion.cpp).
    // Nodes faded at once. The detour scans this many entries per matching call, so it stays small.
    constexpr int OCCLUDER_FADE_MAX_NODES = 32;
    /// The fade's sightlines from the camera: the head, the chest, the hips and either side of the chest.
    constexpr int OCCLUDER_FADE_SIGHTLINES = 5;
    // Fade timing. A node must hide the character for the delay before it starts to fade, so an object that crosses a
    // sightline for an instant (a post the camera swings past) never blinks; then it fades in, is held while it
    // flickers on and off the sightline, and fades back out. An object at the lens skips the delay.
    constexpr float OCCLUDER_FADE_DELAY_SECONDS = 0.1f;
    constexpr float OCCLUDER_FADE_IN_SECONDS = 0.15f;
    constexpr float OCCLUDER_FADE_HOLD_SECONDS = 0.4f;
    constexpr float OCCLUDER_FADE_OUT_SECONDS = 0.4f;
    // Render nodes the occluder scan keeps per query (the brushes, trees and entities near the arm and the character).
    constexpr int OCCLUDER_SCAN_MAX = 768;
    // The scan's octree query covers the arm plus this margin (meters), and is reused until the camera or the
    // character leaves it, so a walking camera does not pay the octree query every frame. Its entity proxies and
    // movable brushes, which move and appear with the box held still, are queried again every OCCLUDER_SCAN_MAX_AGE
    // seconds; its static brushes, vegetation and merged patches, which only the level streaming changes, every
    // OCCLUDER_SCAN_STATIC_MAX_AGE seconds. An octree query costs about the same for a box this size or a little
    // larger, so the margin is set where turning the view around in a town takes under one full rescan a second. The
    // scanned entities' boxes and states follow them between those queries (OCCLUDER_ENTITY_REFRESH_MS), so the dynamic
    // query is for what arrives, and something that arrives meanwhile stops the camera like any unmatched hit.
    constexpr float OCCLUDER_SCAN_MARGIN = 2.0f;
    constexpr float OCCLUDER_SCAN_MAX_AGE = 1.0f;
    constexpr float OCCLUDER_SCAN_STATIC_MAX_AGE = 2.0f;
    // The next scan is built ahead over three camera frames (a costly octree query each), so it is usually in use
    // before the one it replaces runs out: once the room left to the box's usable edge is less than
    // OCCLUDER_SCAN_AHEAD_FRAMES frames of the points' recent travel (kept between OCCLUDER_SCAN_AHEAD_ROOM_MIN and
    // _MAX meters), or the static part is within OCCLUDER_SCAN_AHEAD_LEAD seconds of OCCLUDER_SCAN_STATIC_MAX_AGE. A
    // walk then builds rarely and a fast turn early enough.
    constexpr float OCCLUDER_SCAN_AHEAD_FRAMES = 5.0f;
    constexpr float OCCLUDER_SCAN_AHEAD_ROOM_MIN = 0.3f;
    constexpr float OCCLUDER_SCAN_AHEAD_ROOM_MAX = 1.0f;
    // How much of the points' recent travel per frame is kept from one frame to the next (a decaying peak).
    constexpr float OCCLUDER_SCAN_AHEAD_SPEED_KEEP = 0.9f;
    constexpr float OCCLUDER_SCAN_AHEAD_LEAD = 0.5f;
    // How often a reused scan reads its entities' boxes and states again (milliseconds): a door that starts to swing or
    // a cart that rolls on is followed within this, and the frames in between keep what the last read found.
    constexpr std::uint64_t OCCLUDER_ENTITY_REFRESH_MS = 50;
    // Cap on the triangles tested per scan, so a dense scene cannot stall a frame.
    constexpr int OCCLUDER_TRIANGLE_BUDGET = 400000;
    // Render meshes whose triangle-run boxes and positions the occluder tests keep, and the bytes those copies may take
    // in all. A camera moving through a town tests the same few dozen meshes again and again, and every fresh read of
    // one decodes its whole vertex stream (GetPosPtr).
    constexpr int OCCLUDER_MESH_CACHE_ENTRIES = 64;
    constexpr std::size_t OCCLUDER_MESH_CACHE_BYTES = std::size_t{48} * 1024 * 1024;
    // How far past the camera arm a structure's or a prop's box may lie for its mesh to be measured ahead, one per
    // frame, before the arm reaches it.
    constexpr float OCCLUDER_MESH_PREFETCH_MARGIN = 1.0f;

    // The character attachments and the NPC lens fade.
    // The walk's limits: carried items kept (the character fade's detour compares each drawn proxy against this many),
    // attachments read per character (the player's has about fifty), how deep carried characters are followed (the
    // character, a bow on it, an arrow on the bow), and how many characters are walked in all.
    constexpr int CHARACTER_FADE_MAX_CARRIED = 16;
    constexpr int CHARACTER_ATTACHMENT_SCAN_CAP = 128;
    constexpr int CHARACTER_ATTACHMENT_MAX_DEPTH = 3;
    constexpr int CHARACTER_ATTACHMENT_MAX_CHARACTERS = 8;
    // The NPC lens fade's published table (hooks/character_fade.cpp): the entities it dissolves at once, the bodies of
    // the people and animals at the lens and the items they carry. The detour compares a drawn proxy's entity against
    // them only after a 256-bit filter lets it through.
    constexpr int NPC_FADE_MAX_ENTITIES = 48;

    // The shader twins (hooks/shader_twins.cpp).
    // Distinct source shader instances with a twin, one per kind and material gen mask (the textures a material holds
    // can split one further), with separate entries when a shader needs both plant and frame scopes. A stock level
    // supplies at least 140 such pairs (Kutnohorsko: 62 plant and 53 frame pairs in its level inventory, plus about
    // 25 to 40 character twins; at most 119 plant and frame pairs across both vanilla levels). Keep headroom for later
    // loads and mods; when it runs out, the plant and frame pairs take the slots before the character's. The
    // AddRenderObject detour scans only entries in use. SwapSet's byte-sized character indices limit this to 256.
    constexpr int SHADER_TWIN_MAX_SOURCES = 256;
    // A loading screen names at most this many of the candidates that found no source slot left, one line each (a
    // large vegetation mod could miss many on every load); its closing line counts them all.
    constexpr int SHADER_TWIN_CAPACITY_LOG_LINES = 16;
    // A published swap set or window older than this is ignored, so the character's items and the trees go back to
    // the stock shaders on their own when the camera stops updating them (a menu, a reload).
    constexpr uint64_t SHADER_TWIN_STALE_MS = 250;
    // A twin whose parse has not finished after this long is given up for the session.
    constexpr float SHADER_TWIN_TIMEOUT_SECONDS = 30.0f;
    // A ready twin whose source or twin the engine reloaded or changed (another gen mask or technique count, a parse
    // under way, EF_RELOADED newly set, a twin that lost its source's feature bits) keeps its items on the stock
    // shader until both have read back consistent for this long: a reload frees and parses the techniques again on the
    // render thread, and EF_LOADED need not clear meanwhile, so one consistent read alone does not prove the parse is
    // over.
    constexpr uint64_t SHADER_TWIN_REVALIDATE_MS = 250;
    // How often the ready twins are checked against their sources while nothing is stale and the engine's shader
    // reload count holds (see revalidate_twins()).
    constexpr uint64_t SHADER_TWIN_REVALIDATE_SWEEP_MS = 250;
    // A variant that first draws or loads after its level's loading boundary (content spawned later, a mod's, or every
    // missing one after a DEV hot reload) gets its twin only while the game stands still: the game clock stopped, under
    // the Esc game menu, a UI overlay (inventory, map, codex) or neither (the Esc menu open across a DEV hot reload,
    // whose new module has not seen it open), and never in a dialogue (service_shader_twins_paused()). Creation starts
    // once such a pause has lasted SHADER_TWIN_PAUSE_DWELL_MS, so a menu opened and closed at once creates nothing;
    // then one twin at a time, each parse finished by a render-command barrier inside its paused frame (a parse takes
    // about 30 to 190 ms), with at least SHADER_TWIN_PAUSE_SPACING_MS from the end of one barrier to the next creation
    // so the menu keeps answering. A pause that ends within SHADER_TWIN_PAUSE_SPACING_MS of a creation is counted in
    // the logs.
    constexpr uint64_t SHADER_TWIN_PAUSE_DWELL_MS = 500;
    constexpr uint64_t SHADER_TWIN_PAUSE_SPACING_MS = 100;
    // How long the first-use preparation waits before it asks again for an engine interface that is not up yet.
    constexpr uint64_t SHADER_TWIN_PREPARE_RETRY_MS = 1000;

    // The character twins and their warm-up (hooks/twins).
    // A swapped tree or a set for the fade still waiting for its twin's PSOs after this long is logged once: a twin
    // permutation that failed to compile never becomes valid, and a tree keeps its stock look.
    constexpr float SHADER_TWIN_PSO_WAIT_LOG_SECONDS = 20.0f;
    // A skin attachment's temporary render object takes its compiled objects from a pool kept per attachment while
    // wh_r_SemiPersistentCompiledObjectsDuration is set (sub_1804A38CC puts the pool's iterator at the render object's
    // +0x100 for the draw), one pool per frame parity, LOD and main or shadow view (sub_181DF1530), so the shadow pass
    // keeps compiled objects of its own. A pooled compiled object whose element, shader and resources match the item
    // is reused without a PSO build (sub_1804A6104 marks only its constants dirty, 0x1804A6190); another shader, or a
    // change of the object's flags under 0xDFD60000 (FOB_DISSOLVE and FOB_DISSOLVE_OUT among them, sub_181DF0F50),
    // rebuilds it whole. A pooled object that kept its stock PSOs would keep them for as long as its item stays the
    // same, so the character twins are warmed up before the first close-up. In third person the character's items
    // name their twin whether or not he dissolves (the twin draws exactly like the stock shader at no dissolve), and a
    // build of one that does not dissolve keeps the stock PSOs and builds aside, in local arrays, the twin's and the
    // stock's sets for the same item as the close-up fade will ask for them (the description with FOB_DISSOLVE,
    // FOB_DISSOLVE_OUT and the dissolve RT bit), which asks the engine to compile the twin's dissolving permutations.
    // The render threads share a table of the descriptions built aside, so the same set is never built twice, and
    // the main thread holds each set until it can draw. A twin turns warm once every set of it can draw and stays warm
    // until a fading build turns it cold; only then do its items keep the twin while they dissolve. Until then they
    // take the stock shader for the fade, and a dissolving build of one also builds the twin's set aside (once per
    // description, like the others). A dissolving build of an item that names the twin is checked like a tree's: one
    // whose permutation is not compiled (an RT mask no warm-up saw, a quality change) keeps the stock PSOs and turns
    // the twin cold, so its items go back to the stock shader for the fade, rebuild, and warm it up again. The
    // dissolving eyes and hair are never left undrawn while a permutation compiles.
    // The warm-up sets the main thread keeps at once, one per distinct description: the character's two Eye items,
    // about five hood and hair items, two beard items, the eyelashes and the eye film, a multi-pass hair item once per
    // chained technique, and room for the other RT masks and LODs his items take over a session. A warm set holds no
    // PSO and is the first to make room.
    constexpr int CHARACTER_WARM_MAX_ENTRIES = 128;
    // Slots of the render threads' table of the descriptions already built aside (a power of two), and how far one
    // lookup probes. A full table builds nothing more aside; the dissolving builds still warm up through the checked
    // and stock paths.
    constexpr std::size_t CHARACTER_WARM_KEY_SLOTS = 512;
    constexpr std::size_t CHARACTER_WARM_KEY_PROBES = 32;
    // A character twin's source counts as on the character's current items this long after a render job last saw one
    // of them; eye_fade_ready() and character_fade_ready() need every such source warm. While no source was seen this
    // recently (the character is not drawn), the readiness keeps its last value.
    constexpr uint64_t CHARACTER_TWIN_RECENT_MS = 500;
    // A twin turned cold by a fading build whose sets never reached the main thread (the hand-over queue was full)
    // stays cold this long, after which its warm sets count again and the next fading build checks it once more.
    constexpr uint64_t CHARACTER_TWIN_COLD_HOLD_MS = 2000;
    // The character twins' kept sets belong to the materials of one level or save: once the character's render node
    // is another one for this many camera frames in a row (a load made a new one), the sets of the old node are dropped
    // and the key table is emptied. The sets built for the new node in those frames stay (his items take the twins at
    // once, and their pooled compiled objects are not built again while he stays at rest), and their keys are taken
    // again.
    constexpr int CHARACTER_ROOT_SETTLE_FRAMES = 30;
    // A character twin's set for the fade that cannot draw is given up with its twin for the session once it has not
    // been able to for this long and over this many polls in a row (camera frames, so the time a minimised game spends
    // without frames gives nothing up on its own): its twin build failed, or a pass its stock set draws has no twin PSO
    // or one the PSO factory no longer creates again (not valid, not queued, its attempt count standing still). The
    // factory takes a PSO off its list for every creation result but 1, the one for shaders still compiling
    // (sub_181E12AD0 and sub_181E12B60, which sub_180C185C8 runs for each PSO of the list every frame), so such a
    // permutation failed for good. A count that keeps moving still compiles and is waited for, however long the first
    // run takes.
    constexpr uint64_t CHARACTER_TWIN_GIVE_UP_MS = 20000;
    constexpr int CHARACTER_TWIN_GIVE_UP_POLLS = 300;
    // Render nodes the swap set names: the character's own render proxy and the carried items' the attachment walk
    // lists (CharacterNodes). An item of a temporary render object that names one of them at RENDER_OBJECT_NODE_OFFSET
    // swaps to its twin; every other character keeps the stock shaders.
    constexpr int CHARACTER_TWIN_MAX_NODES = 1 + CHARACTER_FADE_MAX_CARRIED;
    // A multi-pass Hair item (the beard, the hood's hair) adds one item per chained technique (HairFrontPass,
    // HairBackPass, after General; AddRenderObject 0x1804A5384), each with a compiled object of its own whose warm-up
    // set can arrive frames after the others. A twin turns warm only once every set of it has been able to draw for
    // this long, so one that arrives late and still compiles holds the twin back instead of turning it cold again at
    // its first fade.
    constexpr uint64_t CHARACTER_TWIN_SETTLE_MS = 200;

    // The DEV twin clone (dev/twin_clone/twin_clone.cpp).
    // At most this many techniques a clone copies (a plant instance has 7 or 8 at runtime).
    constexpr int TWIN_CLONE_MAX_TECHNIQUES = 16;
    // The dry-run's bounds. A loading boundary compares for at most TWIN_CLONE_DRY_RUN_LOADING_US and leaves the rest
    // to the next pause; a paused frame with nothing left to create compares at most TWIN_CLONE_DRY_RUN_PAUSE_PAIRS
    // pairs or for TWIN_CLONE_DRY_RUN_PAUSE_US, checked between pairs (a pair takes a few milliseconds: two splices
    // with a table of about 3,850 strings, and a copy and compare of each pass the twin does not share with its stock).
    // A pair that finds the factory's lock busy ends its step and is tried again in the next one, in at most
    // TWIN_CLONE_DRY_RUN_BUSY_RETRIES more steps; the run then gives it up, and a later occasion queues it again.
    constexpr uint64_t TWIN_CLONE_DRY_RUN_LOADING_US = 500000;
    constexpr uint64_t TWIN_CLONE_DRY_RUN_PAUSE_US = 6000;
    constexpr int TWIN_CLONE_DRY_RUN_PAUSE_PAIRS = 4;
    constexpr int TWIN_CLONE_DRY_RUN_BUSY_RETRIES = 3;

    // The level inventory (hooks/twin_inventory.cpp).
    // The walk's bounds, far above a level's (a Kutnohorsko save holds 599 groups, 25,028 statobjs and 14,417 brush
    // materials, and a material at most a few dozen sub-materials): a table past its bound is taken for a layout drift
    // and skipped. The dedupe set (a power of two) holds the distinct plant and frame materials of a level (its brush
    // material table holds about 1,400 distinct materials of every kind); past three quarters full the walk goes on
    // without it.
    constexpr std::size_t TWIN_INVENTORY_MAX_GROUPS = 8192;
    constexpr std::size_t TWIN_INVENTORY_MAX_STATOBJS = 131072;
    constexpr std::size_t TWIN_INVENTORY_MAX_MATERIALS = 65536;
    constexpr int TWIN_INVENTORY_MAX_SUBMATERIALS = 128;
    constexpr std::size_t TWIN_INVENTORY_MATERIAL_SET = 8192;

    // The tree see-through (hooks/shader_twins.cpp).
    // Tree and frame render nodes swapped to a twin at once. The AddRenderObject detour scans this many entries per
    // call for a node of the swap set, so it stays small.
    constexpr int TREE_HOLE_MAX_NODES = 32;
    // A node stays swapped this long after the camera stops listing it, so a tree that flickers in and out of the
    // window does not refill its render object every few frames.
    constexpr float TREE_HOLE_HOLD_SECONDS = 1.0f;

    // The tree see-through's twins built during play.
    // Compiled objects of swapped trees that wait on their stock PSOs for the twin's to compile, at most.
    constexpr int TREE_HOLE_MAX_PENDING = 64;
    // A swapped tree that lost a wait (the hand-over queue or the list above was full) is refilled again this long
    // after, so its compiled objects hand over new waits; a refill for another reason comes first and clears it.
    constexpr uint64_t TREE_HOLE_RETRY_MS = 2000;
    // The time the extra twin builds of swapped trees may take in one camera frame. A build normally takes a few
    // microseconds, but one that loads a twin permutation not yet in memory can take tens of milliseconds, and the
    // engine streams new trees in bursts, so two of those in one frame stack into one long stall. Past the budget a
    // compiled object keeps its stock look for this frame, and its tree is refilled TREE_HOLE_DEFER_RETRY_MS later.
    // An in-flight build reserves admission without blocking other callers and charges the frame in which it ends.
    // This is an admission threshold, not a deadline that can interrupt an individual native build.
    constexpr uint32_t TREE_HOLE_TWIN_BUILD_BUDGET_US = 2000;
    constexpr uint64_t TREE_HOLE_DEFER_RETRY_MS = 150;
} // namespace Constants

#endif // TPVCAMERA_TUNING_HPP
