/**
 * @file engine/herb_scan.cpp
 * @brief Native detection of pickable herbs and mushrooms around a point.
 */

#include "engine/herb_scan.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/octree_query.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HenrySenses
{
    namespace
    {
        using GetEntityStatObjFn = std::uintptr_t(__fastcall *)(std::uintptr_t node, std::uint32_t part, void *matrix);

        // Every CMergedMeshRenderNode field the scan reads lies below this offset.
        constexpr std::size_t NODE_HEADER_SIZE = 0x100;
        constexpr std::size_t GROUP_PATH_LENGTH = 160;
        constexpr float MIN_CELL_EXTENT = 1.0f;
        constexpr float MAX_CELL_EXTENT = 64.0f;
        // A flat box is enough: herbs grow on the ground around the player.
        constexpr float MAX_BOX_HALF_HEIGHT = 15.0f;
        // The octree is asked again once the player moved this far from the last query (the query box is grown by
        // the same margin, so the cached nodes still cover the scan radius) or after REQUERY_MS. The cell index
        // lookup is cheap enough to run every scan and needs no cache.
        constexpr float REQUERY_DISTANCE = 4.0f;
        constexpr std::int64_t REQUERY_MS = 5000;
        // A level has about 600 vegetation groups; a count beyond this is not the group table.
        constexpr std::size_t MAX_VEGETATION_GROUPS = 65536;
        // A cached vegetation node that no longer sits at its matrix translation was freed and reused.
        constexpr float NODE_MOVED_TOLERANCE = 0.01f;
        // Cross-checks of the cell index against the octree that may disagree (a cell can finish streaming between
        // the two reads) before the octree query is kept for the session.
        constexpr int MAX_CROSS_CHECK_MISMATCHES = 3;
        // Cell index lookups in a row that may fail before the octree query is kept for the session. A bucket is a
        // std::vector the engine grows while cells stream in, and its {begin, end} pair is read without a lock, so a
        // read can catch one half old and one half new; such a scan takes the octree query once.
        constexpr int MAX_LOOKUP_FAILURES = 5;
        // Upper bound on the cells whose last skipped state is remembered for the log.
        constexpr std::size_t MAX_LOGGED_CELL_STATES = 4096;
        constexpr std::size_t BUCKET_COUNT = static_cast<std::size_t>(constants::MERGED_MESHES_HASH_DIM_XY) *
                                             constants::MERGED_MESHES_HASH_DIM_XY * constants::MERGED_MESHES_HASH_DIM_Z;
        constexpr std::size_t MANAGER_WINDOW_SLOTS =
            static_cast<std::size_t>(2 * constants::MERGED_MESHES_MANAGER_SLOT_WINDOW) / sizeof(std::uintptr_t) + 1;

        /**
         * @brief The vegetation group table of the loaded level, with the lazily read pickable flag of each group.
         *        It is kept while its storage and size stay the same (one level), so a scan reads only the groups it
         *        meets for the first time.
         */
        struct VegetationTable
        {
            std::uintptr_t begin{0};
            std::size_t count{0};
            // Group 0's CStatObj, so a new level whose table landed at the same address and size is still noticed.
            std::uintptr_t first_stat_obj{0};
            // -1 unknown, 0 no, 1 yes.
            std::vector<std::int8_t> pickable{};
        };

        /** @brief Decoded CMergedMeshRenderNode fields. */
        struct MergedMeshCell
        {
            game_structures::Vec3f origin{};
            float extent{0.0f};
            game_structures::Aabb visible{};
            game_structures::Vec3f rotation_origin{};
            float z_rotation{0.0f};
            std::uintptr_t groups{0};
            std::uint32_t group_count{0};
            std::uint32_t state{0};
        };

        /** @brief A cluster being grown: the seed plant anchors the one-pick radius. */
        struct ClusterSeed
        {
            game_structures::Vec3f seed{};
            game_structures::Vec3f min{};
            game_structures::Vec3f max{};
            std::int32_t group{-1};
            std::uint32_t plants{0};
            float distance_sq{0.0f};
            // One of its plants went out for outlining (its model resolved), so the cluster shows as outlines;
            // without one it keeps its marker.
            bool outlined{false};
        };

        /** @brief A pickable CVegetation node (a mushroom) of the last vegetation query. */
        struct PickableVegetation
        {
            std::uintptr_t node{0};
            game_structures::Matrix34f world{};
            std::uintptr_t stat_obj{0};
            std::int32_t group{-1};
            // The hidden state the log last reported, so a pick and a respawn are logged once each.
            bool hidden{false};
        };

        /** @brief Where the query box and the time of a cached node list came from. */
        struct CacheStamp
        {
            game_structures::Vec3f center{};
            float radius{0.0f};
            std::int64_t ms{0};
            bool valid{false};
        };

        /** @brief Where the merged-mesh cells of a scan come from. */
        enum class CellSource : std::uint8_t
        {
            /// The CMergedMeshesManager slot has not been searched for yet.
            Unsearched,
            /// The manager was found; the octree query runs until one cross-check agrees with it.
            Unverified,
            /// The manager's cell index, looked up every scan.
            Manager,
            /// The render octree query (the manager is missing, disagreed or faulted), for the session.
            Octree,
        };

        /** @brief Per-scan counts for the summary log line. */
        struct ScanCounts
        {
            std::size_t cells{0};
            std::size_t unstreamed_cells{0};
            std::size_t herb_groups{0};
            std::uint64_t plants{0};
            std::size_t mushrooms{0};
            std::size_t mushrooms_picked{0};
        };

        std::atomic<bool> s_available{false};
        std::atomic<bool> s_faulted{false};

        // Main-thread scratch, reused across scans.
        std::vector<std::uintptr_t> s_nodes;
        std::vector<std::uintptr_t> s_bucket_nodes;
        std::vector<std::uintptr_t> s_check_lookup;
        std::vector<std::uintptr_t> s_check_octree;
        std::vector<std::uintptr_t> s_check_manager;
        std::vector<std::byte> s_group_bytes;
        std::vector<std::byte> s_instance_bytes;
        std::vector<ClusterSeed> s_seeds;
        std::bitset<BUCKET_COUNT> s_bucket_seen;
        ScanCounts s_counts;
        // Per-plant output of the current collect (nullptr when not asked for).
        std::vector<HerbPlant> *s_plants_out = nullptr;

        VegetationTable s_table;
        std::unordered_set<std::int32_t> s_logged_groups;
        std::unordered_map<std::uintptr_t, std::uint32_t> s_logged_cell_states;
        std::unordered_set<std::uintptr_t> s_logged_vegetation;

        // The merged-mesh cells a scan decodes. From the cell index they are looked up every scan; from the octree
        // they are the cells of the last query that hold at least one pickable group (a cell's groups are fixed when
        // it is built and a pick only rewrites instance scales, so between queries only these are re-read).
        std::vector<std::uintptr_t> s_cells;
        CacheStamp s_cells_stamp;
        CellSource s_cell_source = CellSource::Unsearched;
        std::uintptr_t s_manager_slot = 0;
        int s_cross_check_mismatches = 0;
        int s_lookup_failures = 0;

        // Pickable CVegetation nodes of the last vegetation query, re-read every scan for their hidden (picked) flag.
        std::vector<PickableVegetation> s_vegetation;
        CacheStamp s_vegetation_stamp;
        bool s_vegetation_off = false;

        std::size_t s_last_cluster_count = static_cast<std::size_t>(-1);
        std::uint64_t s_last_plant_total = static_cast<std::uint64_t>(-1);
        std::size_t s_last_outlined = static_cast<std::size_t>(-1);
        std::size_t s_last_mushrooms = static_cast<std::size_t>(-1);
        std::size_t s_last_unstreamed = static_cast<std::size_t>(-1);
        std::size_t s_last_fallback_markers = static_cast<std::size_t>(-1);

        template <typename T> [[nodiscard]] T field(const std::byte *base, std::size_t offset) noexcept
        {
            T value{};
            std::memcpy(&value, base + offset, sizeof(T));
            return value;
        }

        [[nodiscard]] std::int64_t steady_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        [[nodiscard]] double elapsed_ms(std::chrono::steady_clock::time_point since) noexcept
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
        }

        [[nodiscard]] bool all_finite(const game_structures::Vec3f &v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /**
         * @brief The box around @p center whose nodes can hold a plant within @p reach: flat, because herbs grow on
         *        the ground around the player.
         */
        [[nodiscard]] game_structures::Aabb scan_box(const game_structures::Vec3f &center, float reach) noexcept
        {
            const float half_height = std::min(reach, MAX_BOX_HALF_HEIGHT);
            return game_structures::Aabb{
                {center.x - reach, center.y - reach, center.z - half_height},
                {center.x + reach, center.y + reach, center.z + half_height}
            };
        }

        /** @brief The engine's own box test (strict on every axis), so both cell sources keep the same cells. */
        [[nodiscard]] bool overlaps(const game_structures::Aabb &a, const game_structures::Aabb &b) noexcept
        {
            return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y &&
                   a.min.z < b.max.z && b.min.z < a.max.z;
        }

        /** @brief True when a node list cached at @p stamp no longer covers a scan of @p radius around @p center. */
        [[nodiscard]] bool stale(const CacheStamp &stamp, const game_structures::Vec3f &center, float radius) noexcept
        {
            if (!stamp.valid || radius != stamp.radius || steady_ms() - stamp.ms >= REQUERY_MS)
            {
                return true;
            }
            const float dx = center.x - stamp.center.x;
            const float dy = center.y - stamp.center.y;
            const float dz = center.z - stamp.center.z;
            return dx * dx + dy * dy + dz * dz > REQUERY_DISTANCE * REQUERY_DISTANCE;
        }

        void stamp_now(CacheStamp &stamp, const game_structures::Vec3f &center, float radius) noexcept
        {
            stamp = CacheStamp{center, radius, steady_ms(), true};
        }

        /**
         * @brief Reads the level's vegetation group table and keeps the cached flags while it is the same table.
         * @details A new table (a level change) drops the per-group flags, the cached nodes and the once-per-object
         *          log memory, since group indices and nodes belong to one level.
         */
        [[nodiscard]] bool refresh_vegetation_table()
        {
            const std::uintptr_t slot = anchor_address(AnchorId::ObjManager);
            const auto manager = DMK::memory::read<std::uintptr_t>(DMK::Address{slot});
            if (!manager || !DMK::memory::is_plausible_ptr(DMK::Address{*manager}))
            {
                return false;
            }
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*manager + constants::OBJMAN_VEG_GROUPS_BEGIN_OFFSET});
            const auto end =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*manager + constants::OBJMAN_VEG_GROUPS_END_OFFSET});
            if (!begin || !end || *end <= *begin || !DMK::memory::is_plausible_ptr(DMK::Address{*begin}) ||
                (*end - *begin) % constants::VEG_GROUP_STRIDE != 0)
            {
                return false;
            }
            const std::size_t count = static_cast<std::size_t>((*end - *begin) / constants::VEG_GROUP_STRIDE);
            if (count > MAX_VEGETATION_GROUPS)
            {
                return false;
            }
            const auto first =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*begin + constants::VEG_GROUP_STAT_OBJ_OFFSET});
            const std::uintptr_t first_stat_obj = first ? *first : 0;
            if (*begin == s_table.begin && count == s_table.count && first_stat_obj == s_table.first_stat_obj)
            {
                return true;
            }
            s_table.begin = *begin;
            s_table.count = count;
            s_table.first_stat_obj = first_stat_obj;
            s_table.pickable.assign(count, -1);
            s_logged_groups.clear();
            s_logged_cell_states.clear();
            s_logged_vegetation.clear();
            s_cells.clear();
            s_cells_stamp.valid = false;
            s_vegetation.clear();
            s_vegetation_stamp.valid = false;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: vegetation group table 0x{:016X} ({} groups)",
                s_table.begin,
                count
            );
            return true;
        }

        [[nodiscard]] std::uintptr_t group_entry(std::int32_t group) noexcept
        {
            return s_table.begin + static_cast<std::size_t>(group) * constants::VEG_GROUP_STRIDE;
        }

        /** @brief Logs the model and kind of a pickable vegetation group the first time a scan meets it. */
        void log_group_once(std::int32_t group)
        {
            if (!s_logged_groups.insert(group).second)
            {
                return;
            }
            std::string path = "?";
            const std::uintptr_t entry = group_entry(group);
            if (const auto statobj = DMK::memory::read<std::uintptr_t>(DMK::Address{entry}); statobj && *statobj != 0)
            {
                if (const auto text =
                        DMK::memory::read<std::uintptr_t>(DMK::Address{*statobj + constants::STATOBJ_PATH_OFFSET});
                    text && *text != 0)
                {
                    if (std::string text_path = read_c_string(*text, GROUP_PATH_LENGTH); !text_path.empty())
                    {
                        path = std::move(text_path);
                    }
                }
            }
            const auto merged =
                DMK::memory::read<std::uint8_t>(DMK::Address{entry + constants::VEG_GROUP_AUTO_MERGED_OFFSET});
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "HerbScan: pickable vegetation group {} = {} ({})",
                group,
                path,
                !merged ? "kind ?" : (*merged != 0 ? "merged mesh" : "vegetation node")
            );
        }

        [[nodiscard]] bool group_is_pickable(std::int32_t group) noexcept
        {
            if (group < 0 || static_cast<std::size_t>(group) >= s_table.count)
            {
                return false;
            }
            std::int8_t &known = s_table.pickable[static_cast<std::size_t>(group)];
            if (known < 0)
            {
                const std::uintptr_t entry = group_entry(group);
                const auto flags =
                    DMK::memory::read<std::uint8_t>(DMK::Address{entry + constants::VEG_GROUP_FLAGS_OFFSET});
                known = flags && (*flags & constants::VEG_GROUP_FLAG_PICKABLE) != 0 ? 1 : 0;
            }
            return known == 1;
        }

        [[nodiscard]] std::uintptr_t group_stat_obj(std::int32_t group) noexcept
        {
            const auto stat_obj = DMK::memory::read<std::uintptr_t>(
                DMK::Address{group_entry(group) + constants::VEG_GROUP_STAT_OBJ_OFFSET}
            );
            return stat_obj && DMK::memory::is_plausible_ptr(DMK::Address{*stat_obj}) ? *stat_obj : 0;
        }

        [[nodiscard]] bool read_cell(std::uintptr_t node, MergedMeshCell &cell) noexcept
        {
            std::array<std::byte, NODE_HEADER_SIZE> header{};
            if (!DMK::memory::read_into(DMK::Address{node}, header).has_value())
            {
                return false;
            }
            const std::byte *base = header.data();
            cell.origin = field<game_structures::Vec3f>(base, constants::MERGED_MESH_AABB_MIN_OFFSET);
            const auto max = field<game_structures::Vec3f>(base, constants::MERGED_MESH_AABB_MAX_OFFSET);
            cell.extent = max.x - cell.origin.x;
            cell.visible.min = field<game_structures::Vec3f>(base, constants::MERGED_MESH_VISIBLE_AABB_MIN_OFFSET);
            cell.visible.max = field<game_structures::Vec3f>(base, constants::MERGED_MESH_VISIBLE_AABB_MAX_OFFSET);
            cell.rotation_origin = field<game_structures::Vec3f>(base, constants::MERGED_MESH_POS_OFFSET);
            cell.z_rotation = field<float>(base, constants::MERGED_MESH_ZROTATION_OFFSET);
            cell.groups = field<std::uintptr_t>(base, constants::MERGED_MESH_GROUPS_OFFSET);
            cell.group_count = field<std::uint32_t>(base, constants::MERGED_MESH_GROUP_COUNT_OFFSET);
            cell.state = field<std::uint32_t>(base, constants::MERGED_MESH_STATE_OFFSET);
            return std::isfinite(cell.extent) && cell.extent >= MIN_CELL_EXTENT && cell.extent <= MAX_CELL_EXTENT &&
                   std::isfinite(cell.z_rotation) && cell.groups != 0 && cell.group_count != 0 &&
                   cell.group_count <= constants::HERB_MAX_GROUPS_PER_CELL;
        }

        /**
         * @brief Reads a node's vtable and cell fields when it is a merged-mesh cell.
         */
        [[nodiscard]] bool read_merged_mesh_cell(std::uintptr_t node, MergedMeshCell &cell) noexcept
        {
            if (node == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{node}))
            {
                return false;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{node});
            return vtable && vtable_is(GameClass::MergedMeshNode, *vtable) && read_cell(node, cell);
        }

        [[nodiscard]] bool streamed_in(const MergedMeshCell &cell) noexcept
        {
            return cell.state == constants::MERGED_MESH_STATE_STREAMED_IN;
        }

        /**
         * @brief Logs a cell the scan skips because it is not streamed in, once per state it is met in.
         */
        void log_unstreamed_cell(std::uintptr_t node, const MergedMeshCell &cell)
        {
            if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                return;
            }
            if (s_logged_cell_states.size() >= MAX_LOGGED_CELL_STATES)
            {
                s_logged_cell_states.clear();
            }
            const auto [it, inserted] = s_logged_cell_states.try_emplace(node, cell.state);
            if (!inserted && it->second == cell.state)
            {
                return;
            }
            it->second = cell.state;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: cell 0x{:016X} at ({:.0f}, {:.0f}, {:.0f}) is not streamed in (state {}); its plants are "
                "skipped",
                node,
                cell.origin.x,
                cell.origin.y,
                cell.origin.z,
                cell.state
            );
        }

        /**
         * @brief World position of one merged-mesh instance (ConvertInstanceAbsolute).
         */
        [[nodiscard]] game_structures::Vec3f
        instance_position(const MergedMeshCell &cell, const std::byte *instance, float cos_z, float sin_z) noexcept
        {
            const float scale = cell.extent / 65535.0f;
            game_structures::Vec3f p{
                cell.origin.x + static_cast<float>(field<std::uint16_t>(instance, 0)) * scale,
                cell.origin.y + static_cast<float>(field<std::uint16_t>(instance, 2)) * scale,
                cell.origin.z + static_cast<float>(field<std::uint16_t>(instance, 4)) * scale
            };
            if (cell.z_rotation != 0.0f)
            {
                const float dx = p.x - cell.rotation_origin.x;
                const float dy = p.y - cell.rotation_origin.y;
                p.x = cell.rotation_origin.x + dx * cos_z - dy * sin_z;
                p.y = cell.rotation_origin.y + dx * sin_z + dy * cos_z;
            }
            return p;
        }

        /**
         * @brief World transform of one merged-mesh instance: CreateRotationQ(q, position) * scale.
         */
        [[nodiscard]] game_structures::Matrix34f
        instance_matrix(const std::byte *instance, const game_structures::Vec3f &p, float s) noexcept
        {
            float x =
                static_cast<float>(field<std::int8_t>(instance, constants::MERGED_MESH_INSTANCE_QUAT_OFFSET)) / 128.0f;
            float y =
                static_cast<float>(field<std::int8_t>(instance, constants::MERGED_MESH_INSTANCE_QUAT_OFFSET + 1)) /
                128.0f;
            float z =
                static_cast<float>(field<std::int8_t>(instance, constants::MERGED_MESH_INSTANCE_QUAT_OFFSET + 2)) /
                128.0f;
            float w =
                static_cast<float>(field<std::int8_t>(instance, constants::MERGED_MESH_INSTANCE_QUAT_OFFSET + 3)) /
                128.0f;
            const float length = std::sqrt(x * x + y * y + z * z + w * w);
            if (length > 1e-6f)
            {
                x /= length;
                y /= length;
                z /= length;
                w /= length;
            }
            else
            {
                x = y = z = 0.0f;
                w = 1.0f;
            }
            game_structures::Matrix34f m{};
            m.m[0][0] = (1.0f - 2.0f * (y * y + z * z)) * s;
            m.m[0][1] = 2.0f * (x * y - w * z) * s;
            m.m[0][2] = 2.0f * (x * z + w * y) * s;
            m.m[0][3] = p.x;
            m.m[1][0] = 2.0f * (x * y + w * z) * s;
            m.m[1][1] = (1.0f - 2.0f * (x * x + z * z)) * s;
            m.m[1][2] = 2.0f * (y * z - w * x) * s;
            m.m[1][3] = p.y;
            m.m[2][0] = 2.0f * (x * z - w * y) * s;
            m.m[2][1] = 2.0f * (y * z + w * x) * s;
            m.m[2][2] = (1.0f - 2.0f * (x * x + y * y)) * s;
            m.m[2][3] = p.z;
            return m;
        }

        /**
         * @brief Adds a plant to the cluster of its species it falls in, or seeds a new one.
         * @return The index of that cluster in s_seeds.
         */
        std::size_t add_plant(std::int32_t group, const game_structures::Vec3f &p, float distance_sq)
        {
            const float join_sq = constants::HERB_CLUSTER_RADIUS * constants::HERB_CLUSTER_RADIUS;
            for (std::size_t i = 0; i < s_seeds.size(); ++i)
            {
                ClusterSeed &seed = s_seeds[i];
                if (seed.group != group)
                {
                    continue;
                }
                const float dx = p.x - seed.seed.x;
                const float dy = p.y - seed.seed.y;
                if (dx * dx + dy * dy > join_sq)
                {
                    continue;
                }
                seed.min = {std::min(seed.min.x, p.x), std::min(seed.min.y, p.y), std::min(seed.min.z, p.z)};
                seed.max = {std::max(seed.max.x, p.x), std::max(seed.max.y, p.y), std::max(seed.max.z, p.z)};
                seed.distance_sq = std::min(seed.distance_sq, distance_sq);
                ++seed.plants;
                return i;
            }
            s_seeds.push_back(ClusterSeed{p, p, p, group, 1, distance_sq});
            return s_seeds.size() - 1;
        }

        /** @brief Hands @p plant out for outlining; its cluster then shows as outlines. */
        void add_outline_plant(std::size_t seed, const HerbPlant &plant)
        {
            s_plants_out->push_back(plant);
            s_seeds[seed].outlined = true;
        }

        /**
         * @brief Adds the unpicked plants of the pickable groups of one cell.
         * @return The number of pickable groups the cell holds.
         */
        std::size_t scan_cell(const MergedMeshCell &cell, const game_structures::Vec3f &center, float radius_sq)
        {
            s_group_bytes.resize(static_cast<std::size_t>(cell.group_count) * constants::MERGED_MESH_GROUP_STRIDE);
            if (!DMK::memory::read_into(DMK::Address{cell.groups}, s_group_bytes).has_value())
            {
                return 0;
            }
            const float cos_z = std::cos(cell.z_rotation);
            const float sin_z = std::sin(cell.z_rotation);
            std::size_t pickable_groups = 0;
            for (std::uint32_t g = 0; g < cell.group_count; ++g)
            {
                const std::byte *header = s_group_bytes.data() + g * constants::MERGED_MESH_GROUP_STRIDE;
                const auto group = field<std::int32_t>(header, constants::MERGED_MESH_GROUP_VEG_INDEX_OFFSET);
                if (!group_is_pickable(group))
                {
                    continue;
                }
                ++pickable_groups;
                log_group_once(group);
                const auto instances = field<std::uintptr_t>(header, constants::MERGED_MESH_GROUP_INSTANCES_OFFSET);
                const auto samples = field<std::uint32_t>(header, constants::MERGED_MESH_GROUP_SAMPLE_COUNT_OFFSET);
                if (instances == 0 || samples == 0 || samples > constants::HERB_MAX_SAMPLES_PER_GROUP)
                {
                    continue;
                }
                s_instance_bytes.resize(static_cast<std::size_t>(samples) * constants::MERGED_MESH_INSTANCE_SIZE);
                if (!DMK::memory::read_into(DMK::Address{instances}, s_instance_bytes).has_value())
                {
                    continue;
                }
                const std::uintptr_t stat_obj = s_plants_out != nullptr ? group_stat_obj(group) : 0;
                for (std::uint32_t i = 0; i < samples; ++i)
                {
                    const std::byte *instance = s_instance_bytes.data() + i * constants::MERGED_MESH_INSTANCE_SIZE;
                    // A harvested plant keeps its slot with scale 0 until it respawns.
                    const auto scale_byte = field<std::uint8_t>(instance, constants::MERGED_MESH_INSTANCE_SCALE_OFFSET);
                    if (scale_byte == 0)
                    {
                        continue;
                    }
                    const game_structures::Vec3f p = instance_position(cell, instance, cos_z, sin_z);
                    const float dx = p.x - center.x;
                    const float dy = p.y - center.y;
                    const float dz = p.z - center.z;
                    const float distance_sq = dx * dx + dy * dy + dz * dz;
                    if (distance_sq > radius_sq)
                    {
                        continue;
                    }
                    ++s_counts.plants;
                    const std::size_t seed = add_plant(group, p, distance_sq);
                    if (stat_obj == 0)
                    {
                        continue;
                    }
                    const float scale = static_cast<float>(scale_byte) / constants::VEGETATION_CONV_FACTOR;
                    add_outline_plant(
                        seed,
                        HerbPlant{instance_matrix(instance, p, scale), stat_obj, std::sqrt(distance_sq)}
                    );
                }
            }
            return pickable_groups;
        }

        /**
         * @brief True when one of the cell's groups is a pickable vegetation group.
         */
        [[nodiscard]] bool cell_has_pickable_group(const MergedMeshCell &cell)
        {
            s_group_bytes.resize(static_cast<std::size_t>(cell.group_count) * constants::MERGED_MESH_GROUP_STRIDE);
            if (!DMK::memory::read_into(DMK::Address{cell.groups}, s_group_bytes).has_value())
            {
                return false;
            }
            for (std::uint32_t g = 0; g < cell.group_count; ++g)
            {
                const std::byte *header = s_group_bytes.data() + g * constants::MERGED_MESH_GROUP_STRIDE;
                if (group_is_pickable(field<std::int32_t>(header, constants::MERGED_MESH_GROUP_VEG_INDEX_OFFSET)))
                {
                    return true;
                }
            }
            return false;
        }

        // The merged-mesh cell index

        /**
         * @brief Finds the static holding the CMergedMeshesManager next to the CObjManager slot.
         * @details The 3D engine's init function stores both singletons into neighbouring statics, so the slot is the
         *          one qword in the window around the CObjManager slot whose pointee's primary vtable is
         *          CMergedMeshesManager's. Two different managers in the window would be ambiguous and find nothing.
         * @return The slot address, or 0.
         */
        [[nodiscard]] std::uintptr_t find_manager_slot()
        {
            const std::uintptr_t anchor = anchor_address(AnchorId::ObjManager);
            const auto window = static_cast<std::uintptr_t>(constants::MERGED_MESHES_MANAGER_SLOT_WINDOW);
            if (anchor <= window)
            {
                return 0;
            }
            std::array<std::uintptr_t, MANAGER_WINDOW_SLOTS> values{};
            const std::uintptr_t first = anchor - window;
            if (!DMK::memory::read_into(DMK::Address{first}, std::as_writable_bytes(std::span{values})).has_value())
            {
                return 0;
            }
            std::uintptr_t slot = 0;
            std::uintptr_t manager = 0;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                const std::uintptr_t value = values[i];
                if (!DMK::memory::is_plausible_ptr(DMK::Address{value}) ||
                    !object_is(GameClass::MergedMeshesManager, value))
                {
                    continue;
                }
                if (manager != 0 && value != manager)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: two CMergedMeshesManager pointers (0x{:016X}, 0x{:016X}) next to the CObjManager "
                        "slot",
                        manager,
                        value
                    );
                    return 0;
                }
                if (manager == 0)
                {
                    manager = value;
                    slot = first + i * sizeof(std::uintptr_t);
                }
            }
            return slot;
        }

        /** @brief The live manager behind the found slot, RTTI-checked, or 0. */
        [[nodiscard]] std::uintptr_t current_manager() noexcept
        {
            if (s_manager_slot == 0)
            {
                return 0;
            }
            const auto manager = DMK::memory::read<std::uintptr_t>(DMK::Address{s_manager_slot});
            return manager && object_is(GameClass::MergedMeshesManager, *manager) ? *manager : 0;
        }

        /**
         * @brief Keeps the octree query for the rest of the session and says why, once.
         */
        void disable_manager(const std::string &reason)
        {
            if (s_cell_source == CellSource::Octree)
            {
                return;
            }
            s_cell_source = CellSource::Octree;
            s_cells.clear();
            s_cells_stamp.valid = false;
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "HerbScan: {}; merged-mesh cells come from the octree query for this session",
                reason
            );
        }

        /**
         * @brief Looks for the manager slot once, when the cell source is still unsearched.
         */
        void ensure_manager_searched()
        {
            if (s_cell_source != CellSource::Unsearched)
            {
                return;
            }
            if (class_vtable(GameClass::MergedMeshesManager) == 0)
            {
                disable_manager("the CMergedMeshesManager identity did not resolve");
                return;
            }
            s_manager_slot = find_manager_slot();
            if (s_manager_slot == 0)
            {
                disable_manager("no CMergedMeshesManager next to the CObjManager slot");
                return;
            }
            s_cell_source = CellSource::Unverified;
            s_cross_check_mismatches = 0;
            s_lookup_failures = 0;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: CMergedMeshesManager slot 0x{:016X} (CObjManager slot {:+#x}); checking it against the "
                "octree before use",
                s_manager_slot,
                static_cast<std::ptrdiff_t>(s_manager_slot - anchor_address(AnchorId::ObjManager))
            );
        }

        /** @brief Bucket coordinate of @p value on one axis, as FindNode computes it. */
        [[nodiscard]] std::uint32_t bucket_coordinate(float value, std::uint32_t dimension) noexcept
        {
            constexpr float inverse_cell = 1.0f / constants::MERGED_MESHES_HASH_CELL_SIZE;
            return static_cast<std::uint32_t>(static_cast<int>(std::fabs(value) * inverse_cell)) & (dimension - 1);
        }

        /**
         * @brief Collects the merged-mesh cells whose visible bounds overlap @p box from the manager's buckets.
         * @details A cell is filed under the bucket of the point it was created for, anywhere inside it, so the buckets
         *          of every 16 m grid cell the box touches, plus one cell of margin, are read on both z layers. Every
         *          node is vtable-checked and its bounds re-tested, because a bucket also holds the cells 512 m apart.
         * @param manager The CMergedMeshesManager.
         * @param box World box.
         * @param out Receives the cells (vtable and structure checked, any state), cleared first.
         * @param read Receives the number of bucket entries read.
         * @param failure Receives why the lookup failed, for the log.
         * @return False on a read fault or a bucket that does not look like a vector of node pointers (a vector the
         *         engine is growing can read that way once; the caller decides whether that is transient).
         */
        [[nodiscard]] bool lookup_cells(
            std::uintptr_t manager,
            const game_structures::Aabb &box,
            std::vector<std::uintptr_t> &out,
            std::uint32_t &read,
            std::string &failure
        )
        {
            out.clear();
            read = 0;
            constexpr float cell_size = constants::MERGED_MESHES_HASH_CELL_SIZE;
            constexpr std::uint32_t dim_xy = constants::MERGED_MESHES_HASH_DIM_XY;
            constexpr std::uint32_t dim_z = constants::MERGED_MESHES_HASH_DIM_Z;
            if (!all_finite(box.min) || !all_finite(box.max))
            {
                failure = "the scan box is not finite";
                return false;
            }
            const auto first_x = static_cast<int>(std::floor((box.min.x - cell_size) / cell_size));
            const auto first_y = static_cast<int>(std::floor((box.min.y - cell_size) / cell_size));
            // Past 32 grid cells an axis wraps onto buckets already read.
            const int last_x = std::min(
                static_cast<int>(std::floor((box.max.x + cell_size) / cell_size)),
                first_x + static_cast<int>(dim_xy) - 1
            );
            const int last_y = std::min(
                static_cast<int>(std::floor((box.max.y + cell_size) / cell_size)),
                first_y + static_cast<int>(dim_xy) - 1
            );
            s_bucket_seen.reset();
            for (int ix = first_x; ix <= last_x; ++ix)
            {
                const std::uint32_t bx = bucket_coordinate((static_cast<float>(ix) + 0.5f) * cell_size, dim_xy);
                for (int iy = first_y; iy <= last_y; ++iy)
                {
                    const std::uint32_t by = bucket_coordinate((static_cast<float>(iy) + 0.5f) * cell_size, dim_xy);
                    for (std::uint32_t bz = 0; bz < dim_z; ++bz)
                    {
                        const std::size_t index = bz + dim_z * (dim_xy * bx + by);
                        if (s_bucket_seen.test(index))
                        {
                            continue;
                        }
                        s_bucket_seen.set(index);
                        const std::uintptr_t bucket = manager + constants::MERGED_MESHES_MANAGER_BUCKETS_OFFSET +
                                                      index * constants::MERGED_MESHES_MANAGER_BUCKET_STRIDE;
                        const auto range = DMK::memory::read<std::array<std::uintptr_t, 2>>(DMK::Address{bucket});
                        if (!range)
                        {
                            failure = std::format("bucket {} at 0x{:016X} is unreadable", index, bucket);
                            return false;
                        }
                        const std::uintptr_t begin = (*range)[0];
                        const std::uintptr_t end = (*range)[1];
                        if (begin == end)
                        {
                            continue;
                        }
                        const std::uintptr_t bytes = end - begin;
                        if (end < begin || !DMK::memory::is_plausible_ptr(DMK::Address{begin}) ||
                            bytes % sizeof(std::uintptr_t) != 0 ||
                            bytes / sizeof(std::uintptr_t) > constants::HERB_MAX_CELLS_PER_BUCKET)
                        {
                            failure = std::format(
                                "bucket {} reads as {{0x{:016X}, 0x{:016X}}}, not a vector of cell pointers",
                                index,
                                begin,
                                end
                            );
                            return false;
                        }
                        s_bucket_nodes.resize(static_cast<std::size_t>(bytes / sizeof(std::uintptr_t)));
                        const std::span<std::byte> storage = std::as_writable_bytes(std::span{s_bucket_nodes});
                        if (!DMK::memory::read_into(DMK::Address{begin}, storage).has_value())
                        {
                            failure = std::format("bucket {} storage at 0x{:016X} is unreadable", index, begin);
                            return false;
                        }
                        read += static_cast<std::uint32_t>(s_bucket_nodes.size());
                        if (read > constants::HERB_MAX_OCTREE_NODES)
                        {
                            failure = std::format("more than {} bucket entries", constants::HERB_MAX_OCTREE_NODES);
                            return false;
                        }
                        for (const std::uintptr_t node : s_bucket_nodes)
                        {
                            // A node being destroyed fails the vtable test or the guarded reads and is skipped.
                            MergedMeshCell cell{};
                            if (read_merged_mesh_cell(node, cell) && overlaps(cell.visible, box))
                            {
                                out.push_back(node);
                            }
                        }
                    }
                }
            }
            return true;
        }

        /** @brief Sorted streamed-in cells of @p nodes (vtable and structure checked) into @p out. */
        void streamed_cells(std::span<const std::uintptr_t> nodes, std::vector<std::uintptr_t> &out)
        {
            out.clear();
            for (const std::uintptr_t node : nodes)
            {
                MergedMeshCell cell{};
                if (read_merged_mesh_cell(node, cell) && streamed_in(cell))
                {
                    out.push_back(node);
                }
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }

        /**
         * @brief Compares the manager's cells with the octree's for the box of an octree query just made.
         * @details Both sides keep the streamed-in merged-mesh cells whose visible bounds overlap the box (the octree
         *          tests the same bounds). Equal sets switch the scan to the manager for the session; a difference is
         *          retried on the next octree queries (a cell can finish streaming between the two reads) and after
         *          MAX_CROSS_CHECK_MISMATCHES keeps the octree. With no cell on either side there is nothing to
         *          compare and the check waits for the next query.
         */
        void cross_check_manager(const game_structures::Aabb &box)
        {
            DMK_PROFILE_FUNCTION();
            const std::uintptr_t manager = current_manager();
            if (manager == 0)
            {
                disable_manager(std::format("slot 0x{:016X} no longer holds a CMergedMeshesManager", s_manager_slot));
                return;
            }
            std::uint32_t read = 0;
            std::string failure;
            if (!lookup_cells(manager, box, s_check_lookup, read, failure))
            {
                // A bucket caught while the engine grows it reads torn once; it counts as a disagreement.
                ++s_cross_check_mismatches;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "HerbScan: CMergedMeshesManager cross-check {}/{}: the lookup failed ({})",
                    s_cross_check_mismatches,
                    MAX_CROSS_CHECK_MISMATCHES,
                    failure
                );
                if (s_cross_check_mismatches >= MAX_CROSS_CHECK_MISMATCHES)
                {
                    disable_manager(
                        std::format(
                            "the CMergedMeshesManager behind slot 0x{:016X} did not read as a cell index {} times "
                            "({} the last time)",
                            s_manager_slot,
                            s_cross_check_mismatches,
                            failure
                        )
                    );
                }
                return;
            }
            streamed_cells(s_check_lookup, s_check_manager);
            streamed_cells(s_nodes, s_check_octree);
            if (s_check_octree.empty() && s_check_manager.empty())
            {
                return;
            }
            std::vector<std::uintptr_t> missing;
            std::set_difference(
                s_check_octree.begin(),
                s_check_octree.end(),
                s_check_manager.begin(),
                s_check_manager.end(),
                std::back_inserter(missing)
            );
            std::vector<std::uintptr_t> extra;
            std::set_difference(
                s_check_manager.begin(),
                s_check_manager.end(),
                s_check_octree.begin(),
                s_check_octree.end(),
                std::back_inserter(extra)
            );
            if (missing.empty() && extra.empty())
            {
                s_cell_source = CellSource::Manager;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "HerbScan: merged-mesh cells come from CMergedMeshesManager 0x{:016X} (slot 0x{:016X}): it lists "
                    "the same {} streamed-in cell(s) as the octree query ({} bucket entries read); no octree query "
                    "from now on",
                    manager,
                    s_manager_slot,
                    s_check_octree.size(),
                    read
                );
                return;
            }
            ++s_cross_check_mismatches;
            const std::uintptr_t example = !missing.empty() ? missing.front() : extra.front();
            MergedMeshCell cell{};
            const bool readable = read_merged_mesh_cell(example, cell);
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: CMergedMeshesManager cross-check {}/{}: {} octree cell(s) missing from the index, {} extra "
                "(of {} octree, {} index); e.g. {} cell 0x{:016X} state {} visible ({:.1f}, {:.1f}, {:.1f})-({:.1f}, "
                "{:.1f}, {:.1f})",
                s_cross_check_mismatches,
                MAX_CROSS_CHECK_MISMATCHES,
                missing.size(),
                extra.size(),
                s_check_octree.size(),
                s_check_manager.size(),
                !missing.empty() ? "missing" : "extra",
                example,
                readable ? cell.state : 0u,
                cell.visible.min.x,
                cell.visible.min.y,
                cell.visible.min.z,
                cell.visible.max.x,
                cell.visible.max.y,
                cell.visible.max.z
            );
            if (s_cross_check_mismatches >= MAX_CROSS_CHECK_MISMATCHES)
            {
                disable_manager(
                    std::format(
                        "CMergedMeshesManager 0x{:016X} disagreed with the octree query {} times ({} missing, {} "
                        "extra the last time)",
                        manager,
                        s_cross_check_mismatches,
                        missing.size(),
                        extra.size()
                    )
                );
            }
        }

        /**
         * @brief Looks the cells of the scan box up in the manager's index.
         * @details A manager that fails its RTTI check keeps the octree for the session at once. A failed lookup (a
         *          bucket read torn while the engine grows it) leaves this scan to the octree query, and only
         *          MAX_LOOKUP_FAILURES of them in a row keep the octree for the session.
         * @return The number of bucket entries read, or std::nullopt when the index is not in use or failed.
         */
        [[nodiscard]] std::optional<std::uint32_t> manager_cells(const game_structures::Vec3f &center, float radius)
        {
            if (s_cell_source != CellSource::Manager)
            {
                return std::nullopt;
            }
            const std::uintptr_t manager = current_manager();
            if (manager == 0)
            {
                disable_manager(std::format("slot 0x{:016X} no longer holds a CMergedMeshesManager", s_manager_slot));
                return std::nullopt;
            }
            std::uint32_t read = 0;
            std::string failure;
            if (!lookup_cells(manager, scan_box(center, radius), s_cells, read, failure))
            {
                s_cells.clear();
                // The cached octree cells, if any, are older than the index's; this scan queries afresh.
                s_cells_stamp.valid = false;
                ++s_lookup_failures;
                if (s_lookup_failures >= MAX_LOOKUP_FAILURES)
                {
                    disable_manager(
                        std::format(
                            "CMergedMeshesManager 0x{:016X} lookups failed {} times in a row ({} the last time)",
                            manager,
                            s_lookup_failures,
                            failure
                        )
                    );
                }
                else
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: CMergedMeshesManager 0x{:016X} lookup failed ({}; {}/{} in a row); this scan uses "
                        "the octree query",
                        manager,
                        failure,
                        s_lookup_failures,
                        MAX_LOOKUP_FAILURES
                    );
                }
                return std::nullopt;
            }
            s_lookup_failures = 0;
            return read;
        }

        /**
         * @brief Asks the render octree for the merged-mesh cells holding pickable groups around @p center.
         * @details Only merged-mesh nodes are asked for: the octree then skips the brushes and vegetation instances
         *          that fill the box (a query of every node type took 3-5 ms in a town, most of it spent on those).
         *          While the manager is unverified, the same box is cross-checked against it.
         * @return The number of merged-mesh cells the query returned, or std::nullopt when the query failed.
         */
        [[nodiscard]] std::optional<std::uint32_t> query_cells(const game_structures::Vec3f &center, float radius)
        {
            DMK_PROFILE_FUNCTION();
            constexpr std::array<std::uint32_t, 1> cell_type{constants::RENDERNODE_TYPE_MERGED_MESH};
            s_cells.clear();
            s_cells_stamp.valid = false;
            const game_structures::Aabb box = scan_box(center, radius + REQUERY_DISTANCE);
            switch (query_render_nodes_of_types(box, cell_type, s_nodes, constants::HERB_MAX_OCTREE_NODES))
            {
            case OctreeQueryResult::Ok:
                break;
            case OctreeQueryResult::Faulted:
                s_faulted.store(true, std::memory_order_relaxed);
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "HerbScan: the octree query faulted; herbs are off for this session"
                );
                return std::nullopt;
            case OctreeQueryResult::TooMany:
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "HerbScan: more than {} merged-mesh cells around the player; skipped",
                    constants::HERB_MAX_OCTREE_NODES
                );
                return std::nullopt;
            case OctreeQueryResult::Unavailable:
            default:
                return std::nullopt;
            }
            if (s_cell_source == CellSource::Unverified)
            {
                cross_check_manager(box);
            }
            for (const std::uintptr_t node : s_nodes)
            {
                MergedMeshCell cell{};
                if (read_merged_mesh_cell(node, cell) && cell_has_pickable_group(cell))
                {
                    s_cells.push_back(node);
                }
            }
            stamp_now(s_cells_stamp, center, radius);
            return static_cast<std::uint32_t>(s_nodes.size());
        }

        // Pickable vegetation nodes (mushrooms)

        [[nodiscard]] bool call_get_entity_stat_obj(
            std::uintptr_t fn,
            std::uintptr_t node,
            game_structures::Matrix34f *matrix,
            std::uintptr_t *stat_obj
        ) noexcept
        {
            __try
            {
                *stat_obj = reinterpret_cast<GetEntityStatObjFn>(fn)(node, 0, matrix);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] game_structures::Vec3f translation(const game_structures::Matrix34f &m) noexcept
        {
            return game_structures::Vec3f{m.m[0][3], m.m[1][3], m.m[2][3]};
        }

        [[nodiscard]] bool
        within_tolerance(const game_structures::Vec3f &a, const game_structures::Vec3f &b, float tolerance) noexcept
        {
            return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
                   std::fabs(a.z - b.z) <= tolerance;
        }

        /** @brief True the first time a vegetation node the query returned is logged (once per node and level). */
        [[nodiscard]] bool first_vegetation_log(std::uintptr_t node)
        {
            return DMK::log().is_enabled(DMK::LogLevel::Debug) && s_logged_vegetation.insert(node).second;
        }

        /**
         * @brief Turns the mushroom provider off for the session and says why.
         */
        void disable_vegetation(DMK::LogLevel level, const std::string &reason)
        {
            s_vegetation_off = true;
            s_vegetation.clear();
            s_vegetation_stamp.valid = false;
            (void)DMK::log().try_log(level, "HerbScan: {}; pickable mushrooms are off for this session", reason);
        }

        /**
         * @brief Validates one node of the vegetation query and reads its transform and model.
         * @details The model and matrix come from CVegetation::GetEntityStatObj (vtable slot 22), called only on a node
         *          the query has just returned. Its result must be the node's own group CStatObj and its translation
         *          the node's position; anything else means the slot is not that method and turns mushrooms off.
         * @return True when @p out holds a pickable mushroom.
         */
        [[nodiscard]] bool resolve_vegetation(std::uintptr_t node, PickableVegetation &out)
        {
            if (!object_is(GameClass::Vegetation, node))
            {
                if (first_vegetation_log(node))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: vegetation node 0x{:016X} rejected: not a CVegetation",
                        node
                    );
                }
                return false;
            }
            const auto flags =
                DMK::memory::read<std::uint64_t>(DMK::Address{node + constants::RENDERNODE_RNDFLAGS_OFFSET});
            const auto group =
                DMK::memory::read<std::int32_t>(DMK::Address{node + constants::VEGETATION_GROUP_INDEX_OFFSET});
            const auto position =
                DMK::memory::read<game_structures::Vec3f>(DMK::Address{node + constants::VEGETATION_POSITION_OFFSET});
            if (!flags || !group || !position || (*flags & constants::ERF_PICKABLE) == 0 || !all_finite(*position))
            {
                if (first_vegetation_log(node))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: vegetation node 0x{:016X} rejected: unreadable or not pickable",
                        node
                    );
                }
                return false;
            }
            if (!group_is_pickable(*group))
            {
                if (first_vegetation_log(node))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: vegetation node 0x{:016X} at ({:.2f}, {:.2f}, {:.2f}) rejected: group {} is not a "
                        "pickable group",
                        node,
                        position->x,
                        position->y,
                        position->z,
                        *group
                    );
                }
                return false;
            }
            const std::uintptr_t fn = read_vtable_slot(node, constants::VEGETATION_VTABLE_GET_ENTITY_STAT_OBJ_OFFSET);
            if (fn == 0)
            {
                disable_vegetation(
                    DMK::LogLevel::Warning,
                    std::format("CVegetation 0x{:016X} has no GetEntityStatObj in the game image", node)
                );
                return false;
            }
            alignas(16) game_structures::Matrix34f world{};
            std::uintptr_t stat_obj = 0;
            if (!call_get_entity_stat_obj(fn, node, &world, &stat_obj))
            {
                disable_vegetation(
                    DMK::LogLevel::Error,
                    std::format("CVegetation::GetEntityStatObj faulted on 0x{:016X}", node)
                );
                return false;
            }
            const std::uintptr_t expected = group_stat_obj(*group);
            const game_structures::Vec3f at = translation(world);
            if (stat_obj == 0 || stat_obj != expected || !all_finite(at) ||
                !within_tolerance(at, *position, NODE_MOVED_TOLERANCE))
            {
                disable_vegetation(
                    DMK::LogLevel::Warning,
                    std::format(
                        "CVegetation 0x{:016X} slot 22 returned model 0x{:016X} at ({:.2f}, {:.2f}, {:.2f}), not its "
                        "group {} model 0x{:016X} at ({:.2f}, {:.2f}, {:.2f})",
                        node,
                        stat_obj,
                        at.x,
                        at.y,
                        at.z,
                        *group,
                        expected,
                        position->x,
                        position->y,
                        position->z
                    )
                );
                return false;
            }
            const bool hidden = (*flags & constants::ERF_HIDDEN) != 0;
            out = PickableVegetation{node, world, stat_obj, *group, hidden};
            log_group_once(*group);
            if (first_vegetation_log(node))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "HerbScan: mushroom 0x{:016X} (group {}) at ({:.2f}, {:.2f}, {:.2f}){}",
                    node,
                    *group,
                    at.x,
                    at.y,
                    at.z,
                    hidden ? ", picked" : ""
                );
            }
            return true;
        }

        /**
         * @brief Asks the octree for the pickable CVegetation nodes around @p center (type 2 with the ERF_PICKABLE
         *        mask, so the engine drops every other vegetation instance before it reads its bounds).
         */
        void query_vegetation(const game_structures::Vec3f &center, float radius)
        {
            DMK_PROFILE_FUNCTION();
            constexpr std::array<std::uint32_t, 1> vegetation_type{constants::RENDERNODE_TYPE_VEGETATION};
            const auto started = std::chrono::steady_clock::now();
            s_vegetation.clear();
            s_vegetation_stamp.valid = false;
            const game_structures::Aabb box = scan_box(center, radius + REQUERY_DISTANCE);
            switch (query_render_nodes_of_types(
                box,
                vegetation_type,
                s_nodes,
                constants::HERB_MAX_VEGETATION_NODES,
                constants::ERF_PICKABLE
            ))
            {
            case OctreeQueryResult::Ok:
                break;
            case OctreeQueryResult::Faulted:
                disable_vegetation(DMK::LogLevel::Error, "the pickable vegetation query faulted");
                return;
            case OctreeQueryResult::TooMany:
                // Retried after the usual interval rather than every scan.
                stamp_now(s_vegetation_stamp, center, radius);
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "HerbScan: more than {} pickable vegetation nodes around the player; skipped",
                    constants::HERB_MAX_VEGETATION_NODES
                );
                return;
            case OctreeQueryResult::Unavailable:
            default:
                return;
            }
            std::size_t picked = 0;
            for (const std::uintptr_t node : s_nodes)
            {
                PickableVegetation entry{};
                if (!resolve_vegetation(node, entry))
                {
                    if (s_vegetation_off)
                    {
                        return;
                    }
                    continue;
                }
                picked += entry.hidden ? 1 : 0;
                s_vegetation.push_back(entry);
            }
            stamp_now(s_vegetation_stamp, center, radius);
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: pickable vegetation query around ({:.0f}, {:.0f}, {:.0f}) +-{:.0f} m: {} node(s), {} "
                "mushroom(s) kept ({} picked), {:.3f} ms",
                center.x,
                center.y,
                center.z,
                radius + REQUERY_DISTANCE,
                s_nodes.size(),
                s_vegetation.size(),
                picked,
                elapsed_ms(started)
            );
        }

        /**
         * @brief Adds the unpicked mushrooms of the cached vegetation nodes within the radius.
         * @details Each node is re-read every scan: a freed node fails the vtable or position test, and a picked one
         *          carries ERF_HIDDEN until it respawns.
         */
        void scan_vegetation(const game_structures::Vec3f &center, float radius_sq)
        {
            for (PickableVegetation &entry : s_vegetation)
            {
                if (!object_is(GameClass::Vegetation, entry.node))
                {
                    continue;
                }
                const auto flags =
                    DMK::memory::read<std::uint64_t>(DMK::Address{entry.node + constants::RENDERNODE_RNDFLAGS_OFFSET});
                const auto position = DMK::memory::read<game_structures::Vec3f>(
                    DMK::Address{entry.node + constants::VEGETATION_POSITION_OFFSET}
                );
                const game_structures::Vec3f p = translation(entry.world);
                if (!flags || !position || (*flags & constants::ERF_PICKABLE) == 0 ||
                    !within_tolerance(*position, p, NODE_MOVED_TOLERANCE))
                {
                    continue;
                }
                const bool hidden = (*flags & constants::ERF_HIDDEN) != 0;
                if (hidden != entry.hidden)
                {
                    entry.hidden = hidden;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "HerbScan: mushroom 0x{:016X} (group {}) at ({:.2f}, {:.2f}, {:.2f}) {}",
                        entry.node,
                        entry.group,
                        p.x,
                        p.y,
                        p.z,
                        hidden ? "picked" : "respawned"
                    );
                }
                const float dx = p.x - center.x;
                const float dy = p.y - center.y;
                const float dz = p.z - center.z;
                const float distance_sq = dx * dx + dy * dy + dz * dz;
                if (distance_sq > radius_sq)
                {
                    continue;
                }
                if (hidden)
                {
                    ++s_counts.mushrooms_picked;
                    continue;
                }
                ++s_counts.plants;
                ++s_counts.mushrooms;
                const std::size_t seed = add_plant(entry.group, p, distance_sq);
                if (s_plants_out != nullptr)
                {
                    add_outline_plant(seed, HerbPlant{entry.world, entry.stat_obj, std::sqrt(distance_sq)});
                }
            }
        }
    } // namespace

    DMK::Result<void> initialize_herb_scan()
    {
        if (!feature_ready(Feature::HerbScan))
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning, "HerbScan: the herb scan gate failed; herbs are off");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "herb_scan/gate"});
        }
        if (class_vtable(GameClass::MergedMeshNode) == 0 || class_vtable(GameClass::ThreeDEngine) == 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "HerbScan: CMergedMeshRenderNode or C3DEngine identity did not resolve; herbs are off"
            );
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "herb_scan/rtti"});
        }
        s_faulted.store(false, std::memory_order_relaxed);
        s_available.store(true, std::memory_order_release);
        s_table = VegetationTable{};
        s_logged_groups.clear();
        s_logged_cell_states.clear();
        s_logged_vegetation.clear();
        s_cells.clear();
        s_cells_stamp = CacheStamp{};
        s_cell_source = CellSource::Unsearched;
        s_manager_slot = 0;
        s_cross_check_mismatches = 0;
        s_lookup_failures = 0;
        s_vegetation.clear();
        s_vegetation_stamp = CacheStamp{};
        s_vegetation_off = class_vtable(GameClass::Vegetation) == 0;
        s_last_cluster_count = static_cast<std::size_t>(-1);
        s_last_plant_total = static_cast<std::uint64_t>(-1);
        s_last_outlined = static_cast<std::size_t>(-1);
        s_last_mushrooms = static_cast<std::size_t>(-1);
        s_last_unstreamed = static_cast<std::size_t>(-1);
        s_last_fallback_markers = static_cast<std::size_t>(-1);
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "HerbScan: ready (GetObjectsInBox {}, CObjManager slot {}, CMergedMeshRenderNode vtable {}, "
            "CMergedMeshesManager vtable {}, CVegetation vtable {}{})",
            DMK::format::format_address(anchor_address(AnchorId::GetObjectsInBox)),
            DMK::format::format_address(anchor_address(AnchorId::ObjManager)),
            DMK::format::format_address(class_vtable(GameClass::MergedMeshNode)),
            DMK::format::format_address(class_vtable(GameClass::MergedMeshesManager)),
            DMK::format::format_address(class_vtable(GameClass::Vegetation)),
            s_vegetation_off ? "; mushrooms are off" : ""
        );
        return {};
    }

    void shutdown_herb_scan() noexcept
    {
        s_available.store(false, std::memory_order_release);
        s_cells_stamp.valid = false;
        s_vegetation_stamp.valid = false;
    }

    bool herb_scan_available() noexcept
    {
        return s_available.load(std::memory_order_acquire) && !s_faulted.load(std::memory_order_relaxed);
    }

    std::size_t collect_herb_clusters(
        const game_structures::Vec3f &center,
        float radius,
        std::vector<HerbCluster> &out,
        std::vector<HerbPlant> *plants_out
    )
    {
        DMK_PROFILE_FUNCTION();
        out.clear();
        if (plants_out != nullptr)
        {
            plants_out->clear();
        }
        if (!herb_scan_available() || !(radius > 0.0f))
        {
            return 0;
        }
        const std::uintptr_t engine = genv_interface(constants::GENV_3DENGINE_OFFSET);
        if (engine == 0 || !object_is(GameClass::ThreeDEngine, engine) || !refresh_vegetation_table())
        {
            return 0;
        }

        const auto started = std::chrono::steady_clock::now();
        ensure_manager_searched();
        std::string source = "cached cells";
        if (const std::optional<std::uint32_t> read = manager_cells(center, radius); read.has_value())
        {
            source = std::format("the cell index ({} bucket entries)", *read);
        }
        else if (stale(s_cells_stamp, center, radius))
        {
            const std::optional<std::uint32_t> queried = query_cells(center, radius);
            if (!queried.has_value())
            {
                return 0;
            }
            source = std::format("an octree query of {} merged-mesh cells", *queried);
        }

        s_seeds.clear();
        s_counts = ScanCounts{};
        s_plants_out = plants_out;
        const float radius_sq = radius * radius;
        for (const std::uintptr_t node : s_cells)
        {
            // A cell streamed out since the query fails the vtable test or the guarded reads and is skipped.
            MergedMeshCell cell{};
            if (!read_merged_mesh_cell(node, cell))
            {
                continue;
            }
            if (!streamed_in(cell))
            {
                // Only a cell with herbs is worth a line: most cells the index returns are plain grass.
                if (cell_has_pickable_group(cell))
                {
                    ++s_counts.unstreamed_cells;
                    log_unstreamed_cell(node, cell);
                }
                continue;
            }
            const std::size_t groups = scan_cell(cell, center, radius_sq);
            s_counts.cells += groups != 0 ? 1 : 0;
            s_counts.herb_groups += groups;
        }
        if (!s_vegetation_off)
        {
            if (stale(s_vegetation_stamp, center, radius))
            {
                query_vegetation(center, radius);
            }
            scan_vegetation(center, radius_sq);
        }
        s_plants_out = nullptr;

        std::sort(
            s_seeds.begin(),
            s_seeds.end(),
            [](const ClusterSeed &a, const ClusterSeed &b) { return a.distance_sq < b.distance_sq; }
        );
        out.reserve(s_seeds.size());
        std::size_t fallback_markers = 0;
        for (const ClusterSeed &seed : s_seeds)
        {
            const float pad = constants::HERB_MARKER_PADDING;
            const bool marker_fallback = plants_out != nullptr && !seed.outlined;
            fallback_markers += marker_fallback ? 1 : 0;
            out.push_back(
                HerbCluster{
                    game_structures::Aabb{
                        {seed.min.x - pad, seed.min.y - pad, seed.min.z - pad},
                        {seed.max.x + pad, seed.max.y + pad, seed.max.z + constants::HERB_MARKER_HEIGHT}
                    },
                    seed.group,
                    seed.plants,
                    marker_fallback
                }
            );
        }

        const std::size_t outlined = plants_out != nullptr ? plants_out->size() : 0;
        if (out.size() != s_last_cluster_count || s_counts.plants != s_last_plant_total ||
            outlined != s_last_outlined || s_counts.mushrooms != s_last_mushrooms ||
            s_counts.unstreamed_cells != s_last_unstreamed || fallback_markers != s_last_fallback_markers)
        {
            s_last_cluster_count = out.size();
            s_last_plant_total = s_counts.plants;
            s_last_outlined = outlined;
            s_last_mushrooms = s_counts.mushrooms;
            s_last_unstreamed = s_counts.unstreamed_cells;
            s_last_fallback_markers = fallback_markers;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "HerbScan: {} cluster(s) of {} unpicked plant(s) within {:.1f} m ({} pickable group slot(s) in {} "
                "herb cell(s) from {}, {} herb cell(s) not streamed in; {} mushroom(s), {} picked); {} to outline ({} "
                "cluster(s) keep a marker: no model to outline), {:.2f} ms",
                out.size(),
                s_counts.plants,
                radius,
                s_counts.herb_groups,
                s_counts.cells,
                source,
                s_counts.unstreamed_cells,
                s_counts.mushrooms,
                s_counts.mushrooms_picked,
                outlined,
                fallback_markers,
                elapsed_ms(started)
            );
        }
        return out.size();
    }

} // namespace HenrySenses
