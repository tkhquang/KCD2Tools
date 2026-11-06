/**
 * @file custom_tags.cpp
 * @brief The custom tag registry and its per-frame conditions (see custom_tags.hpp).
 */

#include "custom_tags.hpp"
#include "console_vars.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace AnimationFramework
{
    namespace
    {
        /// The source of the built-in thirdPerson tag, which follows the camera view instead of its variable.
        constexpr std::string_view BUILT_IN_SOURCE = "built-in";

        /** @brief What turns one tag on. */
        struct Condition
        {
            /// thirdPerson: the camera view. Otherwise the console variable below.
            bool camera_view = false;
            std::string cvar;
            std::int32_t min_value = 1;
            std::string name;
            std::string source;
        };

        /** @brief One generation: the snapshot the queries read and the conditions the frame tick evaluates. */
        struct Published
        {
            CustomTagSnapshot snapshot;
            /// Index-aligned with the snapshot's tags.
            std::vector<Condition> conditions;
            /// The declarations it was built from, to tell an unchanged reload.
            std::vector<CustomTagDeclaration> declared;
        };

        // Readers on any thread load the pointer. Every generation lives as long as this image, so a pointer a query
        // still holds stays valid after a reload publishes the next one.
        std::atomic<const Published *> s_published{nullptr};
        std::mutex s_owner_mutex;
        std::vector<std::unique_ptr<Published>> s_owned;
        // generation << 32 | active mask, written by the frame tick.
        std::atomic<std::uint64_t> s_active{0};
        // Set by each load of Henry's tag definition, turned into an epoch step by the next frame tick.
        std::atomic<bool> s_definition_loaded{false};
        std::atomic<std::uint32_t> s_definition_epoch{0};

        bool s_third_person = false;
        std::atomic<bool> s_mod_tags{true};
        std::vector<std::string> s_ini_priority;
        std::vector<std::string> s_ini_disabled;

        // Main thread (the frame tick) only.
        std::uint32_t s_poll_generation = 0;
        std::uint32_t s_poll_mask = 0;
        std::array<void *, tag_rules::MAX_CUSTOM_TAGS> s_cvars{};
        std::array<bool, tag_rules::MAX_CUSTOM_TAGS> s_cvar_failed{};

        /// Whether @p a and @p b declare the same tags in the same order.
        [[nodiscard]] bool same_declarations(
            const std::vector<CustomTagDeclaration> &a,
            const std::vector<CustomTagDeclaration> &b
        ) noexcept
        {
            return a.size() == b.size() && std::equal(
                                               a.begin(),
                                               a.end(),
                                               b.begin(),
                                               [](const CustomTagDeclaration &x, const CustomTagDeclaration &y)
                                               {
                                                   return x.name == y.name && x.cvar == y.cvar &&
                                                          x.min_value == y.min_value && x.priority == y.priority &&
                                                          x.source == y.source;
                                               }
                                           );
        }

        /**
         * @brief Ranks @p mod_declared with the built-in tags and publishes the result as the next generation.
         * @details The log lines are built under the owner lock and written after it is released.
         */
        void publish(const std::vector<CustomTagDeclaration> &mod_declared)
        {
            std::string dropped;
            std::string order;
            std::uint32_t generation = 0;
            {
                // Setup and Henry's tag definition loads may publish from different threads.
                std::lock_guard lock(s_owner_mutex);
                std::vector<CustomTagDeclaration> all;
                if (s_third_person)
                {
                    all.push_back(
                        CustomTagDeclaration{
                            .name = constants::THIRD_PERSON_TAG,
                            .cvar = constants::CAMERA_VIEW_CVAR,
                            .min_value = 1,
                            .priority = 0,
                            .source = std::string{BUILT_IN_SOURCE},
                        }
                    );
                }
                if (s_mod_tags.load(std::memory_order_relaxed))
                {
                    all.insert(all.end(), mod_declared.begin(), mod_declared.end());
                }
                const Published *current = s_published.load(std::memory_order_acquire);
                if (current != nullptr && same_declarations(current->declared, all))
                {
                    return;
                }
                std::vector<tag_rules::Declared> ranked_input;
                ranked_input.reserve(all.size());
                for (const CustomTagDeclaration &d : all)
                {
                    ranked_input.push_back(tag_rules::Declared{.name = d.name, .priority = d.priority});
                }
                std::vector<std::size_t> ranked = tag_rules::rank(ranked_input, s_ini_priority, s_ini_disabled);
                if (ranked.size() > tag_rules::MAX_CUSTOM_TAGS)
                {
                    // Rising rank: the lowest-ranked come first and are the ones left out.
                    for (std::size_t i = 0; i < ranked.size() - tag_rules::MAX_CUSTOM_TAGS; ++i)
                    {
                        dropped += (dropped.empty() ? "" : ", ") + all[ranked[i]].name;
                    }
                    ranked.erase(
                        ranked.begin(),
                        ranked.end() - static_cast<std::ptrdiff_t>(tag_rules::MAX_CUSTOM_TAGS)
                    );
                }
                auto next = std::make_unique<Published>();
                next->declared = all;
                next->snapshot.generation = current != nullptr ? current->snapshot.generation + 1 : 1;
                next->snapshot.count = static_cast<std::uint32_t>(ranked.size());
                for (std::size_t slot = 0; slot < ranked.size(); ++slot)
                {
                    const CustomTagDeclaration &d = all[ranked[slot]];
                    next->snapshot.crc[slot] = tag_rules::mannequin_crc(d.name);
                    const std::size_t length = std::min(d.name.size(), next->snapshot.name[slot].size() - 1);
                    std::copy_n(d.name.data(), length, next->snapshot.name[slot].data());
                    const bool camera = d.source == BUILT_IN_SOURCE;
                    next->conditions.push_back(
                        Condition{
                            .camera_view = camera,
                            .cvar = d.cvar,
                            .min_value = d.min_value,
                            .name = d.name,
                            .source = d.source,
                        }
                    );
                    const std::string condition = camera
                                                      ? std::string{"camera view"}
                                                      : d.source + ", " + d.cvar + " >= " + std::to_string(d.min_value);
                    order = d.name + " (" + condition + ")" + (order.empty() ? std::string{} : " > " + order);
                }
                generation = next->snapshot.generation;
                const Published *raw = next.get();
                s_owned.push_back(std::move(next));
                s_published.store(raw, std::memory_order_release);
            }
            if (!dropped.empty())
            {
                DMK::log().warning(
                    "CustomTags: more than {} tags declared, so the lowest-ranked are left out: {}",
                    tag_rules::MAX_CUSTOM_TAGS,
                    dropped
                );
            }
            DMK::log().info(
                "CustomTags: generation {}: {}",
                generation,
                order.empty() ? std::string{"no tags"} : "ranked highest first: " + order
            );
        }

        /// The console help of the variable behind the tag of @p c.
        [[nodiscard]] std::string help_for(const Condition &c)
        {
            return "AnimationFramework custom tag '" + c.name + "' (declared by " + c.source +
                   "): Henry's animations tagged '" + c.name + "' play while this is at least " +
                   std::to_string(c.min_value) + ". Any mod or script may set it.";
        }
    } // namespace

    void configure_custom_tags(bool third_person, bool mod_tags, std::string_view priority, std::string_view disabled)
    {
        s_third_person = third_person;
        s_mod_tags.store(mod_tags, std::memory_order_relaxed);
        s_ini_priority = tag_rules::parse_tag_list(priority);
        s_ini_disabled = tag_rules::parse_tag_list(disabled);
        publish({});
    }

    bool mod_tags_enabled() noexcept
    {
        return s_mod_tags.load(std::memory_order_relaxed);
    }

    void declare_custom_tags(const std::vector<CustomTagDeclaration> &declared)
    {
        s_definition_loaded.store(true, std::memory_order_release);
        publish(declared);
    }

    void poll_custom_tags(bool third_person_view) noexcept
    {
        if (s_definition_loaded.exchange(false, std::memory_order_acq_rel))
        {
            s_definition_epoch.fetch_add(1, std::memory_order_release);
        }
        const Published *published = s_published.load(std::memory_order_acquire);
        if (published == nullptr)
        {
            return;
        }
        const CustomTagSnapshot &snapshot = published->snapshot;
        if (snapshot.generation != s_poll_generation)
        {
            s_poll_generation = snapshot.generation;
            s_poll_mask = 0;
            s_cvars.fill(nullptr);
            s_cvar_failed.fill(false);
        }
        std::uint32_t mask = 0;
        for (std::uint32_t i = 0; i < snapshot.count; ++i)
        {
            const Condition &c = published->conditions[i];
            bool on = false;
            if (c.camera_view)
            {
                on = third_person_view;
            }
            else
            {
                if (s_cvars[i] == nullptr && !s_cvar_failed[i] && console_ready())
                {
                    bool registered = false;
                    try
                    {
                        s_cvars[i] = find_or_register_int_cvar(c.cvar, help_for(c), registered);
                    }
                    catch (...)
                    {
                        s_cvars[i] = nullptr;
                    }
                    s_cvar_failed[i] = s_cvars[i] == nullptr;
                    (void)DMK::log().try_log(
                        s_cvars[i] != nullptr ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                        "CustomTags: {} follows {} ({})",
                        c.name,
                        c.cvar,
                        s_cvars[i] == nullptr ? "the variable could not be found or registered; the tag stays off"
                        : registered          ? "registered, value 0"
                                              : "found"
                    );
                }
                on = s_cvars[i] != nullptr && cvar_int(s_cvars[i]) >= c.min_value;
            }
            if (on)
            {
                mask |= 1u << i;
            }
            if (on != (((s_poll_mask >> i) & 1u) != 0) && !c.camera_view)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info, "CustomTags: {} {}", c.name, on ? "on" : "off");
            }
        }
        s_poll_mask = mask;
        s_active.store((static_cast<std::uint64_t>(snapshot.generation) << 32) | mask, std::memory_order_release);
    }

    const CustomTagSnapshot *custom_tag_snapshot() noexcept
    {
        const Published *published = s_published.load(std::memory_order_acquire);
        return published != nullptr ? &published->snapshot : nullptr;
    }

    std::uint32_t tag_definition_epoch() noexcept
    {
        return s_definition_epoch.load(std::memory_order_acquire);
    }

    std::uint32_t custom_tags_active(const CustomTagSnapshot &snapshot) noexcept
    {
        const std::uint64_t active = s_active.load(std::memory_order_acquire);
        return static_cast<std::uint32_t>(active >> 32) == snapshot.generation ? static_cast<std::uint32_t>(active) : 0;
    }

    void reset_custom_tags() noexcept
    {
        s_active.store(0, std::memory_order_release);
        s_poll_generation = 0;
        s_poll_mask = 0;
        s_cvars.fill(nullptr);
        s_cvar_failed.fill(false);
    }
} // namespace AnimationFramework
