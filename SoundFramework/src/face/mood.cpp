/**
 * @file face/mood.cpp
 * @brief The face rig's pose names, the mood presets and pose-list parsing.
 */

#include "face/mood.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace SoundFramework::face
{
    namespace
    {
        /// The pose bones of the KCD2 face rig, as the skeleton names them after `FA_POSE_`.
        constexpr std::string_view POSES[] = {
            "C_O",
            "C_U",
            "C_bite",
            "C_chewJawDrop",
            "C_chewLipPart",
            "C_chewMouthStretch",
            "C_chinRaise",
            "C_closeMax",
            "C_eyesDown",
            "C_eyesLeft",
            "C_eyesRight",
            "C_eyesUp",
            "C_funnel",
            "C_jawDrop",
            "C_jawThrust",
            "C_jaw_left",
            "C_jaw_right",
            "C_kiss",
            "C_laughDrop",
            "C_laughPart",
            "C_laughStretch",
            "C_lipPart",
            "C_mouthLeft",
            "C_mouthRight",
            "C_mouthStretch",
            "C_placeholder01",
            "C_placeholder02",
            "C_placeholder03",
            "C_placeholder04",
            "C_placeholder05",
            "C_placeholder06",
            "C_placeholder07",
            "C_placeholder08",
            "C_placeholder09",
            "C_placeholder10",
            "C_placeholder11",
            "C_placeholder12",
            "C_press",
            "C_protude",
            "C_screamDrop",
            "C_screamPart",
            "C_screamStretch",
            "C_sticky_center",
            "C_suck",
            "C_tighten",
            "L_browsLowerer",
            "L_browsUp_in",
            "L_browsUp_out",
            "L_cheekRaiser",
            "L_cornerDepressor",
            "L_cornerPull",
            "L_crowsFeet",
            "L_dimpler",
            "L_eyelidClose",
            "L_eyelidCloseIBT",
            "L_eyelidOpenMax",
            "L_lipStretcher",
            "L_lowLipDepressor",
            "L_nasolabialFurrowDeepener",
            "L_neck_stretch",
            "L_noseWrinkler",
            "L_noseWrinkler_up",
            "L_puff",
            "L_sharpPull",
            "L_showTeeth",
            "L_smileOpen",
            "L_squint_low",
            "L_squint_up",
            "L_sticky_in",
            "L_sticky_mid",
            "L_sticky_out",
            "L_towards_negative_low",
            "L_towards_negative_up",
            "L_upLipRaise",
            "R_browsLowerer",
            "R_browsUp_in",
            "R_browsUp_out",
            "R_cheekRaiser",
            "R_cornerDepressor",
            "R_cornerPull",
            "R_crowsFeet",
            "R_dimpler",
            "R_eyelidClose",
            "R_eyelidCloseIBT",
            "R_eyelidOpenMax",
            "R_lipStretcher",
            "R_lowLipDepressor",
            "R_nasolabialFurrowDeepener",
            "R_neck_stretch",
            "R_noseWrinkler",
            "R_noseWrinkler_up",
            "R_puff",
            "R_sharpPull",
            "R_showTeeth",
            "R_smileOpen",
            "R_squint_low",
            "R_squint_up",
            "R_sticky_in",
            "R_sticky_mid",
            "R_sticky_out",
            "R_towards_negative_low",
            "R_towards_negative_up",
            "R_upLipRaise",
        };

        /**
         * @brief Mouth poses that move with the syllables. In the game's own facial clips each one puts a fifth or
         *        more of its motion between 2 and 8 Hz. Corners, smile, dimples, sneer, frown and chin put at most 17%
         *        there and over half below 1 Hz.
         */
        constexpr std::array<std::string_view, 24> ARTICULATION_POSES = {
            "C_O",
            "C_U",
            "C_bite",
            "C_chewJawDrop",
            "C_chewMouthStretch",
            "C_funnel",
            "C_jawDrop",
            "C_mouthStretch",
            "C_placeholder01",
            "C_placeholder02",
            "C_placeholder08",
            "C_press",
            "L_lowLipDepressor",
            "L_sticky_in",
            "L_sticky_mid",
            "L_sticky_out",
            "L_towards_negative_up",
            "L_upLipRaise",
            "R_lowLipDepressor",
            "R_sticky_in",
            "R_sticky_mid",
            "R_sticky_out",
            "R_towards_negative_up",
            "R_upLipRaise",
        };

        /// Upper-face poses; everything else moves the mouth or jaw.
        constexpr std::array<std::string_view, 9> UPPER_STEMS = {
            "browsLowerer",
            "browsUp_in",
            "browsUp_out",
            "cheekRaiser",
            "crowsFeet",
            "eyelid",
            "noseWrinkler",
            "squint",
            "eyes",
        };

        [[nodiscard]] char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
        {
            return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
        }

        [[nodiscard]] std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }
            return text;
        }
    } // namespace

    std::optional<std::string> canonical_pose(std::string_view name)
    {
        name = trim(name);
        constexpr std::string_view prefix = "FA_POSE_";
        if (name.size() > prefix.size() && iequals(name.substr(0, prefix.size()), prefix))
        {
            name.remove_prefix(prefix.size());
        }
        const auto found = std::ranges::find_if(POSES, [&](std::string_view pose) { return iequals(pose, name); });
        if (found == std::ranges::end(POSES))
        {
            return std::nullopt;
        }
        return std::string(*found);
    }

    Region region_of(std::string_view canonical_name) noexcept
    {
        const std::string_view stem = canonical_name.size() > 2 ? canonical_name.substr(2) : canonical_name;
        return std::ranges::any_of(UPPER_STEMS, [&](std::string_view upper) { return stem.starts_with(upper); })
                   ? Region::Upper
                   : Region::Mouth;
    }

    std::span<const std::string_view> articulation_poses() noexcept
    {
        return ARTICULATION_POSES;
    }

    std::optional<std::vector<PoseWeight>> parse_poses(std::string_view text, std::string &error)
    {
        std::vector<PoseWeight> poses;
        while (!text.empty())
        {
            const std::size_t comma = text.find(',');
            const std::string_view item = trim(text.substr(0, comma));
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            if (item.empty())
            {
                continue;
            }
            const std::size_t colon = item.find(':');
            if (colon == std::string_view::npos)
            {
                error = std::format("'{}' is not pose:weight", item);
                return std::nullopt;
            }
            const std::optional<std::string> pose = canonical_pose(item.substr(0, colon));
            if (!pose.has_value())
            {
                error = std::format("'{}' is not a pose of the face rig", trim(item.substr(0, colon)));
                return std::nullopt;
            }
            const std::string_view number = trim(item.substr(colon + 1));
            float weight = 0.0f;
            const auto [end, parse_error] = std::from_chars(number.data(), number.data() + number.size(), weight);
            if (parse_error != std::errc{} || end != number.data() + number.size() || weight < 0.0f ||
                weight > MAX_POSE_WEIGHT)
            {
                error = std::format("'{}' is not a weight between 0 and {}", number, MAX_POSE_WEIGHT);
                return std::nullopt;
            }
            poses.push_back(PoseWeight{*pose, weight});
        }
        return poses;
    }

    std::vector<PoseWeight> merge_poses(std::vector<PoseWeight> base, std::span<const PoseWeight> extra)
    {
        for (const PoseWeight &add : extra)
        {
            const auto same = std::ranges::find_if(base, [&](const PoseWeight &pose) { return pose.pose == add.pose; });
            if (same != base.end())
            {
                same->weight = add.weight;
            }
            else
            {
                base.push_back(add);
            }
        }
        return base;
    }
} // namespace SoundFramework::face
