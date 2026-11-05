/**
 * @file core/layout.cpp
 * @brief The clock's parts and their attachments (see layout.hpp).
 */

#include "core/layout.hpp"
#include "strings.hpp"

namespace HUDClock::core
{
    namespace
    {
        constexpr std::array<std::string_view, PART_COUNT> PART_NAMES = {"Dial", "Line1", "Line2", "Line3", "Line4"};
        constexpr std::string_view SCREEN_NAME = "Screen";

        [[nodiscard]] constexpr bool is_part(int part) noexcept
        {
            return part >= 0 && part < static_cast<int>(PART_COUNT);
        }
    } // namespace

    std::string_view part_name(int part) noexcept
    {
        return is_part(part) ? PART_NAMES[static_cast<std::size_t>(part)] : SCREEN_NAME;
    }

    std::optional<int> parse_target(std::string_view text) noexcept
    {
        text = strings::trim(text);
        if (strings::equals_ignoring_case(text, SCREEN_NAME))
        {
            return SCREEN_TARGET;
        }
        for (std::size_t i = 0; i < PART_COUNT; ++i)
        {
            if (strings::equals_ignoring_case(text, PART_NAMES[i]))
            {
                return static_cast<int>(i);
            }
        }
        return std::nullopt;
    }

    ResolvedTargets resolve_targets(const std::array<int, PART_COUNT> &targets) noexcept
    {
        ResolvedTargets resolved{.targets = targets};
        for (int &target : resolved.targets)
        {
            if (!is_part(target))
            {
                target = SCREEN_TARGET;
            }
        }
        // Follow each part's chain. A chain that comes back to a part already on it is a loop, and the part whose
        // attachment closes it goes to the screen. Each pass breaks at most one loop, so the parts bound the passes.
        for (std::size_t start = 0; start < PART_COUNT; ++start)
        {
            std::array<bool, PART_COUNT> on_chain{};
            int part = static_cast<int>(start);
            while (part != SCREEN_TARGET)
            {
                on_chain[static_cast<std::size_t>(part)] = true;
                const int next = resolved.targets[static_cast<std::size_t>(part)];
                if (next != SCREEN_TARGET && on_chain[static_cast<std::size_t>(next)])
                {
                    resolved.targets[static_cast<std::size_t>(part)] = SCREEN_TARGET;
                    resolved.broken |= 1u << part;
                    break;
                }
                part = next;
            }
        }
        return resolved;
    }
} // namespace HUDClock::core
