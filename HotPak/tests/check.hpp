/**
 * @file tests/check.hpp
 * @brief The assertion the plain unit tests share. No framework: the exit code is the verdict.
 */
#ifndef HOTPAK_TESTS_CHECK_HPP
#define HOTPAK_TESTS_CHECK_HPP

#include <cstdio>

namespace HotPak::tests
{
    /// Failed checks so far in this process.
    inline int s_failures = 0;

    /** @brief Records a failed check with its source text and line. */
    inline void check(bool condition, const char *what, int line)
    {
        if (!condition)
        {
            std::printf("FAIL line %d: %s\n", line, what);
            ++s_failures;
        }
    }

    /** @brief Prints the verdict and returns the process exit code. */
    inline int finish(const char *suite)
    {
        std::printf("%s: %s (%d failure(s))\n", suite, s_failures == 0 ? "PASS" : "FAIL", s_failures);
        return s_failures == 0 ? 0 : 1;
    }
} // namespace HotPak::tests

#define CHECK(cond) ::HotPak::tests::check((cond), #cond, __LINE__)

#endif // HOTPAK_TESTS_CHECK_HPP
