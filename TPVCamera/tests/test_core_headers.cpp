/**
 * @file test_core_headers.cpp
 * @brief Every core header compiles on its own in the test build, which has no DetourModKit or engine include path:
 *        a core header that reaches for one fails here.
 */

#include "core/blend/blend.hpp"
#include "core/blend/ease.hpp"
#include "core/collision/arm.hpp"
#include "core/collision/fan.hpp"
#include "core/collision/probe_types.hpp"
#include "core/collision/world_probe.hpp"
#include "core/input/stick.hpp"
#include "core/math/vec.hpp"
#include "core/occlusion/fence_coverage.hpp"
#include "core/policy/state_policy.hpp"
#include "core/rig/rig_math.hpp"
#include "core/rules/camera_rules.hpp"
#include "core/view/view_box.hpp"

#include <gtest/gtest.h>

namespace
{
    TEST(CoreHeaders, CompileWithoutTheEngine)
    {
        SUCCEED();
    }
} // namespace
