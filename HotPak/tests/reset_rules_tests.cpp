/**
 * @file tests/reset_rules_tests.cpp
 * @brief Unit tests of which clothing proxy the reset window resets.
 */

#include "check.hpp"
#include "reset_rules.hpp"

#include <optional>

namespace
{
    using namespace HotPak;

    constexpr int STATE_INIT = 0;
    constexpr int STATE_LOADING = 1;
    constexpr int STATE_STREAMING = constants::PROXY_STATE_STREAMING;
    constexpr int STATE_WAITING = constants::PROXY_STATE_WAITING_FOR_SWAP;
    constexpr int STATE_ERROR = constants::PROXY_STATE_ERROR;

    [[nodiscard]] bool resets_for(const ProxyFacts &facts, ResetReason expected)
    {
        const std::optional<ResetReason> reason = reset_reason(facts);
        return reason.has_value() && *reason == expected;
    }

    /** @brief True when the window leaves a proxy with @p facts alone. */
    [[nodiscard]] bool left_alone(const ProxyFacts &facts)
    {
        return !reset_reason(facts).has_value();
    }

    void replaced_objects_reset()
    {
        CHECK(resets_for(
            ProxyFacts{.shipped = true, .replaced_skin = true, .state = STATE_STREAMING},
            ResetReason::ReplacedSkin
        ));
        CHECK(resets_for(
            ProxyFacts{.shipped = true, .retired_material = true, .state = STATE_STREAMING},
            ResetReason::RetiredMaterial
        ));
        CHECK(resets_for(ProxyFacts{.shipped = true, .state = STATE_ERROR}, ResetReason::Error));
        // A proxy outside the pak's files is never examined further.
        CHECK(left_alone(ProxyFacts{.replaced_skin = true, .state = STATE_STREAMING}));
    }

    void fresh_character_after_a_load_is_left_alone()
    {
        // A character a save load created: new manager, nothing replaced, HotPak reset nothing on it.
        for (const int state : {STATE_INIT, STATE_LOADING, STATE_STREAMING, STATE_WAITING})
        {
            CHECK(left_alone(ProxyFacts{.shipped = true, .restream = true, .state = state}));
        }
        // Only a proxy of it that lands in Error is retried.
        CHECK(resets_for(ProxyFacts{.shipped = true, .restream = true, .state = STATE_ERROR}, ResetReason::Error));
    }

    void table_rows_restream_existing_characters()
    {
        CHECK(resets_for(
            ProxyFacts{.shipped = true, .restream = true, .state = STATE_STREAMING, .manager_early = true},
            ResetReason::TableRows
        ));
        CHECK(
            left_alone(ProxyFacts{.shipped = true, .restream = false, .state = STATE_STREAMING, .manager_early = true})
        );
    }

    void stuck_swap_waits_for_a_reset_of_the_character()
    {
        CHECK(left_alone(ProxyFacts{.shipped = true, .state = STATE_WAITING}));
        CHECK(
            left_alone(ProxyFacts{.shipped = true, .state = STATE_WAITING, .since_manager_reset_ms = STUCK_SWAP_MS - 1})
        );
        CHECK(resets_for(
            ProxyFacts{.shipped = true, .state = STATE_WAITING, .since_manager_reset_ms = STUCK_SWAP_MS},
            ResetReason::StuckSwap
        ));
        CHECK(
            left_alone(ProxyFacts{.shipped = true, .state = STATE_STREAMING, .since_manager_reset_ms = STUCK_SWAP_MS})
        );
    }
} // namespace

int main()
{
    replaced_objects_reset();
    fresh_character_after_a_load_is_left_alone();
    table_rows_restream_existing_characters();
    stuck_swap_waits_for_a_reset_of_the_character();
    return HotPak::tests::finish("reset_rules");
}
