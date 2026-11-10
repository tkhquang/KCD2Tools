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

project(KCD2_AnimationFramework VERSION ${_ver_major}.${_ver_minor}.${_ver_patch} LANGUAGES CXX)

# Re-run configure when the version header changes so PROJECT_VERSION stays current.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_version_header}")

# Build options.
# ANIMFW_DEV_BUILD OFF (default): the production ASI and the KCSE plugin DLL, each built from the logic.
# ON: a resident loader ASI plus a logic DLL it reloads as staged generations (see src/dev/).
option(ANIMFW_DEV_BUILD "Build the two-DLL hot-reload dev configuration (resident loader ASI + logic DLL)" OFF)
# Empty by default: nothing is deployed unless a game directory is configured explicitly.
set(ANIMFW_GAME_DIR "" CACHE PATH
  "Directory to deploy the built mod into (the game's ASI loader directory). Empty disables deployment.")
option(ANIMFW_BUILD_TESTS "Build the unit tests of the game-independent code" ON)

# Standard and compiler options.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project ships as an MSVC build only.
if(NOT MSVC)
  message(FATAL_ERROR "KCD2_AnimationFramework requires the MSVC toolchain (Visual Studio 2022+).")
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

# The game-independent file code (clip index headers, clips, skeletons, zip archives, database deltas, the clip
# compiler and the mod maker): plain C++ with no game, Windows or DetourModKit dependency.
add_library(animfw_core STATIC
  src/core/anim_catalog.cpp
  src/core/bridge_protocol.cpp
  src/core/caf_reader.cpp
  src/core/clip_check.cpp
  src/core/clip_compiler.cpp
  src/core/chr_merge.cpp
  src/core/clip_cache.cpp
  src/core/clip_header.cpp
  src/core/game_paks.cpp
  src/core/legacy_merge.cpp
  src/core/mod_maker.cpp
  src/core/mod_tools.cpp
  src/core/scene_move.cpp
  src/core/tag_rules.cpp
  src/core/xml_elements.cpp
  src/core/zip_reader.cpp
  src/core/zip_writer.cpp
)
configure_mod_compile(animfw_core)
target_include_directories(animfw_core PUBLIC src)

# The clip check and the mod maker without the game running. animfw.exe (command line and MCP server) and
# animfw-studio.exe (AnimFW Studio, a window with a mannequin preview) read the stock game from its paks and make mods
# in a source folder. Both share animfw_offline.
option(ANIMFW_BUILD_STUDIO "Build animfw-studio.exe (fetches raylib, Dear ImGui and rlImGui)" ON)
include(FetchContent)

# Retargeting other skeletons' animations (BVH, FBX) onto Henry's: tools only, never in the game. ufbx reads FBX.
enable_language(C)
# ufbx ships no CMakeLists.txt, so FetchContent_MakeAvailable only populates it and the library is declared here.
FetchContent_Declare(ufbx GIT_REPOSITORY https://github.com/ufbx/ufbx.git GIT_TAG v0.23.1 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ufbx)
add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})

add_library(animfw_retarget STATIC src/retarget/mocap.cpp src/retarget/retarget.cpp)
configure_mod_compile(animfw_retarget)
target_link_libraries(animfw_retarget PUBLIC animfw_core PRIVATE ufbx)

add_library(animfw_offline STATIC
  src/tool/offline_tools.cpp
  src/tool/cutscene_clone.cpp
  src/tool/cutscene_files.cpp
  src/tool/sandbox_layer.cpp
  src/tool/sandbox_setup.cpp
)
configure_mod_compile(animfw_offline)
target_link_libraries(animfw_offline PUBLIC animfw_core animfw_retarget)

# The Sandbox editor's "AnimFW export" button (sandbox/), embedded in animfw.exe: `animfw sandbox-setup` writes it into
# the editor's folder, so no release archive carries the editor script itself.
set(ANIMFW_SANDBOX_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/sandbox/animfw_export.py ${CMAKE_CURRENT_SOURCE_DIR}/sandbox/AnimationFramework.xml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ANIMFW_SANDBOX_SOURCES})
# MSVC refuses a string literal over 16380 bytes (C2026), so each file embeds as adjacent raw literals of at most 8000
# bytes. Carriage returns go first, so no piece ends between the two bytes of a line break.
function(animfw_embed_text out_var file delimiter)
  file(READ ${file} text)
  string(REPLACE "\r" "" text "${text}")
  string(LENGTH "${text}" length)
  set(pieces "")
  set(at 0)
  while(at LESS length)
    string(SUBSTRING "${text}" ${at} 8000 piece)
    string(APPEND pieces "R\"${delimiter}(${piece})${delimiter}\"\n")
    math(EXPR at "${at} + 8000")
  endwhile()
  set(${out_var} "${pieces}" PARENT_SCOPE)
endfunction()
animfw_embed_text(ANIMFW_SANDBOX_EXPORT_PY ${CMAKE_CURRENT_SOURCE_DIR}/sandbox/animfw_export.py afw_py)
animfw_embed_text(ANIMFW_SANDBOX_TOOLBAR_XML ${CMAKE_CURRENT_SOURCE_DIR}/sandbox/AnimationFramework.xml afw_xml)
configure_file(src/tool/sandbox_files.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/sandbox_files.hpp @ONLY)
target_include_directories(animfw_offline PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# utf8.manifest makes UTF-8 the process code page, so narrow argv, std::filesystem::path conversions and the MCP
# server's JSON all carry any path as UTF-8. Under a legacy code page, path::string() throws on a character outside it.
add_executable(animfw_tool src/tool/animfw_tool.cpp src/tool/mcp_server.cpp src/tool/utf8.manifest)
configure_mod_compile(animfw_tool)
target_link_libraries(animfw_tool PRIVATE animfw_offline nlohmann_json::nlohmann_json)
set_target_properties(animfw_tool PROPERTIES OUTPUT_NAME animfw)

# The Blender add-on, installable as is (Edit > Preferences > Add-ons > Install from Disk): its Python with the tool
# inside, so it needs no setup. Build with --target animfw_blender_addon. The zip lands in the build folder.
file(GLOB ANIMFW_BLENDER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/blender/animfw_bridge/*.py)
set(ANIMFW_BLENDER_STAGE ${CMAKE_CURRENT_BINARY_DIR}/blender_addon)
set(ANIMFW_BLENDER_ZIP ${CMAKE_CURRENT_BINARY_DIR}/KCD2_AnimationFramework_Blender.zip)
add_custom_command(OUTPUT ${ANIMFW_BLENDER_ZIP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${ANIMFW_BLENDER_STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ANIMFW_BLENDER_STAGE}/animfw_bridge
  COMMAND ${CMAKE_COMMAND} -E copy ${ANIMFW_BLENDER_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
          $<TARGET_FILE:animfw_tool> ${ANIMFW_BLENDER_STAGE}/animfw_bridge
  COMMAND ${CMAKE_COMMAND} -E chdir ${ANIMFW_BLENDER_STAGE} ${CMAKE_COMMAND} -E tar cf ${ANIMFW_BLENDER_ZIP} --format=zip animfw_bridge
  DEPENDS ${ANIMFW_BLENDER_SOURCES} animfw_tool
  COMMENT "Packing the Blender add-on with the tool"
  VERBATIM)
add_custom_target(animfw_blender_addon DEPENDS ${ANIMFW_BLENDER_ZIP})

if(ANIMFW_BUILD_STUDIO)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  # rlImGui's Raylib_6_0 tag is the pairing it is tested with: raylib 6.0 and Dear ImGui 1.92.7.
  FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 6.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.7 GIT_SHALLOW TRUE)
  FetchContent_Declare(rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG 3bc5731c4216bb8caa67fbea24aa85ce80d57ccb
  )
  FetchContent_MakeAvailable(raylib imgui rlimgui)

  add_library(animfw_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp
  )
  target_include_directories(animfw_imgui PUBLIC ${imgui_SOURCE_DIR} ${rlimgui_SOURCE_DIR})
  target_compile_features(animfw_imgui PUBLIC cxx_std_20)
  target_link_libraries(animfw_imgui PUBLIC raylib)

  add_executable(animfw_studio WIN32
    src/tool/studio/studio_main.cpp
    src/tool/studio/mannequin.cpp
    src/tool/studio/file_dialogs.cpp
    src/tool/utf8.manifest
  )
  configure_mod_compile(animfw_studio)
  target_link_libraries(animfw_studio PRIVATE animfw_offline animfw_imgui ole32 shell32)
  # A window with a plain main(): no console behind it.
  target_link_options(animfw_studio PRIVATE /ENTRY:mainCRTStartup)
  set_target_properties(animfw_studio PROPERTIES OUTPUT_NAME animfw-studio)
endif()

# The tested code touches no game memory, so the tests run anywhere.
if(ANIMFW_BUILD_TESTS)
  enable_testing()
  add_executable(animfw_core_tests tests/core_tests.cpp)
  configure_mod_compile(animfw_core_tests)
  # The generated sandbox_files.hpp and its sources, which the test compares.
  target_include_directories(animfw_core_tests PRIVATE tests ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_compile_definitions(animfw_core_tests PRIVATE ANIMFW_SANDBOX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sandbox")
  target_link_libraries(animfw_core_tests PRIVATE animfw_core animfw_retarget)
  add_test(NAME core COMMAND animfw_core_tests)
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

# Source files shared by the production builds and the dev logic DLL.
set(COMMON_SOURCES
  src/anchors.cpp
  src/anim_catalog_console.cpp
  src/clip_check_console.cpp
  src/mod_maker_console.cpp
  src/animation_framework.cpp
  src/camera_view.cpp
  src/config.cpp
  src/console_vars.cpp
  src/custom_tags.cpp
  src/cutscenes.cpp
  src/forced_options.cpp
  src/live_bridge.cpp
  src/mod_settings.cpp
  src/npc_actions.cpp
  src/version.cpp
  src/hooks/anim_patch.cpp
  src/hooks/frame_tick.cpp
  src/hooks/quest_console.cpp
  src/hooks/view_variants.cpp
)

# Applies the settings shared by the production builds and the dev logic DLL: include paths, link libraries,
# and the Release speed/opt flags. /Gy + /Gw with /OPT:REF + /OPT:ICF let the linker drop and fold unused
# functions and data.
function(configure_logic_target target)
  configure_mod_compile(${target})
  target_include_directories(${target} PRIVATE src)
  target_link_libraries(${target} PRIVATE animfw_core DetourModKit user32 kernel32 advapi32)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2 /Gy /Gw>)
  target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endfunction()

if(NOT ANIMFW_DEV_BUILD)
  # Production build: one set of objects linked twice, as the ASI for an ASI loader and as a DLL for
  # Kingdom Come Script Extender (KCSE), which adds only its two plugin exports (src/kcse_plugin.cpp).
  add_library(animfw_logic OBJECT ${COMMON_SOURCES} src/dllmain.cpp)
  configure_logic_target(animfw_logic)

  add_library(${PROJECT_NAME}-ASI SHARED)
  configure_logic_target(${PROJECT_NAME}-ASI)
  target_link_libraries(${PROJECT_NAME}-ASI PRIVATE animfw_logic)
  set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
    OUTPUT_NAME "KCD2_AnimationFramework-temp-asi"
    SUFFIX ".asi"
    PREFIX ""
  )

  add_library(${PROJECT_NAME}-KCSE SHARED src/kcse_plugin.cpp)
  configure_logic_target(${PROJECT_NAME}-KCSE)
  target_link_libraries(${PROJECT_NAME}-KCSE PRIVATE animfw_logic)
  set_target_properties(${PROJECT_NAME}-KCSE PROPERTIES
    OUTPUT_NAME "KCD2_AnimationFramework-temp-kcse"
    SUFFIX ".dll"
    PREFIX ""
  )

  # Link-time optimization on Release (whole-program inlining across the mod).
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_supported LANGUAGES CXX OUTPUT _ipo_error)
  if(_ipo_supported)
    set_target_properties(animfw_logic ${PROJECT_NAME}-ASI ${PROJECT_NAME}-KCSE PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    )
    message(STATUS "LTO enabled for the ${PROJECT_NAME} Release builds")
  else()
    message(STATUS "LTO not supported for ${PROJECT_NAME}: ${_ipo_error}")
  endif()

  # Normalize the artifacts to their canonical names at the build-tree root so they are found at the same path under
  # both the Ninja and Visual Studio generators (the release workflow copies build/KCD2_AnimationFramework.asi and
  # build/KCSE/KCD2_AnimationFramework.dll).
  add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-ASI>
      ${CMAKE_CURRENT_BINARY_DIR}/KCD2_AnimationFramework.asi
    COMMENT "Renaming ASI output to final name"
  )
  add_custom_command(TARGET ${PROJECT_NAME}-KCSE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/KCSE
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-KCSE>
      ${CMAKE_CURRENT_BINARY_DIR}/KCSE/KCD2_AnimationFramework.dll
    COMMENT "Renaming KCSE plugin output to final name"
  )

  # Optional deploy of the production ASI to a configured game directory.
  if(ANIMFW_GAME_DIR)
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/KCD2_AnimationFramework.asi
        "${ANIMFW_GAME_DIR}/KCD2_AnimationFramework.asi"
      COMMENT "Deploying ASI to ${ANIMFW_GAME_DIR}"
    )
  endif()

  message(STATUS "KCD2_AnimationFramework (${PROJECT_VERSION}) - production build")
  message(STATUS "    - ${PROJECT_NAME}-ASI  -> KCD2_AnimationFramework.asi (ASI loader)")
  message(STATUS "    - ${PROJECT_NAME}-KCSE -> KCSE/KCD2_AnimationFramework.dll (KCSE plugin)")
else()
  # Dev build: a resident loader ASI plus a logic DLL it hot-reloads as unique-name staged generations
  # (src/dev/mod_loader.cpp, src/dev/mod_logic.cpp, src/dev/abi/reload.h).

  # Resident staged-generation loader ASI. Its file names derive from ANIMFW_LOADER_MOD_NAME, and it stays inert
  # in every process except ANIMFW_LOADER_PROCESS_NAME.
  add_library(${PROJECT_NAME}-loader SHARED src/dev/mod_loader.cpp)
  configure_mod_compile(${PROJECT_NAME}-loader)
  target_compile_definitions(${PROJECT_NAME}-loader PRIVATE
    ANIMFW_LOADER_MOD_NAME="KCD2_AnimationFramework"
    ANIMFW_LOADER_PROCESS_NAME="KingdomCome.exe"
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
  get_target_property(_af_loader_links ${PROJECT_NAME}-loader LINK_LIBRARIES)
  if(NOT _af_loader_links MATCHES "DetourModKit::WheelHost")
    message(FATAL_ERROR "${PROJECT_NAME}-loader must link DetourModKit::WheelHost.")
  endif()
  if(_af_loader_links MATCHES "DetourModKit::DetourModKit" OR _af_loader_links MATCHES "(^|;)DetourModKit(;|$)")
    message(FATAL_ERROR "${PROJECT_NAME}-loader must not link the full DetourModKit archive.")
  endif()

  set_target_properties(${PROJECT_NAME}-loader PROPERTIES
    OUTPUT_NAME "KCD2_AnimationFramework"
    SUFFIX ".asi"
    PREFIX ""
  )

  # Logic DLL: every line of mod code, replaced wholesale on each hot reload.
  add_library(${PROJECT_NAME}-logic SHARED ${COMMON_SOURCES} src/dev/mod_logic.cpp)
  configure_logic_target(${PROJECT_NAME}-logic)
  target_include_directories(${PROJECT_NAME}-logic PRIVATE src/dev)
  set_target_properties(${PROJECT_NAME}-logic PROPERTIES
    OUTPUT_NAME "KCD2_AnimationFramework.logic"
    PREFIX ""
  )
  # /Zi emits a PDB, and /DEBUG /INCREMENTAL:NO keep each rebuild a clean, reloadable DLL.
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

  # The logic DLL and its PDB go into staging/. The loader promotes them on startup and on each Numpad 3 reload,
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
    message(STATUS "Logic DLL staged; press Numpad 3 in game to load it")
else()
    message(STATUS "Logic DLL staging failed (the loader was promoting the previous build); build again")
endif()
]=])

  if(ANIMFW_GAME_DIR AND EXISTS "${ANIMFW_GAME_DIR}")
    add_custom_command(TARGET ${PROJECT_NAME}-loader POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DSRC_ASI=$<TARGET_FILE:${PROJECT_NAME}-loader>
        -DDST_ASI=${ANIMFW_GAME_DIR}/KCD2_AnimationFramework.asi
        -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake"
      COMMENT "Deploying loader ASI to ${ANIMFW_GAME_DIR} (skips if locked)"
    )
    add_custom_command(TARGET ${PROJECT_NAME}-logic POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DSRC_DLL=$<TARGET_FILE:${PROJECT_NAME}-logic>
        -DSTAGING_DLL=${ANIMFW_GAME_DIR}/staging/KCD2_AnimationFramework.logic.dll
        -DSRC_PDB=$<TARGET_PDB_FILE:${PROJECT_NAME}-logic>
        -DSTAGING_PDB=${ANIMFW_GAME_DIR}/staging/KCD2_AnimationFramework.logic.pdb
        -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
      COMMENT "Staging logic DLL+PDB in ${ANIMFW_GAME_DIR}/staging (loader promotes on startup / Numpad 3)"
    )
  elseif(ANIMFW_GAME_DIR)
    message(STATUS "ANIMFW_GAME_DIR does not exist, dev deploy disabled: ${ANIMFW_GAME_DIR}")
  else()
    message(STATUS "ANIMFW_GAME_DIR not set: dev build will not auto-deploy. "
                   "Set -DANIMFW_GAME_DIR=<game ASI directory> to enable hot-reload deploy.")
  endif()

  message(STATUS "KCD2_AnimationFramework (${PROJECT_VERSION}) - DEV (two-DLL hot-reload) build")
  message(STATUS "    - ${PROJECT_NAME}-loader -> KCD2_AnimationFramework.asi (resident loader, DetourModKit::WheelHost only)")
  message(STATUS "    - ${PROJECT_NAME}-logic  -> KCD2_AnimationFramework.logic.dll (mod logic, staged as .genNNNN copies)")
endif()
