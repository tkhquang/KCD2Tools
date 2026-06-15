cmake_minimum_required(VERSION 3.28)

# Derive the project version from src/version.hpp, the single source of truth
# (scripts/version_updater.py bumps only that header). PROJECT_VERSION currently feeds only the
# configure-time status banner; deriving it here keeps the two from drifting after a version bump.
set(_version_header "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp")
file(STRINGS "${_version_header}" _ver_major REGEX "^#define +VERSION_MAJOR +[0-9]+")
file(STRINGS "${_version_header}" _ver_minor REGEX "^#define +VERSION_MINOR +[0-9]+")
file(STRINGS "${_version_header}" _ver_patch REGEX "^#define +VERSION_PATCH +[0-9]+")
string(REGEX REPLACE ".*VERSION_MAJOR +([0-9]+).*" "\\1" _ver_major "${_ver_major}")
string(REGEX REPLACE ".*VERSION_MINOR +([0-9]+).*" "\\1" _ver_minor "${_ver_minor}")
string(REGEX REPLACE ".*VERSION_PATCH +([0-9]+).*" "\\1" _ver_patch "${_ver_patch}")

project(KCD2_TPVCamera VERSION ${_ver_major}.${_ver_minor}.${_ver_patch} LANGUAGES CXX)

# Re-run configure when the version header changes so PROJECT_VERSION stays current.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_version_header}")

# --- Build Options ---
# TPVCAMERA_DEV_BUILD OFF (default): a single production ASI built from the logic
# plus dllmain.cpp. ON: a thin loader ASI plus a logic DLL, enabling DetourModKit
# hot-reload for fast iteration (the loader reloads the logic in place; see src/dev/).
option(TPVCAMERA_DEV_BUILD "Build the two-DLL hot-reload dev configuration (thin loader ASI + logic DLL)" OFF)
set(TPVCAMERA_GAME_DIR "" CACHE PATH
  "Directory to deploy the built mod into (the game's mod-loader/plugins directory). Used by the dev build for hot-reload.")

# --- Standard and Compiler Options ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project ships as an MSVC build only.
if(NOT MSVC)
  message(FATAL_ERROR "KCD2_TPVCamera requires the MSVC toolchain (Visual Studio 2022+).")
endif()

add_compile_options(/W4)

# --- Windows Definitions ---
if(WIN32)
  add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _XM_SSE_INTRINSICS_ # Required for DirectXMath inline functions
  )
endif()

# --- DetourModKit Dependency (git submodule) ---
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

# --- Dear ImGui (compiled from source, linked directly -- no ReShade) ---
# The self-hosted overlay (src/overlay) renders ImGui through a private WARP
# D3D11 device and composites it over the game window with GDI; it links ImGui
# directly and calls ImGui:: / ImGui_ImplWin32_* / ImGui_ImplDX11_* with no
# function-table indirection. The docking tag is used so the Win32 + DX11
# backends match the v1.92.5 API the overlay was written against.
include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.5-docking
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# --- nlohmann/json (header-only) for the preset store ---
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT TARGET imgui_lib)
  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  # ImGui is third-party: do not subject it to /W4.
  target_compile_options(imgui_lib PRIVATE /W0)
endif()

# --- Source Files ---
set(COMMON_SOURCES
  src/aob_resolver.cpp
  src/config.cpp
  src/game_interface.cpp
  src/game_state.cpp
  src/global_state.cpp
  src/offset_heal.cpp
  src/physics_raycast.cpp
  src/tpv_camera.cpp
  src/version.cpp
  src/hooks/camera_hook.cpp
  src/hooks/ui_menu_hooks.cpp
  src/hooks/ui_overlay_hooks.cpp
  src/hooks/interaction_hook.cpp
  src/hooks/player_onaction_hook.cpp
  src/overlay/overlay.cpp
  src/overlay/dx_overlay.cpp
  src/overlay/overlay_ui.cpp
  src/presets/camera_preset.cpp
  src/presets/camera_preset_fields.cpp
  src/presets/preset_runtime.cpp
  src/presets/preset_store.cpp
)

# Applies the settings shared by the production ASI and the dev logic DLL: include
# paths, link libraries, and the Release speed/opt flags. /O2 favors speed: the
# overlay renders through a software WARP device with a per-pixel premultiply loop
# every frame, so the few KB /O1 would save are not worth deoptimizing that path on
# an .asi where image size is not a real constraint. /Gy + /Gw with /OPT:REF +
# /OPT:ICF still let the linker drop and fold unused functions and data.
function(configure_logic_target target)
  target_include_directories(${target} PRIVATE src)
  # imgui_lib brings the ImGui + Win32/DX11 backend include paths transitively.
  # d3d11/dxgi back the private WARP device; gdi32 backs the layered-window
  # composite; winmm backs timeBeginPeriod (also pinned via #pragma comment).
  target_link_libraries(${target} PRIVATE
    DetourModKit imgui_lib nlohmann_json::nlohmann_json
    psapi user32 kernel32 d3d11 dxgi gdi32 winmm)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2 /Gy /Gw>)
  target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endfunction()

if(NOT TPVCAMERA_DEV_BUILD)
  # --- Production Build: a single monolithic ASI ---
  add_library(${PROJECT_NAME}-ASI SHARED ${COMMON_SOURCES} src/dllmain.cpp)
  configure_logic_target(${PROJECT_NAME}-ASI)
  set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
    OUTPUT_NAME "KCD2_TPVCamera-temp-asi"
    SUFFIX ".asi"
    PREFIX ""
  )

  # Link-time optimization on Release (whole-program inlining across the mod).
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_supported LANGUAGES CXX OUTPUT _ipo_error)
  if(_ipo_supported)
    set_target_properties(${PROJECT_NAME}-ASI PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "LTO enabled for ${PROJECT_NAME}-ASI Release builds")
  else()
    message(STATUS "LTO not supported for ${PROJECT_NAME}-ASI: ${_ipo_error}")
  endif()

  # Normalize the artifact to the canonical name at the build-tree root so it is
  # found at the same path under both the Ninja and Visual Studio generators
  # (the release workflow copies build/KCD2_TPVCamera.asi).
  add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-ASI>
      ${CMAKE_CURRENT_BINARY_DIR}/KCD2_TPVCamera.asi
    COMMENT "Renaming ASI output to final name"
  )

  # Optional deploy of the production ASI to a configured game directory.
  if(TPVCAMERA_GAME_DIR)
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/KCD2_TPVCamera.asi
        "${TPVCAMERA_GAME_DIR}/KCD2_TPVCamera.asi"
      COMMENT "Deploying ASI to ${TPVCAMERA_GAME_DIR}"
    )
  endif()

  message(STATUS "KCD2_TPVCamera (${PROJECT_VERSION}) -- production ASI build -> KCD2_TPVCamera.asi")
else()
  # --- Dev Build: a thin loader ASI plus a logic DLL it hot-reloads in place ---

  # Thin loader ASI: loads the logic DLL and reloads it from the staging/ subdir
  # on a Numpad 0 press. Links only the Win32 libraries (no DetourModKit).
  add_library(${PROJECT_NAME}-loader SHARED src/dev/loader.cpp)
  target_link_libraries(${PROJECT_NAME}-loader PRIVATE kernel32 user32)
  set_target_properties(${PROJECT_NAME}-loader PROPERTIES
    OUTPUT_NAME "KCD2_TPVCamera"
    SUFFIX ".asi"
    PREFIX ""
  )

  # Logic DLL: all mod logic plus the exported Init/Shutdown entry points.
  add_library(${PROJECT_NAME}-logic SHARED ${COMMON_SOURCES} src/dev/logic_exports.cpp)
  configure_logic_target(${PROJECT_NAME}-logic)
  target_compile_definitions(${PROJECT_NAME}-logic PRIVATE TPVCAMERA_DEV_BUILD)
  set_target_properties(${PROJECT_NAME}-logic PROPERTIES
    OUTPUT_NAME "KCD2_TPVCamera_Logic"
    PREFIX ""
  )
  # /Zi emits a PDB; /DEBUG /INCREMENTAL:NO keep each rebuild a clean, reloadable DLL.
  target_compile_options(${PROJECT_NAME}-logic PRIVATE /Zi)
  target_link_options(${PROJECT_NAME}-logic PRIVATE /DEBUG /INCREMENTAL:NO)

  if(TPVCAMERA_GAME_DIR)
    # Loader ASI deploys straight into the game directory. copy_if_different
    # tolerates a running game holding it open; the loader changes rarely, so it
    # is expected to land while the game is closed.
    add_custom_command(TARGET ${PROJECT_NAME}-loader POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PROJECT_NAME}-loader>
        "${TPVCAMERA_GAME_DIR}/KCD2_TPVCamera.asi"
      COMMENT "Deploying loader ASI to ${TPVCAMERA_GAME_DIR}"
    )

    # Logic DLL (+PDB) deploys into staging/. The loader promotes it into the game
    # directory on startup and on each Numpad 0 reload, so the build never writes a
    # file the running game holds open.
    add_custom_command(TARGET ${PROJECT_NAME}-logic POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${TPVCAMERA_GAME_DIR}/staging"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PROJECT_NAME}-logic>
        "${TPVCAMERA_GAME_DIR}/staging/KCD2_TPVCamera_Logic.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:${PROJECT_NAME}-logic>
        "${TPVCAMERA_GAME_DIR}/staging/KCD2_TPVCamera_Logic.pdb"
      COMMENT "Deploying logic DLL+PDB to staging (loader promotes on startup / Numpad 0)"
    )
  else()
    message(STATUS "TPVCAMERA_GAME_DIR not set: dev build will not auto-deploy. "
                   "Set -DTPVCAMERA_GAME_DIR=<game plugins dir> to enable hot-reload deploy.")
  endif()

  message(STATUS "KCD2_TPVCamera (${PROJECT_VERSION}) -- DEV (two-DLL hot-reload) build")
  message(STATUS "    - ${PROJECT_NAME}-loader -> KCD2_TPVCamera.asi (thin loader)")
  message(STATUS "    - ${PROJECT_NAME}-logic  -> KCD2_TPVCamera_Logic.dll (mod logic)")
endif()
