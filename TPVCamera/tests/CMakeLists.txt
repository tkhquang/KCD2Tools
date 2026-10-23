# Local tests (TPVCAMERA_BUILD_TESTS). Build them with the msvc-tests preset and run them with its ctest preset.
#
# tpvcamera_tests holds the engine-free units: the camera eases, the collision ease, the preset blend filter, the
# right stick's response curve, the rig geometry, the state policy rows, the view-box depth and the fence wall policy,
# each with its rate matrix where it integrates over time.
# The camera rules parity runs the C++ classifier against the Python reference on every scenario; it needs the local
# tools folder, so it is registered only when that folder and a Python 3 interpreter are present.

find_package(GTest 1.17 QUIET)
if(NOT GTest_FOUND OR NOT TARGET GTest::gtest_main)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723 # v1.17.0 release commit, the one DetourModKit pins
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(tpvcamera_tests
  test_blend.cpp
  test_channels.cpp
  test_core_headers.cpp
  test_ease.cpp
  test_fade_sightlines.cpp
  test_fence_coverage.cpp
  test_move_input.cpp
  test_preset_filter.cpp
  test_rig_math.cpp
  test_state_policy.cpp
  test_stick.cpp
  test_view_blend.cpp
  test_view_box.cpp
)
target_include_directories(tpvcamera_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tpvcamera_tests PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(tpvcamera_tests PRIVATE tpv_core GTest::gtest_main)
gtest_discover_tests(tpvcamera_tests DISCOVERY_MODE PRE_TEST)

add_custom_target(tpvcamera_all_tests DEPENDS tpvcamera_tests)

# The collision replay: a recording a DEV build wrote with [Debug] RecordFrames (KCD2_TPVCamera_frames_genNNNN.tpvrec)
# runs again through the core and must reproduce every probe call, output and state bit for bit. A recording is a
# local fixture of the build that wrote it, so the test is registered only when TPVCAMERA_REPLAY_FILE names one.
set(TPVCAMERA_REPLAY_FILE "" CACHE FILEPATH "A collision recording to replay through the core (empty: no replay test).")
add_executable(tpvcamera_replay replay_main.cpp)
target_include_directories(tpvcamera_replay PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(tpvcamera_replay PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(tpvcamera_replay PRIVATE tpv_core)
add_dependencies(tpvcamera_all_tests tpvcamera_replay)
if(TPVCAMERA_REPLAY_FILE)
  add_test(NAME Replay.Collision COMMAND tpvcamera_replay "${TPVCAMERA_REPLAY_FILE}")
endif()

set(TPVCAMERA_RULES_PARITY_DIR "${PROJECT_SOURCE_DIR}/../tools/model-analysis/camera_rules" CACHE PATH
  "Folder holding rules_ref.py and parity/ (local tools); the rules parity test is skipped when it is missing.")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(EXISTS "${TPVCAMERA_RULES_PARITY_DIR}/rules_ref.py" AND EXISTS "${TPVCAMERA_RULES_PARITY_DIR}/parity/rules_dump.cpp"
   AND Python3_Interpreter_FOUND)
  add_executable(tpvcamera_rules_dump ${TPVCAMERA_RULES_PARITY_DIR}/parity/rules_dump.cpp)
  # The local tool includes "camera_rules.hpp" by its bare name.
  target_include_directories(tpvcamera_rules_dump PRIVATE ${PROJECT_SOURCE_DIR}/src/core/rules
                                                          ${TPVCAMERA_RULES_PARITY_DIR}/parity)
  target_link_libraries(tpvcamera_rules_dump PRIVATE tpv_core)
  target_compile_options(tpvcamera_rules_dump PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_dependencies(tpvcamera_all_tests tpvcamera_rules_dump)
  add_test(NAME CameraRules.Parity
    COMMAND ${Python3_EXECUTABLE} -I ${TPVCAMERA_RULES_PARITY_DIR}/parity/scenarios_cpp.py
            ${PROJECT_SOURCE_DIR}/build/template/KCD2_TPVCamera_rules.txt ${TPVCAMERA_RULES_PARITY_DIR}
            $<TARGET_FILE:tpvcamera_rules_dump> ${CMAKE_CURRENT_BINARY_DIR}/rules_parity)
  message(STATUS "KCD2_TPVCamera tests: camera rules parity against ${TPVCAMERA_RULES_PARITY_DIR}")
else()
  message(STATUS "KCD2_TPVCamera tests: camera rules parity skipped (no rules_ref.py, rules_dump.cpp or Python 3)")
endif()
