/**
 * @file camera_rules.hpp
 * @brief The camera rules: how the third-person camera treats each object, read from KCD2_TPVCamera_rules.txt.
 *
 * Every list that decides how the camera treats an object (collide, fade, pass, the see-through window, the lens cut,
 * the close-up fade's shader preparation) lives in the rules files, never in code. This module parses those files
 * into an immutable RuleSet and classifies an object by its model path, how it is placed (Kind) and the one engine
 * state it is in (State). It depends on the standard library only: it reads no engine memory and no file, so an
 * offline tool can link it and check it against every model the game ships. camera_rules_files.hpp loads the files
 * and publishes the set in force.
 */
#ifndef TPVCAMERA_CAMERA_RULES_HPP
#define TPVCAMERA_CAMERA_RULES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera::Rules
{

    /** @brief What the camera does with an object: the behaviour words of the rules file. */
    enum class Behaviour : std::uint8_t
    {
        Collide,
        Fade,
        Bush,
        Tree,
        Grass,
        Frame,
        Pass,
        Character,
        // Not scanned at all: no fade, no window, no lens cut, no twins; its physics collides as plain collision.
        Exclude,
    };

    /** @brief The runtime check a rule asks for; its result picks the rule's behaviour or its outcome. */
    enum class Check : std::uint8_t
    {
        None,
        // The fence measurement: a measured wall, a pending one or one that cannot be measured takes the outcome.
        Wall,
        // The two-sided cloth check: a model that draws no two-sided piece takes the outcome.
        Bare,
    };

    /** @brief How an object is placed: its render node type. */
    enum class Kind : std::uint8_t
    {
        Brush,
        Movable,
        Entity,
        Vegetation,
        Merged,
    };

    /** @brief The one engine state an object is in, decided by the caller from engine facts in this order. */
    enum class State : std::uint8_t
    {
        None,
        // A merged-mesh patch (EERTYPE_MERGED_MESH): no model path.
        Merged,
        // A brush or movable brush drawn with one no-draw or collision-proxy material.
        NoDraw,
        // An entity that is an actor, or attached to one.
        Actor,
        // An entity with a rendered animated slot (actors excepted).
        Animated,
        // Vegetation whose model path cannot be read.
        Unnamed,
    };

    /// The twin scope bits twin_scopes() returns: a plant, a bare frame, and the character's own shaders.
    inline constexpr std::uint8_t TWIN_PLANTS = 1;
    inline constexpr std::uint8_t TWIN_FRAMES = 2;
    inline constexpr std::uint8_t TWIN_CHARACTER = 4;

    /// The rule index of a verdict no rule decided (no rule matched and the main file declares no @default).
    inline constexpr std::uint16_t NO_RULE = 0xFFFF;
    /// The longest match a rule can hold, in bytes; a longer one is an error that skips the line.
    inline constexpr std::size_t MAX_MATCH = 511;
    /// The most rules the layers of one set declare together, overridden ones included; later ones are errors.
    inline constexpr std::size_t MAX_RULES = 65534;

    /** @brief What the deciding rule says about an object: six bytes, kept on every scanned node. */
    struct Verdict
    {
        Behaviour behaviour = Behaviour::Exclude;
        Check check = Check::None;
        // The behaviour the check gives when it fires (if-wall= / if-bare=); unused without a check.
        Behaviour outcome = Behaviour::Exclude;
        // The drawn-mesh tests apply wherever the rule ends in collide.
        bool mesh = false;
        // The deciding rule (RuleSet::rule()), or NO_RULE.
        std::uint16_t rule = NO_RULE;
    };

    /** @brief How a rule matches the model path. */
    enum class MatchType : std::uint8_t
    {
        None,   // a state without a path, or @default
        Prefix, // the path starts with the pattern (a folder, or the start of a file name)
        Exact,  // the whole path (a pattern that ends in .cgf, .cga, .chr, .cdf or .skin)
        Text,   // the path contains the pattern (*text*)
    };

    /**
     * @brief One compiled rule, as written in its file.
     * @details Its tier is its form: 1 a state with a model path, 2 a text match, 3 a state without a path, 4 a model
     *          path, 5 @default. RuleSet::classify() states the precedence, which decides @actor and @animated (tier 3)
     *          before text matches, and within a tier puts the later layer before the longer match.
     */
    struct Rule
    {
        std::uint16_t source = 0;  // its layer: index into RuleSet::source()
        std::uint32_t line = 0;    // 1-based line in its source
        std::uint8_t tier = 0;     // 1 state + path, 2 text, 3 state, 4 path, 5 @default
        State state = State::None; // the state of a tier 1 or tier 3 rule
        MatchType match = MatchType::None;
        bool has_only = false;  // the rule carries only=
        std::uint8_t kinds = 0; // the kinds it counts for, as 1 << Kind (every kind without only=)
        Behaviour behaviour = Behaviour::Collide;
        Check check = Check::None;
        Behaviour outcome = Behaviour::Collide;
        bool mesh = false;
        bool twins_character = false; // twins=character
        std::uint8_t twin_bits = 0;   // TWIN_PLANTS / TWIN_FRAMES of its behaviour and outcome
        std::string pattern;          // lower case, '/' slashes, no leading slash (a text match: the inner text)
    };

    /// The layer of a Diagnostic about the set as a whole rather than about one file.
    inline constexpr std::uint16_t NO_LAYER = 0xFFFF;

    /** @brief A message about a rules file, for the log. */
    struct Diagnostic
    {
        /// How a message is logged: an Error names a line or a file the set skipped.
        enum class Level : std::uint8_t
        {
            Warning,
            Error,
        };
        Level level = Level::Warning;
        std::string text;
        // The layer (index into compile()'s layers) of the file it is about, or NO_LAYER. A live reload holds an edit
        // back only for an Error in a file that changed.
        std::uint16_t layer = NO_LAYER;
    };

    /** @brief What a rules file is to the set: the main file (or its built-in copy), a patch, or the player's own. */
    enum class SourceRole : std::uint8_t
    {
        Main,
        Patch, // KCD2_TPVCamera_rules_<Name>.txt
        User,  // KCD2_TPVCamera_rules_User.txt, read last
    };

    /** @brief One rules file: its name for the log, its bytes and its role. Layer 0 is the main file. */
    struct SourceText
    {
        std::string name;
        std::string_view text;
        SourceRole role = SourceRole::Main;
    };

    /** @brief A rules file found beside the ASI: its name as on disk and its role. */
    struct RuleFile
    {
        std::string name;
        SourceRole role = SourceRole::Main;
    };

    /**
     * @brief The end of a file name that marks a reference copy, never a rules file: [Advanced] ExportRules writes the
     *        built-in copy to <mod>_rules.default.txt (Constants::RULES_DEFAULT_EXPORT_SUFFIX).
     */
    inline constexpr std::string_view REFERENCE_SUFFIX = ".default.txt";

    /**
     * @brief The rules files among the file names of one folder, in the order they layer.
     * @details The main file (<mod>_rules.txt) first, then the patches (<mod>_rules_<Name>.txt) by name with the case
     *          of A-Z ignored, then <mod>_rules_User.txt last. Names are compared with the case of A-Z ignored; a name
     *          that ends in REFERENCE_SUFFIX, and any other name, is not a rules file.
     * @param names The file names in the folder (no directories).
     * @param mod_name The mod's base name, "KCD2_TPVCamera".
     */
    [[nodiscard]] std::vector<RuleFile> rule_files(std::span<const std::string> names, std::string_view mod_name);

    /** @brief How many rules of each form one source contributed, for the start-up log line. */
    struct SourceCounts
    {
        std::string name;
        int state = 0;      // a state without a path
        int state_path = 0; // a state with a model path
        int text = 0;
        int path = 0;
        int defaults = 0;
    };

    /**
     * @class RuleSet
     * @brief A compiled, immutable set of rules from one or more layers.
     * @details Built by compile() and shared by every thread that reads it: every member function is const and reads
     *          only, so a reader holding the set needs no lock. classify(), twin_scopes() and the noexcept accessors
     *          allocate nothing; where(), match_text() and rule_text() build strings for the log. Path matches live in
     *          a byte trie over the patterns, so one walk over a name finds every path rule it matches.
     */
    class RuleSet
    {
    public:
        /**
         * @brief The verdict for an object.
         * @details The first of these tiers that applies decides: a rule for @p state with a model path, a rule for
         *          @p state when it is Actor or Animated, a text match, a rule for any other @p state, a model path,
         *          @default. Within one tier a later layer wins whatever the length of its match, then the longest
         *          match, then a rule with only= for fewer kinds, then a later line: a folder line of the User file
         *          beats the main file's longer model lines, but never a rule of an earlier tier (the main file's
         *          @nodraw, @actor, @animated or a text match). A rule whose only= leaves out @p kind does not count.
         *          Without @default, an object no rule matches gets Exclude and NO_RULE.
         * @param name The model path as the call site reads it. ASCII case is ignored. Slashes compare as given.
         * @param kind How the object is placed.
         * @param state Its engine state (the caller reads the facts in the State order).
         * @return The deciding rule's verdict.
         * @note Callable inside a structured-exception frame: it builds nothing that needs unwinding.
         */
        [[nodiscard]] Verdict classify(std::string_view name, Kind kind, State state) const noexcept;

        /**
         * @brief The twin scopes of a model or material path.
         * @details The TWIN_PLANTS and TWIN_FRAMES bits of the deciding rule of every way an object with @p name can be
         *          placed (brush and movable with and without NoDraw, entity with no state, Actor and Animated,
         *          vegetation): a superset of any one object's verdict. A rule without a model path (a state without
         *          one, @default) contributes none. TWIN_CHARACTER is set when any rule with a match that fits @p name
         *          carries twins=character, whichever rule decides the camera.
         * @param name The path, as for classify(). Empty gives 0.
         */
        [[nodiscard]] std::uint8_t twin_scopes(std::string_view name) const noexcept;

        /** @brief Rule @p index, or nullptr for NO_RULE or an index out of range. */
        [[nodiscard]] const Rule *rule(std::uint16_t index) const noexcept;

        /** @brief The name of source @p index (the file it came from). */
        [[nodiscard]] std::string_view source(std::uint16_t index) const noexcept;

        /** @brief Rules per source, in layer order. */
        [[nodiscard]] std::span<const SourceCounts> counts() const noexcept;

        /** @brief The rules in force (overridden and repeated matches dropped). */
        [[nodiscard]] std::size_t size() const noexcept;

        /** @brief The @default rule's index, or NO_RULE when the main file declares none. */
        [[nodiscard]] std::uint16_t catch_all() const noexcept;

        /** @brief The generation compile() stamped on the set; a cache keyed by it revalidates when it changes. */
        [[nodiscard]] std::uint64_t generation() const noexcept;

        /** @brief "<file>:<line>" of rule @p index, or "<no rule>". */
        [[nodiscard]] std::string where(std::uint16_t index) const;

        /** @brief The match of rule @p index as the trace prints it ("objects/...", "*text*", "@actor", ...). */
        [[nodiscard]] std::string match_text(std::uint16_t index) const;

        /** @brief Rule @p index as a rules file writes it: "objects/x.cgf = collide mesh only=brush". */
        [[nodiscard]] std::string rule_text(std::uint16_t index) const;

        /** @brief The role of source @p index (SourceRole::Main for an index out of range). */
        [[nodiscard]] SourceRole source_role(std::uint16_t index) const noexcept;

    private:
        friend std::shared_ptr<const RuleSet> compile(std::span<const SourceText> layers, std::uint64_t generation,
                                                      std::vector<Diagnostic> &diagnostics);

        struct TrieNode
        {
            std::uint32_t first_edge = 0;
            std::uint32_t edge_count = 0;
            std::uint32_t first_rule = 0; // into m_node_rules
            std::uint32_t rule_count = 0;
            std::uint32_t depth = 0;
        };
        struct TrieEdge
        {
            char byte = 0;
            std::uint32_t child = 0;
        };

        /** @brief Which of the first 64 text rules a name contains, once asked (twin_scopes() decides eight times). */
        struct TextHits
        {
            std::uint64_t known = 0;
            std::uint64_t contained = 0;
        };

        /** @brief The trie nodes along @p name that carry rules, shallowest first; returns how many. */
        std::size_t walk(std::string_view name, std::span<std::uint32_t> hits) const noexcept;
        /** @brief The deciding rule for one placement, from the walk's hits. */
        std::uint16_t decide(std::string_view name, Kind kind, State state, std::span<const std::uint32_t> hits,
                             TextHits &text) const noexcept;
        /** @brief Whether text rule @p order (an index into m_text) is contained in @p name. */
        bool text_contained(std::string_view name, std::size_t order, TextHits &text) const noexcept;

        std::vector<Rule> m_rules;
        std::vector<std::string> m_sources;
        std::vector<SourceRole> m_roles;
        std::vector<SourceCounts> m_counts;
        std::vector<TrieNode> m_nodes;
        std::vector<TrieEdge> m_edges;
        std::vector<std::uint16_t> m_node_rules;
        std::vector<std::uint16_t> m_text;                 // text rules, in precedence order
        std::array<std::vector<std::uint16_t>, 6> m_state; // state rules without a path, by State, in order
        std::uint16_t m_default = NO_RULE;
        // The TWIN_PLANTS / TWIN_FRAMES bits any text rule can give, for twin_scopes()'s bound.
        std::uint8_t m_text_twin_bits = 0;
        std::uint64_t m_generation = 0;
    };

    /**
     * @brief Parses and compiles @p layers into one set.
     * @details Layer 0 is the main file; on the very same match (same state and match) a later layer wins for every
     *          kind it counts for, whatever only= each says. A bad line is skipped with an Error naming its file and
     *          line, and the rest applies. A file saved as UTF-16 with a byte order mark is read as the same text;
     *          a file that holds zero bytes otherwise is one Error, and none of its lines apply. A layer past the first
     *          NO_LAYER is not read (one Warning).
     *          A patch whose name ends in .txt.txt, repeated and overridden matches, a @default in a patch (ignored;
     *          the main file and the User file may declare one), an only= no state can use, a mesh that never applies,
     *          a pathless rule that would prepare twins, a check that moves a model between a plant and frame and a
     *          missing @default are Warnings in file order, followed by one Warning per model path a text match takes
     *          over for some kinds (the text match decides those models first), then one Warning per rule of a later
     *          layer that decides, for some kinds, the models of longer matches of earlier layers in its tier, with
     *          how many such lines of each file (a rule as long is an override on the same match, logged above).
     * @param layers The sources, main file first.
     * @param generation The generation stamped on the set.
     * @param diagnostics Receives the messages (existing entries are kept).
     * @return The set (never null).
     * @throws std::bad_alloc When the set cannot allocate.
     */
    [[nodiscard]] std::shared_ptr<const RuleSet> compile(std::span<const SourceText> layers, std::uint64_t generation,
                                                         std::vector<Diagnostic> &diagnostics);

    /** @brief The rules file word of @p behaviour ("collide", "fade", ...). */
    [[nodiscard]] const char *behaviour_name(Behaviour behaviour) noexcept;

    /** @brief The rules file word of @p kind ("brush", "movable", "entity", "vegetation", "merged"). */
    [[nodiscard]] const char *kind_name(Kind kind) noexcept;

    /** @brief The rules file word of @p state ("actor", ...), or "" for State::None. */
    [[nodiscard]] const char *state_name(State state) noexcept;

    /** @brief A verdict as the rules file writes it: "fade if-wall=collide mesh", "collide mesh", "tree". */
    [[nodiscard]] std::string describe(const Verdict &verdict);

    /** @brief How a newer rule set differs from an older one, for the reload log. */
    struct SetChanges
    {
        // "~ <file>:<line>  <rule> (was <rule>)", "+ <file>:<line>  <rule>" and "- <file>:<line>  <rule>", each group
        // in file and line order. A rule of the main file and of the built-in copy compare as the same file's.
        std::vector<std::string> lines;
        // Rules that only moved: the same rule in the same file on another line, or now in the built-in copy.
        std::size_t moved = 0;
    };

    /** @brief Compares the rules in force of @p before and @p after, rule by rule. */
    [[nodiscard]] SetChanges compare(const RuleSet &before, const RuleSet &after);

} // namespace TPVCamera::Rules

#endif // TPVCAMERA_CAMERA_RULES_HPP
