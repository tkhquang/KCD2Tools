/**
 * @file tests/tumbler_sim_tests.cpp
 * @brief Unit tests of the tumbler lock state machine. Plain asserts, no framework: the exit code is the verdict.
 */

#include "sim/tumbler_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
    using namespace OblivionLockpicking::sim;

    int s_failures = 0;

    void check(bool condition, const char *what, int line)
    {
        if (!condition)
        {
            std::printf("FAIL line %d: %s\n", line, what);
            ++s_failures;
        }
    }

#define CHECK(cond) check((cond), #cond, __LINE__)

    LockSetup setup_for(int pins_to_set, float skill = 0.0f, std::uint32_t seed = 7)
    {
        LockSetup setup;
        setup.pins_to_set = pins_to_set;
        setup.skill = skill;
        setup.seed = seed;
        return setup;
    }

    /** @brief Full skill: the lock speed is 0.1, Oblivion's Very Easy lock, so every push is slow with a long pause. */
    LockSetup easy_setup(int pins_to_set)
    {
        return setup_for(pins_to_set, 1.0f);
    }

    int first_unset(const TumblerSim &lock)
    {
        for (int i = 0; i < MAX_TUMBLERS; ++i)
        {
            if (lock.pin(i).phase != PinPhase::Set)
            {
                return i;
            }
        }
        return -1;
    }

    void move_pick_to(TumblerSim &lock, int index)
    {
        (void)lock.step(
            0.0f,
            Inputs{
                .pick_delta = static_cast<float>(index) - lock.pick_position(),
            }
        );
    }

    [[nodiscard]] StepResult push(TumblerSim &lock)
    {
        return lock.step(
            0.0f,
            Inputs{
                .push = true,
            }
        );
    }

    [[nodiscard]] StepResult set(TumblerSim &lock)
    {
        return lock.step(
            0.0f,
            Inputs{
                .set = true,
            }
        );
    }

    /** @brief Advances @p seconds in 5 ms steps (single steps are clamped to 0.25 s by design). */
    void advance(TumblerSim &lock, float seconds)
    {
        for (int steps = static_cast<int>(std::lround(seconds / 0.005f)); steps > 0; --steps)
        {
            (void)lock.step(0.005f, Inputs{});
        }
    }

    /** @brief Steps in 5 ms increments until @p done or @p limit seconds pass. */
    bool run_until(TumblerSim &lock, float limit, const std::function<bool()> &done)
    {
        for (float t = 0.0f; t < limit; t += 0.005f)
        {
            if (done())
            {
                return true;
            }
            (void)lock.step(0.005f, Inputs{});
        }
        return done();
    }

    /** @brief Pushes the first unset tumbler of an easy lock and sets it at the top. */
    void set_one(TumblerSim &lock)
    {
        const int index = first_unset(lock);
        move_pick_to(lock, index);
        CHECK(push(lock).pushed);
        CHECK(run_until(lock, 2.0f, [&] { return lock.pin(index).phase == PinPhase::Top; }));
        CHECK(set(lock).set);
    }

    /** @brief One push from rest, timed phase by phase in 1 ms steps. */
    struct Cycle
    {
        /// The speed class of the roll.
        Speed speed{Speed::Fast};
        /// Seconds of the rise.
        float rise{0.0f};
        /// Seconds of the pause at the top.
        float hang{0.0f};
        /// Seconds of the fall.
        float fall{0.0f};
    };

    Cycle time_cycle(TumblerSim &lock, int index)
    {
        Cycle cycle;
        (void)push(lock);
        cycle.speed = lock.speed(index);
        float *phase_time = &cycle.rise;
        for (int ms = 0; ms < 6000; ++ms)
        {
            *phase_time += 0.001f;
            const StepResult r = lock.step(0.001f, Inputs{});
            if (r.reached_top)
            {
                phase_time = &cycle.hang;
            }
            if (r.dropped)
            {
                phase_time = &cycle.fall;
            }
            if (r.landed)
            {
                break;
            }
        }
        return cycle;
    }

    [[nodiscard]] bool within(float value, float low, float high)
    {
        return value >= low && value <= high;
    }

    void test_reset_presets()
    {
        TumblerSim lock;
        lock.reset(setup_for(3));
        CHECK(lock.remaining() == 3);
        int preset = 0;
        for (int i = 0; i < MAX_TUMBLERS; ++i)
        {
            preset += lock.pin(i).phase == PinPhase::Set ? 1 : 0;
        }
        CHECK(preset == 2);
        CHECK(!lock.solved());
        CHECK(lock.selected() == 2);
    }

    void test_deterministic_rolls()
    {
        std::vector<int> speeds[2];
        for (std::vector<int> &out : speeds)
        {
            TumblerSim lock;
            lock.reset(setup_for(5, 0.0f, 1234));
            move_pick_to(lock, 0);
            for (int k = 0; k < 20; ++k)
            {
                (void)push(lock);
                out.push_back(static_cast<int>(lock.speed(0)));
                CHECK(run_until(lock, 3.0f, [&] { return lock.pin(0).phase == PinPhase::Down; }));
            }
        }
        CHECK(speeds[0] == speeds[1]);
    }

    void test_bands_never_repeat()
    {
        // At skill 0 the lock speed is 1 and each band has its own speed, so the speeds show the bands.
        TumblerSim lock;
        lock.reset(setup_for(5, 0.0f, 99));
        int counts[3] = {};
        int previous = -1;
        int repeats = 0;
        for (int k = 0; k < 300; ++k)
        {
            const int index = k % MAX_TUMBLERS;
            move_pick_to(lock, index);
            (void)push(lock);
            const int speed = static_cast<int>(lock.speed(index));
            ++counts[speed];
            repeats += speed == previous ? 1 : 0;
            previous = speed;
            CHECK(run_until(lock, 3.0f, [&] { return lock.pin(index).phase == PinPhase::Down; }));
        }
        // The band of the last push, on any tumbler, is never rolled again at once. The other two share the rest.
        CHECK(repeats == 0);
        for (const int count : counts)
        {
            CHECK(count > 70 && count < 130);
        }
    }

    void test_original_timings()
    {
        // Oblivion at lock speed 1: rise after the 47 ms push jump, pause at the top, then a slower fall from rest.
        TumblerSim lock;
        lock.reset(setup_for(5, 0.0f, 5));
        move_pick_to(lock, 1);
        int seen[3] = {};
        for (int k = 0; k < 60; ++k)
        {
            const Cycle c = time_cycle(lock, 1);
            ++seen[static_cast<int>(c.speed)];
            CHECK(c.fall > c.rise);
            switch (c.speed)
            {
            case Speed::Slow:
                CHECK(within(c.rise, 0.11f, 0.165f) && within(c.hang, 0.239f, 0.302f) && within(c.fall, 0.96f, 1.10f));
                break;
            case Speed::Fast:
                CHECK(within(c.rise, 0.027f, 0.06f) && within(c.hang, 0.121f, 0.184f) && within(c.fall, 0.67f, 0.79f));
                break;
            case Speed::Fastest:
                CHECK(within(c.rise, 0.002f, 0.017f) && within(c.hang, 0.004f, 0.066f) && within(c.fall, 0.54f, 0.61f));
                break;
            }
        }
        CHECK(seen[0] > 0 && seen[1] > 0 && seen[2] > 0);

        // Full skill, lock speed 0.1 (Oblivion's Very Easy): every push is slow and pauses at least 0.27 s.
        lock.reset(easy_setup(5));
        move_pick_to(lock, 1);
        for (int k = 0; k < 30; ++k)
        {
            const Cycle c = time_cycle(lock, 1);
            CHECK(c.speed == Speed::Slow);
            CHECK(within(c.rise, 0.17f, 0.29f) && within(c.hang, 0.269f, 0.302f) && within(c.fall, 1.12f, 1.42f));
        }

        // TimeScale stretches every phase.
        LockSetup slower = easy_setup(5);
        slower.time_scale = 2.0f;
        lock.reset(slower);
        move_pick_to(lock, 1);
        const Cycle c = time_cycle(lock, 1);
        CHECK(within(c.rise, 0.34f, 0.58f) && within(c.hang, 0.538f, 0.604f) && within(c.fall, 2.24f, 2.84f));
    }

    void test_set_only_at_top()
    {
        TumblerSim lock;
        lock.reset(easy_setup(5));
        move_pick_to(lock, 0);
        (void)push(lock);
        CHECK(run_until(lock, 1.0f, [&] { return lock.pin(0).height > 0.9f; }));
        CHECK(lock.pin(0).phase == PinPhase::Rising);
        // Almost at the top is not at the top.
        const StepResult r = set(lock);
        CHECK(r.wrong_set && !r.set);
        CHECK(lock.pin(0).phase == PinPhase::Down);
        CHECK(push(lock).pushed);
        CHECK(run_until(lock, 1.0f, [&] { return lock.pin(0).phase == PinPhase::Top; }));
        // While it pauses it sits a little above the top, and a set tumbler sits exactly at it.
        CHECK(lock.pin(0).height > 1.03f && lock.pin(0).height <= 1.15f);
        CHECK(set(lock).set);
        CHECK(lock.pin(0).phase == PinPhase::Set);
        CHECK(lock.pin(0).height == 1.0f);
    }

    void test_overshoot_marks_the_drop()
    {
        TumblerSim lock;
        lock.reset(setup_for(5));
        move_pick_to(lock, 1);
        for (int k = 0; k < 30; ++k)
        {
            (void)push(lock);
            CHECK(run_until(lock, 1.0f, [&] { return lock.pin(1).phase == PinPhase::Top; }));
            const float paused = lock.pin(1).height;
            // The paused tumbler sits above the top, up to the cap.
            CHECK(paused > 1.03f && paused <= 1.15f);
            CHECK(run_until(lock, 1.0f, [&] { return lock.pin(1).phase == PinPhase::Falling; }));
            // The pause ends with a visible drop back to the top.
            CHECK(lock.pin(1).height <= 1.0f && paused - lock.pin(1).height > 0.03f);
            // A set now is wrong, and the result says where the tumbler was.
            const StepResult r = set(lock);
            CHECK(r.wrong_set && r.wrong_phase == PinPhase::Falling && r.wrong_height > 0.9f);
            CHECK(run_until(lock, 0.1f, [&] { return lock.pin(1).phase == PinPhase::Down; }));
        }
    }

    void test_repush_keeps_speed()
    {
        TumblerSim lock;
        lock.reset(setup_for(5));
        move_pick_to(lock, 3);
        (void)push(lock);
        const Speed speed = lock.speed(3);
        CHECK(run_until(lock, 1.0f, [&] { return lock.pin(3).phase == PinPhase::Falling; }));
        advance(lock, 0.2f);
        const float before = lock.pin(3).height;
        CHECK(before > 0.0f && before < 1.0f);
        CHECK(push(lock).pushed);
        CHECK(lock.pin(3).phase == PinPhase::Rising);
        CHECK(std::fabs(lock.pin(3).height - before) < 0.01f);
        CHECK(lock.speed(3) == speed);
    }

    void test_held_push_bounces()
    {
        TumblerSim lock;
        lock.reset(easy_setup(5));
        move_pick_to(lock, 2);
        int pushes = 0;
        int tops = 0;
        float lowest_after_rise = 1.0f;
        bool risen = false;
        for (int step = 0; step < 1000; ++step)
        {
            const StepResult r = lock.step(
                0.005f,
                Inputs{
                    .push_held = true,
                }
            );
            pushes += r.pushed ? 1 : 0;
            tops += r.reached_top ? 1 : 0;
            risen = risen || lock.pin(2).phase == PinPhase::Top;
            if (risen)
            {
                lowest_after_rise = std::min(lowest_after_rise, lock.pin(2).height);
            }
            CHECK(!r.landed);
        }
        // Pushed from rest, then caught on every fall: it bounces at its speed and never lands.
        CHECK(pushes >= 3 && tops >= 3);
        CHECK(lock.speed(2) == Speed::Slow);
        CHECK(lowest_after_rise > 0.5f && lowest_after_rise < 0.8f);

        // Let go: it falls all the way.
        CHECK(run_until(lock, 2.0f, [&] { return lock.pin(2).phase == PinPhase::Down; }));
    }

    void test_held_push_sets_at_top()
    {
        TumblerSim lock;
        lock.reset(easy_setup(5));
        move_pick_to(lock, 0);
        bool set = false;
        for (int step = 0; step < 600 && !set; ++step)
        {
            const bool at_top = lock.pin(0).phase == PinPhase::Top;
            const StepResult r = lock.step(
                0.005f,
                Inputs{
                    .push_held = true,
                    .set = at_top,
                }
            );
            CHECK(!r.wrong_set);
            set = r.set;
        }
        CHECK(set && lock.pin(0).phase == PinPhase::Set);
    }

    void test_keep_on_fail()
    {
        for (const int keep : {KEEP_ALL_ON_FAIL, 0, 1})
        {
            TumblerSim lock;
            LockSetup setup = easy_setup(5);
            setup.keep_on_fail = keep;
            lock.reset(setup);
            const int first = first_unset(lock);
            set_one(lock);
            set_one(lock);
            CHECK(lock.remaining() == 3);

            move_pick_to(lock, first_unset(lock));
            (void)push(lock);
            advance(lock, 0.05f);
            CHECK(set(lock).wrong_set);
            const int expected = keep == KEEP_ALL_ON_FAIL ? 3 : 5 - keep;
            CHECK(lock.remaining() == expected);
            if (keep == 1)
            {
                // The earliest set tumbler is the one that stays.
                CHECK(lock.pin(first).phase == PinPhase::Set);
            }
        }
    }

    void test_set_grace()
    {
        for (const float late : {0.03f, 0.1f})
        {
            TumblerSim lock;
            LockSetup setup = easy_setup(5);
            setup.set_grace = 0.06f;
            lock.reset(setup);
            move_pick_to(lock, 2);
            (void)push(lock);
            CHECK(run_until(lock, 2.0f, [&] { return lock.pin(2).phase == PinPhase::Falling; }));
            (void)lock.step(late, Inputs{});
            // Just after the pause the set still counts. Later it is a wrong set.
            const StepResult r = set(lock);
            CHECK(late < 0.06f ? r.set && lock.pin(2).phase == PinPhase::Set : r.wrong_set && !r.set);
            CHECK(late < 0.06f ? r.set_late >= late - 0.01f && r.set_late <= late + 0.01f : r.set_late < 0.0f);
        }

        // Without grace, Oblivion's strict rule: the first moment of the fall is already too late.
        TumblerSim strict;
        strict.reset(easy_setup(5));
        move_pick_to(strict, 2);
        (void)push(strict);
        CHECK(run_until(strict, 2.0f, [&] { return strict.pin(2).phase == PinPhase::Falling; }));
        CHECK(set(strict).wrong_set);
    }

    void test_set_on_rest()
    {
        // Oblivion and the Remaster, the default: a set on a tumbler at rest only clicks.
        TumblerSim lock;
        lock.reset(setup_for(5));
        StepResult r = set(lock);
        CHECK(!r.wrong_set && !r.pushed && !r.set);
        CHECK(lock.pin(lock.selected()).phase == PinPhase::Down);

        // The lenient option pushes it.
        TumblerSim forgiving;
        LockSetup setup = setup_for(5);
        setup.set_pushes_resting = true;
        forgiving.reset(setup);
        r = set(forgiving);
        CHECK(r.pushed && !r.wrong_set);
    }

    void test_one_at_a_time()
    {
        for (const bool strict : {false, true})
        {
            TumblerSim lock;
            LockSetup setup = easy_setup(5);
            setup.one_at_a_time = strict;
            lock.reset(setup);
            move_pick_to(lock, 2);
            (void)push(lock);
            (void)lock.step(
                0.01f,
                Inputs{
                    .pick_delta = 1.0f,
                }
            );
            // While the tumbler is in the air, the strict pick stays under it. The free one moves on.
            CHECK(lock.selected() == (strict ? 2 : 3));
            CHECK(run_until(lock, 3.0f, [&] { return lock.pin(2).phase == PinPhase::Down; }));
            move_pick_to(lock, 4);
            CHECK(lock.selected() == 4);
        }
    }

    void test_solved_latch_and_large_step()
    {
        TumblerSim lock;
        lock.reset(easy_setup(2));
        set_one(lock);
        const int index = first_unset(lock);
        move_pick_to(lock, index);
        (void)push(lock);
        // One step far beyond a whole cycle is clamped to 0.25 s: the tumbler has not gone past its pause.
        (void)lock.step(5.0f, Inputs{});
        CHECK(lock.pin(index).phase == PinPhase::Rising || lock.pin(index).phase == PinPhase::Top);
        CHECK(run_until(lock, 1.0f, [&] { return lock.pin(index).phase == PinPhase::Top; }));
        const StepResult r = set(lock);
        CHECK(r.set && r.solved && lock.solved());
        CHECK(!push(lock).pushed);
    }
} // namespace

int main()
{
    test_reset_presets();
    test_deterministic_rolls();
    test_bands_never_repeat();
    test_original_timings();
    test_set_only_at_top();
    test_overshoot_marks_the_drop();
    test_repush_keeps_speed();
    test_held_push_bounces();
    test_held_push_sets_at_top();
    test_keep_on_fail();
    test_set_grace();
    test_set_on_rest();
    test_one_at_a_time();
    test_solved_latch_and_large_step();
    if (s_failures != 0)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all tumbler sim tests passed\n");
    return 0;
}
