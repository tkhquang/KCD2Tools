/**
 * @file fixes/link_index.cpp
 * @brief The link map's unlink, insert and erase(node) detours and the endpoint index they share (see fixes.hpp).
 *
 * The map is an MSVC std::map keyed by (from, to, kind): a head node whose parent is the root, nodes with left,
 * parent and right links and an is-nil byte, and a size. The index describes one map at a time, the one the unlink is
 * called on. The insert and erase(node) detours keep it current whether or not the fix is on, so a fix switched back on
 * finds a current index. A node count or head that disagrees with the map invalidates the index, and the next unlink
 * rebuilds it from one walk of the tree.
 *
 * Every index access holds s_lock. No game function runs under it: an unlink takes the lock to pick and unlist one
 * node, then erases it with the lock released, so a game callback that inserts or erases through the hooks cannot
 * deadlock, and a node a nested erase already removed is never erased twice. Log lines are written after the lock is
 * released, from notes the locked section records.
 */

#include "config.hpp"
#include "constants.hpp"
#include "core/endpoint_index.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace EngineTweaks::fixes
{
    namespace
    {
        using UnlinkFunc = std::uintptr_t(__fastcall *)(void *map, std::uintptr_t object);
        using EraseNodeFunc = std::uintptr_t(__fastcall *)(void *map, std::uintptr_t node);
        using InsertFunc = void *(__fastcall *)(void *map, void *result, const void *key);

        std::atomic<UnlinkFunc> s_unlink_original{nullptr};
        std::atomic<EraseNodeFunc> s_erase_original{nullptr};
        std::atomic<InsertFunc> s_insert_original{nullptr};
        std::atomic<const std::uint8_t *> s_first_only_flag{nullptr};

        /** @brief The index and the map it describes. Guarded by s_lock. */
        struct IndexState
        {
            std::uintptr_t map{0};
            std::uintptr_t head{0};
            bool valid{false};
            core::EndpointIndex index;
            std::vector<std::uintptr_t> nodes;
            std::vector<std::uintptr_t> walk;
        };

        SRWLOCK s_lock = SRWLOCK_INIT;
        IndexState s_state;
        /** @brief The map the index describes, readable without the lock, so detours on other maps skip it. */
        std::atomic<std::uintptr_t> s_tracked_map{0};

        /** @brief Exclusive hold of s_lock for one scope. */
        class IndexLock
        {
        public:
            IndexLock() noexcept { AcquireSRWLockExclusive(&s_lock); }
            ~IndexLock() noexcept { ReleaseSRWLockExclusive(&s_lock); }
            IndexLock(const IndexLock &) = delete;
            IndexLock &operator=(const IndexLock &) = delete;
        };

        [[nodiscard]] std::uintptr_t read_word(std::uintptr_t address) noexcept
        {
            return *reinterpret_cast<const std::uintptr_t *>(address);
        }

        [[nodiscard]] std::uintptr_t map_head(std::uintptr_t map) noexcept
        {
            return read_word(map + constants::MAP_HEAD_OFFSET);
        }

        [[nodiscard]] std::size_t map_size(std::uintptr_t map) noexcept
        {
            return static_cast<std::size_t>(read_word(map + constants::MAP_SIZE_OFFSET));
        }

        [[nodiscard]] bool is_nil(std::uintptr_t node) noexcept
        {
            return *reinterpret_cast<const std::uint8_t *>(node + constants::NODE_IS_NIL_OFFSET) != 0;
        }

        [[nodiscard]] core::LinkNode link_of(std::uintptr_t node) noexcept
        {
            return core::LinkNode{
                .node = node,
                .from = read_word(node + constants::NODE_KEY_FROM_OFFSET),
                .to = read_word(node + constants::NODE_KEY_TO_OFFSET),
            };
        }

        /** @brief Empties the index. Requires s_lock. */
        void clear_index() noexcept
        {
            s_state.valid = false;
            s_state.index.clear();
        }

        /** @brief Drops an index that no longer matches the map, so the next unlink rebuilds it. Requires s_lock. */
        void drop_index() noexcept
        {
            clear_index();
            diag::counters().link_invalidations.add();
        }

        /** @brief True when the index describes @p map as it is now. Requires s_lock. */
        [[nodiscard]] bool index_matches(std::uintptr_t map) noexcept
        {
            return s_state.valid && s_state.map == map && s_state.head == map_head(map) &&
                   s_state.index.node_count() == map_size(map);
        }

        /** @brief What one rebuild found, for the log line written after s_lock is released. */
        struct RebuildNote
        {
            /// Why the index was not current. nullptr when no rebuild ran.
            const char *reason{nullptr};
            bool built{false};
            std::size_t walked{0};
            std::size_t size{0};
            double ms{0.0};
        };

        /**
         * @brief Rebuilds the index of @p map from one walk of its tree. Requires s_lock.
         * @return False when the walk did not visit exactly size() nodes, which leaves the index invalid.
         * @throws std::bad_alloc If the index or the walk stack cannot grow. The index is then invalid.
         */
        bool rebuild(std::uintptr_t map, RebuildNote &note)
        {
            const diag::Stopwatch stopwatch;
            clear_index();
            diag::counters().link_rebuilds.add();
            s_state.map = map;
            s_tracked_map.store(map, std::memory_order_release);
            const std::uintptr_t head = map_head(map);
            note.size = map_size(map);
            s_state.walk.clear();
            const std::uintptr_t root = read_word(head + constants::NODE_PARENT_OFFSET);
            if (!is_nil(root))
            {
                s_state.walk.push_back(root);
            }
            // A walk longer than the size means a cycle or a torn tree, so it stops there.
            while (!s_state.walk.empty() && note.walked <= note.size)
            {
                const std::uintptr_t node = s_state.walk.back();
                s_state.walk.pop_back();
                ++note.walked;
                s_state.index.add(link_of(node));
                const std::uintptr_t left = read_word(node + constants::NODE_LEFT_OFFSET);
                const std::uintptr_t right = read_word(node + constants::NODE_RIGHT_OFFSET);
                if (!is_nil(left))
                {
                    s_state.walk.push_back(left);
                }
                if (!is_nil(right))
                {
                    s_state.walk.push_back(right);
                }
            }
            note.ms = stopwatch.elapsed_ms();
            if (note.walked != note.size)
            {
                clear_index();
                diag::counters().link_rebuild_failures.add();
                return false;
            }
            s_state.head = head;
            s_state.valid = true;
            note.built = true;
            return true;
        }

        /** @brief Counts the nodes that touch @p object with a full walk, as the game's scan does. Requires s_lock. */
        std::size_t count_by_walk(std::uintptr_t map, std::uintptr_t object)
        {
            std::size_t matches = 0;
            s_state.walk.clear();
            const std::uintptr_t root = read_word(map_head(map) + constants::NODE_PARENT_OFFSET);
            if (!is_nil(root))
            {
                s_state.walk.push_back(root);
            }
            while (!s_state.walk.empty())
            {
                const std::uintptr_t node = s_state.walk.back();
                s_state.walk.pop_back();
                const core::LinkNode link = link_of(node);
                matches += link.from == object || link.to == object ? 1 : 0;
                const std::uintptr_t left = read_word(node + constants::NODE_LEFT_OFFSET);
                const std::uintptr_t right = read_word(node + constants::NODE_RIGHT_OFFSET);
                if (!is_nil(left))
                {
                    s_state.walk.push_back(left);
                }
                if (!is_nil(right))
                {
                    s_state.walk.push_back(right);
                }
            }
            return matches;
        }

        /** @brief What one indexed unlink step decided. */
        enum class Step : std::uint8_t
        {
            /// The returned node was unlisted and must be erased.
            Erase,
            /// The object has no node left.
            Done,
            /// The index cannot serve this unlink, so the game's scan must run.
            Fallback,
        };

        /** @brief What one step recorded under s_lock, for the log lines written after it is released. */
        struct StepNote
        {
            RebuildNote rebuild{};
            /// Why the step fell back to the game's scan. nullptr when it did not.
            const char *fallback{nullptr};
            bool verified{false};
            std::size_t listed{0};
            std::size_t walked{0};
        };

        /**
         * @brief Picks one node of @p object and unlists it. Takes s_lock.
         * @param first True on the first step of an unlink, which can rebuild the index and verify it.
         */
        Step
        next_node(std::uintptr_t map, std::uintptr_t object, bool first, std::uintptr_t &node, StepNote &note) noexcept
        {
            try
            {
                const IndexLock lock;
                if (!index_matches(map))
                {
                    // Only the first step can rebuild: a mismatch later means the map changed under the unlink.
                    if (!first)
                    {
                        note.fallback = "the map changed during the unlink";
                        return Step::Fallback;
                    }
                    note.rebuild.reason = !s_state.valid       ? "no current index"
                                          : s_state.map != map ? "another map"
                                          : s_state.head != map_head(map)
                                              ? "the map's head moved"
                                              : "the node count differs from the map's size";
                    if (!rebuild(map, note.rebuild))
                    {
                        note.fallback = "the index could not be built";
                        return Step::Fallback;
                    }
                }
                if (first && verify_link_index())
                {
                    s_state.nodes.clear();
                    s_state.index.collect(object, s_state.nodes);
                    note.verified = true;
                    note.listed = s_state.nodes.size();
                    note.walked = count_by_walk(map, object);
                    if (note.listed != note.walked)
                    {
                        diag::counters().link_verify_mismatches.add();
                        drop_index();
                        note.fallback = "verify mismatch";
                        return Step::Fallback;
                    }
                }
                const std::optional<std::uintptr_t> last = s_state.index.last(object);
                if (!last.has_value())
                {
                    return Step::Done;
                }
                node = *last;
                if (!s_state.index.remove(link_of(node)))
                {
                    drop_index();
                    note.fallback = "a listed node was missing from the index";
                    return Step::Fallback;
                }
                return Step::Erase;
            }
            catch (...)
            {
                // Only an allocation of the index or a scratch vector throws, before any node was unlisted.
                const IndexLock lock;
                drop_index();
                note.fallback = "out of memory";
                return Step::Fallback;
            }
        }

        /** @brief Writes the log lines of one step's note. Runs with s_lock released. */
        void log_step_note(std::uintptr_t map, std::uintptr_t object, const StepNote &note) noexcept
        {
            if (note.rebuild.reason != nullptr && note.rebuild.built)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "LinkIndex: index of map {:#x} rebuilt ({}): {} node(s) in {:.2f} ms",
                    map,
                    note.rebuild.reason,
                    note.rebuild.walked,
                    note.rebuild.ms
                );
            }
            else if (note.rebuild.reason != nullptr)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "LinkIndex: rebuild of map {:#x} ({}) walked {} node(s), the map holds {}, so the game's scan runs",
                    map,
                    note.rebuild.reason,
                    note.rebuild.walked,
                    note.rebuild.size
                );
            }
            if (note.verified)
            {
                (void)DMK::log().try_log(
                    note.listed == note.walked ? DMK::LogLevel::Trace : DMK::LogLevel::Warning,
                    "LinkIndex: the index lists {} link(s) of {:#x}, the map holds {}",
                    note.listed,
                    object,
                    note.walked
                );
            }
        }

        /**
         * @brief The unlink detour: erases every node that touches @p object, found through the index.
         * @return The map's end iterator, as the game's scan returns after a full pass.
         * @note Any thread that destroys a linkable object. No-throw.
         */
        std::uintptr_t __fastcall detour_unlink(void *map, std::uintptr_t object) noexcept
        {
            const UnlinkFunc original = s_unlink_original.load(std::memory_order_acquire);
            const std::uint8_t *first_only = s_first_only_flag.load(std::memory_order_acquire);
            const bool enabled = fix_enabled(Fix::LinkIndex);
            // The index erases every match. The scan stops after one when the flag is set, so it keeps that case.
            if (map == nullptr || !enabled || first_only == nullptr || *first_only != 0)
            {
                diag::counters().link_scans.add();
                (void)DMK::log().try_log(
                    DMK::LogLevel::Trace,
                    "LinkIndex: unlink of {:#x} passes through ({})",
                    object,
                    map == nullptr          ? "no map"
                    : !enabled              ? "fix off"
                    : first_only == nullptr ? "no first-only flag"
                                            : "first-only unlink"
                );
                return original(map, object);
            }
            const auto map_address = reinterpret_cast<std::uintptr_t>(map);
            const EraseNodeFunc erase = s_erase_original.load(std::memory_order_acquire);
            std::size_t erased = 0;
            for (bool first = true;; first = false)
            {
                std::uintptr_t node = 0;
                StepNote note{};
                const Step step = next_node(map_address, object, first, node, note);
                log_step_note(map_address, object, note);
                switch (step)
                {
                case Step::Erase:
                    erase(map, node);
                    ++erased;
                    break;
                case Step::Done:
                    diag::counters().link_unlinks.add();
                    diag::counters().link_nodes.add(erased);
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "LinkIndex: unlinked {:#x} from map {:#x}: {} link(s) by the index",
                        object,
                        map_address,
                        erased
                    );
                    diag::maybe_log_summary();
                    return map_head(map_address);
                case Step::Fallback:
                    diag::counters().link_scans.add();
                    diag::counters().link_fallbacks.add();
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "LinkIndex: unlink of {:#x} falls back to the game's scan after {} link(s) by the index ({})",
                        object,
                        erased,
                        note.fallback
                    );
                    // The scan erases whatever the indexed steps left, so the result is the same.
                    return original(map, object);
                }
            }
        }

        /**
         * @brief The erase(node) detour: unlists the node from the index, then erases it.
         * @note Any thread. No-throw.
         */
        std::uintptr_t __fastcall detour_erase_node(void *map, std::uintptr_t node) noexcept
        {
            const EraseNodeFunc original = s_erase_original.load(std::memory_order_acquire);
            const auto map_address = reinterpret_cast<std::uintptr_t>(map);
            if (map != nullptr && map_address == s_tracked_map.load(std::memory_order_acquire))
            {
                bool dropped = false;
                {
                    const IndexLock lock;
                    diag::counters().link_erases.add();
                    if (s_state.valid && s_state.map == map_address && !s_state.index.remove(link_of(node)))
                    {
                        drop_index();
                        dropped = true;
                    }
                }
                if (dropped)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "LinkIndex: index of map {:#x} dropped, an erased node was not in it",
                        map_address
                    );
                }
            }
            return original(map, node);
        }

        /**
         * @brief The try_emplace detour: lists a newly inserted node in the index.
         * @note Any thread. No-throw.
         */
        void *__fastcall detour_insert(void *map, void *result, const void *key) noexcept
        {
            const InsertFunc original = s_insert_original.load(std::memory_order_acquire);
            void *const returned = original(map, result, key);
            const auto map_address = reinterpret_cast<std::uintptr_t>(map);
            if (map == nullptr || result == nullptr || map_address != s_tracked_map.load(std::memory_order_acquire))
            {
                return returned;
            }
            const auto result_address = reinterpret_cast<std::uintptr_t>(result);
            const bool inserted =
                *reinterpret_cast<const std::uint8_t *>(result_address + constants::INSERT_RESULT_INSERTED_OFFSET) != 0;
            if (!inserted)
            {
                return returned;
            }
            const std::uintptr_t node = read_word(result_address + constants::INSERT_RESULT_NODE_OFFSET);
            bool dropped = false;
            {
                const IndexLock lock;
                diag::counters().link_inserts.add();
                if (s_state.valid && s_state.map == map_address)
                {
                    try
                    {
                        s_state.index.add(link_of(node));
                    }
                    catch (...)
                    {
                        drop_index();
                        dropped = true;
                    }
                }
            }
            if (dropped)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "LinkIndex: index of map {:#x} dropped, out of memory on insert",
                    map_address
                );
            }
            return returned;
        }
    } // namespace

    DMK::Result<void> install_link_index(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.link_index)
        {
            return {};
        }
        s_first_only_flag.store(
            reinterpret_cast<const std::uint8_t *>(anchors.link_unlink_first_only_flag),
            std::memory_order_release
        );
        // The erase and insert hooks go first: the unlink calls the erase trampoline, and an index the unlink builds
        // must see every later insert.
        DMK_TRY_VOID(
            install_inline_hook<&detour_erase_node>(
                hooks,
                "LinkMapEraseNode",
                anchors.link_map_erase_node,
                s_erase_original
            )
        );
        DMK_TRY_VOID(
            install_inline_hook<&detour_insert>(hooks, "LinkMapInsert", anchors.link_map_insert, s_insert_original)
        );
        return install_inline_hook<&detour_unlink>(
            hooks,
            "LinkUnlinkScan",
            anchors.link_unlink_scan,
            s_unlink_original
        );
    }

    void reset_link_index() noexcept
    {
        const IndexLock lock;
        clear_index();
        s_state.map = 0;
        s_tracked_map.store(0, std::memory_order_release);
        // Return the memory now: a reload maps a new image while this one can stay mapped.
        s_state.nodes = {};
        s_state.walk = {};
    }
} // namespace EngineTweaks::fixes
