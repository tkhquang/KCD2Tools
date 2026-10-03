/**
 * @file highlight/groups.hpp
 * @brief Highlight groups: the user's [Highlight.<Name>] sections, their hotkeys and their activation state.
 *
 * A group names the targets it shows: kinds the scans classify (Corpses, Items, Animals, Doors, ...) and patterns
 * matched against any object (Class:, Name:, Model:, with * and ? wildcards), optionally with a colour each. It
 * filters them by tags or patterns an object must have (Only) or must not have (Except), and says how it is drawn
 * (Style), switched on (Key, Mode, Duration, FadeOut), how far it reaches (Radius) and when it hides (HideIn). Every
 * one of those falls back to [Settings] or [Render] when a group leaves it out.
 *
 * DetourModKit binds fixed INI keys only, so the sections are read here with a small parser, on load and after
 * every INI auto-reload, and each group's hotkey is registered with input::register_combo. A group keeps its toggle
 * and hold state across a reload when its name is unchanged. Several groups may share a key: one press switches all
 * of them.
 *
 * Threading: the group set is replaced on the init or INI-watcher thread and read on the main thread through
 * current_groups(); the hotkey callbacks (input poll thread) only flip the atomics of GroupActivation.
 */
#ifndef HENRYSENSES_GROUPS_HPP
#define HENRYSENSES_GROUPS_HPP

#include <DetourModKit/error.hpp>
#include <DetourModKit/input.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    /**
     * @enum Target
     * @brief One kind of thing a group can show.
     */
    enum class Target : std::uint8_t
    {
        Corpses,
        Carcasses,
        Items,
        Containers,
        Herbs,
        Npcs,
        /// Every living animal: horses, dogs and critters included.
        Animals,
        /// Living horses alone (a colour of their own next to Animals).
        Horses,
        /// Living dogs alone (a colour of their own next to Animals).
        Dogs,
        /// Flock animals alone: chickens, rats, mice, birds, fish (the engine's boids).
        Critters,
        Doors,
        Workstations,
        Beds,
        Seats,
        UseSpots,
        Objects,
        /// Pattern: an entity class.
        Class,
        /// Pattern: an entity name.
        Name,
        /// Pattern: the model file of an entity or of a static world object.
        Model,
    };

    /** @brief Bit of @p target in a target mask. */
    [[nodiscard]] constexpr std::uint32_t target_bit(Target target) noexcept
    {
        return 1u << static_cast<std::uint32_t>(target);
    }

    /** @brief True for the pattern targets (Class, Name, Model). */
    [[nodiscard]] constexpr bool is_pattern_target(Target target) noexcept
    {
        return target == Target::Class || target == Target::Name || target == Target::Model;
    }

    /** @brief The INI spelling of a target. */
    [[nodiscard]] const char *target_name(Target target) noexcept;

    /**
     * @brief Case-insensitive wildcard match: '*' matches any run of characters, '?' one character.
     * @param pattern The pattern, lower-case.
     * @param text The text (any case).
     */
    [[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view text) noexcept;

    /**
     * @enum GroupMode
     * @brief How a group's key switches it on.
     */
    enum class GroupMode : std::uint32_t
    {
        /// The key shows the group for Duration seconds; a press again restarts it.
        Pulse = 0,
        /// The key switches the group on and off.
        Toggle = 1,
        /// The group shows while the key is held.
        Hold = 2,
        /// Always shown; the key is ignored.
        Always = 3,
    };

    /** @brief The INI spelling of a mode. */
    [[nodiscard]] const char *group_mode_name(GroupMode mode) noexcept;

    /**
     * @brief Parses a mode name (case-insensitive).
     * @return The mode, or std::nullopt for an unknown name.
     */
    [[nodiscard]] std::optional<GroupMode> parse_group_mode(std::string_view text) noexcept;

    /**
     * @enum GroupStyle
     * @brief How a group draws its objects.
     */
    enum class GroupStyle : std::uint8_t
    {
        /// An outline on the object's own model.
        Outline = 0,
        /// A flat tint over the object's own model.
        Fill = 1,
        /// An outline with its interior tinted.
        OutlineFill = 2,
        /// Corner brackets around the object.
        Box = 3,
    };

    /** @brief The INI spelling of a style. */
    [[nodiscard]] const char *group_style_name(GroupStyle style) noexcept;

    /**
     * @struct GroupTarget
     * @brief One entry of a group's Targets list (or a pattern of its Only / Except lists).
     */
    struct GroupTarget
    {
        Target target{Target::Items};
        /// The lower-case pattern of a Class, Name or Model entry.
        std::string pattern{};
        /// Its own colour (0xRRGGBBAA), or 0 for the group's Color.
        std::uint32_t color{0};
    };

    /**
     * @class ObjectFacts
     * @brief What a group matches an object against: its kind, its tags, and (read only when a pattern asks) its
     *        entity class, entity name and model file.
     * @note Main thread; the strings are read on first use and kept for the object's lifetime.
     */
    class ObjectFacts
    {
    public:
        /**
         * @brief Caches the facts used to match an object against highlight groups.
         * @param target The kind the scans gave it, or std::nullopt for an object found by a pattern only.
         * @param flags Its LootFlag bits.
         * @param entity Its CEntity, or 0 for a static world object.
         * @param brush Its CBrush, or 0 for an entity.
         */
        ObjectFacts(std::optional<Target> target, std::uint32_t flags, std::uintptr_t entity, std::uintptr_t brush)
            : m_target(target), m_flags(flags), m_entity(entity), m_brush(brush)
        {
        }

        /** @brief Returns the category assigned by the scans, if any. */
        [[nodiscard]] std::optional<Target> target() const noexcept { return m_target; }

        /** @brief Returns the cached loot flags. */
        [[nodiscard]] std::uint32_t flags() const noexcept { return m_flags; }

        /** @brief The entity class (empty for a static world object). */
        [[nodiscard]] const std::string &klass();

        /** @brief The entity name (empty for a static world object). */
        [[nodiscard]] const std::string &name();

        /** @brief The model file. */
        [[nodiscard]] const std::string &model();

        /** @brief True when @p entry (a pattern) matches this object. */
        [[nodiscard]] bool matches(const GroupTarget &entry);

    private:
        std::optional<Target> m_target;
        std::uint32_t m_flags{0};
        std::uintptr_t m_entity{0};
        std::uintptr_t m_brush{0};
        std::optional<std::string> m_klass{};
        std::optional<std::string> m_name{};
        std::optional<std::string> m_model{};
    };

    /**
     * @struct GroupActivation
     * @brief A group's switch state, flipped by its hotkey on the input poll thread.
     */
    struct GroupActivation
    {
        /// Steady-clock milliseconds of the last pulse press, 0 for none.
        std::atomic<std::int64_t> pulse_started_ms{0};
        /**
         * @brief Steady-clock milliseconds when a toggle was switched off or a hold released (the fade-out start), 0
         * for
         * none.
         */
        std::atomic<std::int64_t> off_started_ms{0};
        /// Toggle mode: switched on.
        std::atomic<bool> toggled{false};
        /// Hold mode: the key is down.
        std::atomic<bool> held{false};
    };

    /**
     * @struct HighlightGroup
     * @brief One [Highlight.<Name>] section, with the [Settings] and [Render] fallbacks applied.
     */
    struct HighlightGroup
    {
        std::string name{};
        bool enabled{true};
        std::vector<GroupTarget> targets{};
        /// OR of target_bit() over targets.
        std::uint32_t target_mask{0};
        /// Colour of the targets listed without one.
        std::uint32_t color{0xFFFFFFFFu};
        /// LootFlag bits an object must all have.
        std::uint32_t only{0};
        /// LootFlag bits an object must have none of.
        std::uint32_t except{0};
        /// Patterns an object must all match.
        std::vector<GroupTarget> only_patterns{};
        /// Patterns an object must match none of.
        std::vector<GroupTarget> except_patterns{};
        GroupStyle style{GroupStyle::Outline};
        /// GameState bits that hide the group.
        std::uint32_t gate_mask{0};
        /// The key as written (own or [Settings] Key), for log lines.
        std::string key_text{};
        DMK::input::KeyComboList keys{};
        /**
         * @brief Key.Consume (own or [Settings] Key.Consume): hide the key's trigger from the game while the combo is
         * down. Only a digital controller button or the mouse wheel can be hidden; keyboard keys, the modifiers and the
         * analog triggers always reach the game.
         */
        bool consume{false};
        GroupMode mode{GroupMode::Pulse};
        /// Seconds a pulse stays at full strength.
        float duration{5.0f};
        /// Seconds the group takes to fade out at the end of a pulse, a toggle off or a hold release.
        float fade_out{1.0f};
        /// Reach in metres.
        float radius{20.0f};
        /// While the group shows, everything but the highlighted objects is darkened ([Render] FocusDarken).
        bool focus{false};
        /// Particle effect shown on the group's containers while it shows (intern_effect_name id; 0 = none).
        std::uint16_t effect{0};
        /// Uniform scale of that effect.
        float effect_scale{1.0f};
        std::shared_ptr<GroupActivation> activation{};

        /**
         * @brief The colour this group gives @p object, or std::nullopt when it does not claim it (not a listed
         *        target, or a tag or pattern filter excludes it). Distance and activation are the caller's.
         */
        [[nodiscard]] std::optional<std::uint32_t> claims(ObjectFacts &object) const;

        /** @brief True when @p flags pass the tag part of Only and Except. */
        [[nodiscard]] bool accepts_flags(std::uint32_t flags) const noexcept
        {
            return (flags & only) == only && (flags & except) == 0;
        }

        /** @brief True when @p kind (or its parent kind) is one of the targets. */
        [[nodiscard]] bool wants(Target kind) const noexcept;
    };

    /**
     * @struct GroupPatterns
     * @brief The pattern targets of every enabled group, for the scans that find pattern-only objects.
     */
    struct GroupPatterns
    {
        std::vector<std::string> classes{};
        std::vector<std::string> names{};
        std::vector<std::string> models{};
        /// One line of all of them, to detect a change.
        std::string signature{};
        /**
         * @brief The widest Radius of a group with a Name or Model pattern (the entity walk reads names and models only
         * that far).
         */
        float reach{0.0f};
    };

    /**
     * @struct GroupSet
     * @brief Every group of the INI, in file order (the last matching group wins an object).
     */
    struct GroupSet
    {
        std::vector<HighlightGroup> groups{};
        GroupPatterns patterns{};
    };

    /**
     * @brief Reads the groups from the INI and registers their hotkeys.
     * @note Init thread, after DMK::config::load() (the fallbacks must be read).
     */
    [[nodiscard]] DMK::Result<void> initialize_groups();

    /**
     * @brief Hands over the settings init starts the input engine with, so an INI reload that registers hotkeys can
     *        start it too: Input::start() with no binding staged is a no-op, so an engine started while every group
     *        was Always (no hotkey) would never poll the keys a later reload registers.
     * @note Init thread, before it starts the input engine.
     */
    void set_group_input_settings(const DMK::input::Input::Settings &settings);

    /**
     * @brief Reads the groups again after an INI change and re-registers their hotkeys when they changed.
     * @note The INI-watcher thread (the auto-reload callback), after the bound settings were applied.
     */
    void reload_groups();

    /** @brief Releases every group hotkey. */
    void shutdown_groups() noexcept;

    /**
     * @brief The current group set.
     * @return A snapshot that stays valid while held; never null after initialize_groups().
     */
    [[nodiscard]] std::shared_ptr<const GroupSet> current_groups() noexcept;

    /** @brief Logs every group with its state (the state report). */
    void log_groups();

    /**
     * @brief Returns the id of a particle effect name ("<library>.<effect>"), adding it on first sight.
     * @details Ids are never reused or dropped, so a HighlightRequest can carry one by value across an INI reload.
     * @param name The full effect name.
     * @return The id; 0 for an empty name (no effect).
     * @note Any thread.
     */
    [[nodiscard]] std::uint16_t intern_effect_name(std::string_view name);

    /**
     * @brief Returns the effect name behind an id from intern_effect_name().
     * @return The name; empty for 0 or an unknown id.
     * @note Any thread.
     */
    [[nodiscard]] std::string effect_name(std::uint16_t id);

} // namespace HenrySenses

#endif // HENRYSENSES_GROUPS_HPP
