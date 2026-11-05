/**
 * @file test_token_list.cpp
 * @brief The INI token lists' split, trim and ASCII case-insensitive compare (core/text/token_list.hpp).
 */

#include "core/text/token_list.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace TPVCamera;

    /** @brief The tokens TokenList::for_each visits in @p csv, in order. */
    [[nodiscard]] std::vector<std::string> tokens_of(std::string_view csv)
    {
        std::vector<std::string> tokens;
        TokenList::for_each(csv, [&](std::string_view token) { tokens.emplace_back(token); });
        return tokens;
    }
} // namespace

TEST(TokenList, TrimDropsSpacesTabsAndLineBreaks)
{
    EXPECT_EQ(TokenList::trim(" \tHelmet \r\n"), "Helmet");
    EXPECT_EQ(TokenList::trim("Head Blood"), "Head Blood");
    EXPECT_EQ(TokenList::trim(" \t "), "");
    EXPECT_EQ(TokenList::trim(""), "");
}

TEST(TokenList, EqualsFoldsAsciiCase)
{
    EXPECT_TRUE(TokenList::equals("HeadBlood", "headblood"));
    EXPECT_TRUE(TokenList::equals("HELMET", "helmet"));
    EXPECT_TRUE(TokenList::equals("helmet", "helmet"));
    EXPECT_FALSE(TokenList::equals("helmets", "helmet"));
    EXPECT_FALSE(TokenList::equals("helm", "helmet"));
    EXPECT_FALSE(TokenList::equals("he1met", "helmet"));
}

TEST(TokenList, EqualsLeavesNonAsciiBytesUnfolded)
{
    // 0xC4 is the first byte of a UTF-8 capital A with diaeresis: a locale fold must not turn it into 0xE4.
    EXPECT_FALSE(TokenList::equals("\xC4", "\xE4"));
    EXPECT_TRUE(TokenList::equals("\xC4", "\xC4"));
}

TEST(TokenList, ForEachVisitsTrimmedTokensInOrder)
{
    const std::vector<std::string> expected{
        "Helmet",
        "Wash",
        "Sleepy",
        "HeadBlood",
    };
    EXPECT_EQ(tokens_of("Helmet,Wash, Sleepy ,HeadBlood"), expected);
}

TEST(TokenList, ForEachSkipsEmptyTokens)
{
    const std::vector<std::string> expected{
        "Helmet",
        "Hit",
    };
    EXPECT_EQ(tokens_of(",Helmet,, ,Hit,"), expected);
    EXPECT_TRUE(tokens_of("").empty());
    EXPECT_TRUE(tokens_of(" , ,").empty());
}

TEST(TokenList, ForEachVisitsASingleToken)
{
    EXPECT_EQ(tokens_of(" None "), (std::vector<std::string>{"None"}));
}

TEST(TokenList, CompileTimeEvaluation)
{
    static_assert(TokenList::trim("  x  ") == "x");
    static_assert(TokenList::equals("Wash", "wash"));
    static_assert(!TokenList::equals("Wash", "hit"));
}
