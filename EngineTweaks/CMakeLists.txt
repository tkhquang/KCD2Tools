cmake_minimum_required(VERSION 3.28)

# Derive the project version from src/version.hpp, the single source of truth, so the configure-time banner
# cannot drift from the header after a version bump.
set(_version_header "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp")
file(STRINGS "${_version_header}" _ver_major REGEX "^#define +VERSION_MAJOR +[0-9]+")
file(STRINGS "${_version_header}" _ver_minor REGEX "^#define +VERSION_MINOR +[0-9]+")
file(STRINGS "${_version_header}" _ver_patch REGEX "^#define +VERSION_PATCH +[0-9]+")
string(REGEX REPLACE ".*VERSION_MAJOR +([0-9]+).*" "\\1" _ver_major "${_ver_major}")
string(REGEX REPLACE ".*VERSION_MINOR +([0-9]+).*" "\\1" _ver_minor "${_ver_minor}")
string(REGEX REPLACE ".*VERSION_PATCH +([0-9]+).*" "\\1" _ver_patch "${_ver_patch}")

project(KCD2_EngineTweaks VERSION ${_ver_major}.${_ver_minor}.${_ver_patch} LANGUAGES CXX)

# Re-run configure when the version header changes so PROJECT_VERSION stays current.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_version_header}")

# Build options.
# ENGINETWEAKS_DEV_BUILD OFF (default): the production ASI and the KCSE plugin DLL, each built from the logic.
# ON: a resident loader ASI plus a logic DLL it reloads as staged generations (see src/dev/).
option(ENGINETWEAKS_DEV_BUILD "Build the two-DLL hot-reload dev configuration (resident loader ASI + logic DLL)" OFF)
option(ENGINETWEAKS_BUILD_TESTS "Build the unit tests" ON)
# Empty by default: nothing is deployed unless a game directory is configured explicitly.
set(ENGINETWEAKS_GAME_DIR "" CACHE PATH
  "Directory to deploy the built mod into (the game's ASI loader directory). Empty disables deployment.")

# Standard and compiler options.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project ships as an MSVC build only.
if(NOT MSVC)
  message(FATAL_ERROR "KCD2_EngineTweaks requires the MSVC toolchain (Visual Studio 2022+).")
endif()
# The mod's own warning level and Windows definitions, applied per target so they never reach the DetourModKit,
# SafetyHook and Zydis targets, which set their own.
function(configure_mod_compile target)
  target_compile_options(${target} PRIVATE /W4)
  target_compile_definitions(${target} PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
endfunction()

# The endpoint index is plain C++ with no game, Windows or DetourModKit dependency, so its tests run without the game.
add_library(engine_tweaks_core STATIC src/core/endpoint_index.cpp)
configure_mod_compile(engine_tweaks_core)
target_include_directories(engine_tweaks_core PUBLIC src)

if(ENGINETWEAKS_BUILD_TESTS)
  enable_testing()
  add_executable(engine_tweaks_tests tests/core_tests.cpp)
  configure_mod_compile(engine_tweaks_tests)
  target_link_libraries(engine_tweaks_tests PRIVATE engine_tweaks_core)
  add_test(NAME core COMMAND engine_tweaks_tests)
endif()

# DetourModKit dependency (git submodule).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/DetourModKit/CMakeLists.txt")
  message(FATAL_ERROR "DetourModKit not found at 'external/DetourModKit'. "
    "Initialize submodules: 'git submodule update --init --recursive'")
endif()

# Guard against double-definition if a parent project already pulled DetourModKit in.
if(NOT TARGET DetourModKit)
  add_subdirectory(external/DetourModKit)
endif()
if(NOT TARGET DetourModKit)
  message(FATAL_ERROR "DetourModKit target not created by subdirectory")
endif()

# Source files shared by the production ASI and the dev logic DLL.
set(COMMON_SOURCES
  src/anchors.cpp
  src/config.cpp
  src/engine_tweaks.cpp
  src/version.cpp
  src/fixes/diag.cpp
  src/fixes/inventory_refresh.cpp
  src/fixes/link_index.cpp
  src/fixes/menu_view_distance.cpp
  src/fixes/perk_copy.cpp
  src/fixes/sleep_prediction.cpp
)

# Applies the settings shared by the production ASI and the dev logic DLL: include paths, link libraries,
# and the Release speed/opt flags. /Gy + /Gw with /OPT:REF + /OPT:ICF let the linker drop and fold unused
# functions and data.
function(configure_logic_target target)
  configure_mod_compile(${target})
  target_include_directories(${target} PRIVATE src)
  target_link_libraries(${target} PRIVATE
    engine_tweaks_core DetourModKit
    user32 kernel32)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2 /Gy /Gw>)
  target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endfunction()

if(NOT ENGINETWEAKS_DEV_BUILD)
  # Production build: one set of objects linked twice, as the ASI for an ASI loader and as a DLL for
  # Kingdom Come Script Extender (KCSE), which adds only its two plugin exports (src/kcse_plugin.cpp).
  add_library(engine_tweaks_logic OBJECT ${COMMON_SOURCES} src/dllmain.cpp)
  configure_logic_target(engine_tweaks_logic)

  add_library(${PROJECT_NAME}-ASI SHARED)
  configure_logic_target(${PROJECT_NAME}-ASI)
  target_link_libraries(${PROJECT_NAME}-ASI PRIVATE engine_tweaks_logic)
  set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
    OUTPUT_NAME "KCD2_EngineTweaks-temp-asi"
    SUFFIX ".asi"
    PREFIX ""
  )

  add_library(${PROJECT_NAME}-KCSE SHARED src/kcse_plugin.cpp)
  configure_logic_target(${PROJECT_NAME}-KCSE)
  target_link_libraries(${PROJECT_NAME}-KCSE PRIVATE engine_tweaks_logic)
  set_target_properties(${PROJECT_NAME}-KCSE PROPERTIES
    OUTPUT_NAME "KCD2_EngineTweaks-temp-kcse"
    SUFFIX ".dll"
    PREFIX ""
  )

  # Link-time optimization on Release (whole-program inlining across the mod).
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_supported LANGUAGES CXX OUTPUT _ipo_error)
  if(_ipo_supported)
    set_target_properties(engine_tweaks_logic ${PROJECT_NAME}-ASI ${PROJECT_NAME}-KCSE PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    )
    message(STATUS "LTO enabled for the ${PROJECT_NAME} Release builds")
  else()
    message(STATUS "LTO not supported for ${PROJECT_NAME}: ${_ipo_error}")
  endif()

  # Normalize the artifacts to their canonical names at the build-tree root so they are found at the same path under
  # both the Ninja and Visual Studio generators (the release workflow copies build/KCD2_EngineTweaks.asi and
  # build/KCSE/KCD2_EngineTweaks.dll).
  add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-ASI>
      ${CMAKE_CURRENT_BINARY_DIR}/KCD2_EngineTweaks.asi
    COMMENT "Renaming ASI output to final name"
  )
  add_custom_command(TARGET ${PROJECT_NAME}-KCSE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/KCSE
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-KCSE>
      ${CMAKE_CURRENT_BINARY_DIR}/KCSE/KCD2_EngineTweaks.dll
    COMMENT "Renaming KCSE plugin output to final name"
  )

  # Optional deploy of the production ASI to a configured game directory.
  if(ENGINETWEAKS_GAME_DIR)
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/KCD2_EngineTweaks.asi
        "${ENGINETWEAKS_GAME_DIR}/KCD2_EngineTweaks.asi"
      COMMENT "Deploying ASI to ${ENGINETWEAKS_GAME_DIR}"
    )
  endif()

  message(STATUS "KCD2_EngineTweaks (${PROJECT_VERSION}) - production build")
  message(STATUS "    - ${PROJECT_NAME}-ASI  -> KCD2_EngineTweaks.asi (ASI loader)")
  message(STATUS "    - ${PROJECT_NAME}-KCSE -> KCSE/KCD2_EngineTweaks.dll (KCSE plugin)")
else()
  # Dev build: a resident loader ASI plus a logic DLL it hot-reloads as unique-name staged generations
  # (src/dev/mod_loader.cpp, src/dev/mod_logic.cpp, src/dev/abi/reload.h).

  # Resident staged-generation loader ASI. Its file names derive from ENGINETWEAKS_LOADER_MOD_NAME, and it stays inert
  # in every process except ENGINETWEAKS_LOADER_PROCESS_NAME.
  add_library(${PROJECT_NAME}-loader SHARED src/dev/mod_loader.cpp)
  configure_mod_compile(${PROJECT_NAME}-loader)
  target_compile_definitions(${PROJECT_NAME}-loader PRIVATE
    ENGINETWEAKS_LOADER_MOD_NAME="KCD2_EngineTweaks"
    ENGINETWEAKS_LOADER_PROCESS_NAME="KingdomCome.exe"
  )
  target_include_directories(${PROJECT_NAME}-loader PRIVATE src/dev)

  # The loader hosts wheel capture for the process, so a user-bound mouse-wheel combo books its permanent module
  # keepalive HERE instead of in the logic DLL, which is what lets a generation unmap. It links the resident host
  # ONLY, and that target already carries user32 on its INTERFACE.
  target_link_libraries(${PROJECT_NAME}-loader PRIVATE
    DetourModKit::WheelHost
  )

  # Guard the property above. A link against the full archive gives the loader its own DetourModKit instance, which
  # is the arrangement DMK's unload proofs exclude.
  get_target_property(_loader_links ${PROJECT_NAME}-loader LINK_LIBRARIES)
  if(NOT _loader_links MATCHES "DetourModKit::WheelHost")
    message(FATAL_ERROR "${PROJECT_NAME}-loader must link DetourModKit::WheelHost.")
  endif()
  if(_loader_links MATCHES "DetourModKit::DetourModKit" OR _loader_links MATCHES "(^|;)DetourModKit(;|$)")
    message(FATAL_ERROR "${PROJECT_NAME}-loader must not link the full DetourModKit archive.")
  endif()

  set_target_properties(${PROJECT_NAME}-loader PROPERTIES
    OUTPUT_NAME "KCD2_EngineTweaks"
    SUFFIX ".asi"
    PREFIX ""
  )

  # Logic DLL: every line of mod code, replaced wholesale on each hot reload.
  add_library(${PROJECT_NAME}-logic SHARED ${COMMON_SOURCES} src/dev/mod_logic.cpp)
  configure_logic_target(${PROJECT_NAME}-logic)
  target_include_directories(${PROJECT_NAME}-logic PRIVATE src/dev)
  set_target_properties(${PROJECT_NAME}-logic PROPERTIES
    OUTPUT_NAME "KCD2_EngineTweaks.logic"
    PREFIX ""
  )
  # /Zi emits a PDB. /DEBUG /INCREMENTAL:NO keep each rebuild a clean, reloadable DLL.
  target_compile_options(${PROJECT_NAME}-logic PRIVATE /Zi)
  target_link_options(${PROJECT_NAME}-logic PRIVATE /DEBUG /INCREMENTAL:NO)

  # The deploy scripts are generated at configure time, so no checked-in script is needed. The loader ASI is locked
  # while the game runs, so its copy gives up quietly then: the loader changes rarely and lands with the game closed.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake" [=[
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_ASI}" "${DST_ASI}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    message(STATUS "Loader ASI deployed")
else()
    message(STATUS "Loader ASI deploy skipped (file locked)")
endif()
]=])

  # The logic DLL and its PDB go into staging/. The loader promotes them on startup and on each Numpad 4 reload,
  # then maps a unique-name copy (<mod>.genNNNN.logic.dll), so no build ever writes a file a mapped image locks.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake" [=[
get_filename_component(staging_dir "${STAGING_DLL}" DIRECTORY)
file(MAKE_DIRECTORY "${staging_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${STAGING_DLL}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    if(EXISTS "${SRC_PDB}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_PDB}" "${STAGING_PDB}")
    endif()
    message(STATUS "Logic DLL staged. Press Numpad 4 in game to load it.")
else()
    message(STATUS "Logic DLL staging failed, because the loader was promoting the previous build. Build again.")
endif()
]=])

  if(ENGINETWEAKS_GAME_DIR AND EXISTS "${ENGINETWEAKS_GAME_DIR}")
    add_custom_command(TARGET ${PROJECT_NAME}-loader POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DSRC_ASI=$<TARGET_FILE:${PROJECT_NAME}-loader>
        -DDST_ASI=${ENGINETWEAKS_GAME_DIR}/KCD2_EngineTweaks.asi
        -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake"
      COMMENT "Deploying loader ASI to ${ENGINETWEAKS_GAME_DIR} (skips if locked)"
    )
    add_custom_command(TARGET ${PROJECT_NAME}-logic POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DSRC_DLL=$<TARGET_FILE:${PROJECT_NAME}-logic>
        -DSTAGING_DLL=${ENGINETWEAKS_GAME_DIR}/staging/KCD2_EngineTweaks.logic.dll
        -DSRC_PDB=$<TARGET_PDB_FILE:${PROJECT_NAME}-logic>
        -DSTAGING_PDB=${ENGINETWEAKS_GAME_DIR}/staging/KCD2_EngineTweaks.logic.pdb
        -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
      COMMENT "Staging logic DLL+PDB in ${ENGINETWEAKS_GAME_DIR}/staging (loader promotes on startup / Numpad 4)"
    )
  elseif(ENGINETWEAKS_GAME_DIR)
    message(STATUS "ENGINETWEAKS_GAME_DIR does not exist, dev deploy disabled: ${ENGINETWEAKS_GAME_DIR}")
  else()
    message(STATUS "ENGINETWEAKS_GAME_DIR not set: dev build will not auto-deploy. "
                   "Set -DENGINETWEAKS_GAME_DIR=<game ASI directory> to enable hot-reload deploy.")
  endif()

  message(STATUS "KCD2_EngineTweaks (${PROJECT_VERSION}) - DEV (two-DLL hot-reload) build")
  message(STATUS "    - ${PROJECT_NAME}-loader -> KCD2_EngineTweaks.asi (resident loader, DetourModKit::WheelHost only)")
  message(STATUS "    - ${PROJECT_NAME}-logic  -> KCD2_EngineTweaks.logic.dll (mod logic, staged as .genNNNN copies)")
endif()
