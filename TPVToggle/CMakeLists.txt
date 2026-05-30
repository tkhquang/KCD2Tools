cmake_minimum_required(VERSION 3.25)

project(KCD2_TPVToggle VERSION 0.6.0 LANGUAGES CXX)

# --- Build Options ---
# TPVTOGGLE_DEV_BUILD OFF (default): a single production ASI built from the logic
# plus dllmain.cpp. ON: a thin loader ASI plus a logic DLL, enabling DetourModKit
# hot-reload for fast iteration (the loader reloads the logic in place; see src/dev/).
option(TPVTOGGLE_DEV_BUILD "Build the two-DLL hot-reload dev configuration (thin loader ASI + logic DLL)" OFF)
set(TPVTOGGLE_GAME_DIR "" CACHE PATH
  "Directory to deploy the built mod into (the game's mod-loader/plugins directory). Used by the dev build for hot-reload.")

# --- Standard and Compiler Options ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project ships as an MSVC build only.
if(NOT MSVC)
  message(FATAL_ERROR "KCD2_TPVToggle requires the MSVC toolchain (Visual Studio 2022+).")
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

add_subdirectory(external/DetourModKit)
if(NOT TARGET DetourModKit)
  message(FATAL_ERROR "DetourModKit target not created by subdirectory")
endif()

# --- nlohmann/json Dependency (header-only submodule) ---
set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/json/include")
if(NOT EXISTS "${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "nlohmann/json not found at 'external/json'. "
    "Initialize submodules: 'git submodule update --init --recursive'")
endif()

# --- Source Files ---
set(COMMON_SOURCES
  src/camera_profile_thread.cpp
  src/camera_profile.cpp
  src/config.cpp
  src/game_interface.cpp
  src/global_state.cpp
  src/toggle_thread.cpp
  src/tpv_toggle.cpp
  src/transition_manager.cpp
  src/version.cpp
  src/hooks/entity_hooks.cpp
  src/hooks/event_hooks.cpp
  src/hooks/fov_hook.cpp
  src/hooks/tpv_camera_hook.cpp
  src/hooks/tpv_input_hook.cpp
  src/hooks/ui_menu_hooks.cpp
  src/hooks/ui_overlay_hooks.cpp
)

# Applies the settings shared by the production ASI and the dev logic DLL: include
# paths, link libraries, and the Release size/opt flags. /O1 favors size (suited to
# an injected DLL); /Gy + /Gw with /OPT:REF + /OPT:ICF let the linker drop and fold
# unused functions and data.
function(configure_logic_target target)
  target_include_directories(${target} PRIVATE src ${JSON_INCLUDE_DIR})
  target_link_libraries(${target} PRIVATE DetourModKit psapi user32 kernel32)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O1 /Gy /Gw>)
  target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endfunction()

if(NOT TPVTOGGLE_DEV_BUILD)
  # --- Production Build: a single monolithic ASI ---
  add_library(${PROJECT_NAME}-ASI SHARED ${COMMON_SOURCES} src/dllmain.cpp)
  configure_logic_target(${PROJECT_NAME}-ASI)
  set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
    OUTPUT_NAME "KCD2_TPVToggle-temp-asi"
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
  # (the release workflow copies build/KCD2_TPVToggle.asi).
  add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:${PROJECT_NAME}-ASI>
      ${CMAKE_CURRENT_BINARY_DIR}/KCD2_TPVToggle.asi
    COMMENT "Renaming ASI output to final name"
  )

  # Optional deploy of the production ASI to a configured game directory.
  if(TPVTOGGLE_GAME_DIR)
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/KCD2_TPVToggle.asi
        "${TPVTOGGLE_GAME_DIR}/KCD2_TPVToggle.asi"
      COMMENT "Deploying ASI to ${TPVTOGGLE_GAME_DIR}"
    )
  endif()

  message(STATUS "KCD2_TPVToggle (${PROJECT_VERSION}) -- production ASI build -> KCD2_TPVToggle.asi")
else()
  # --- Dev Build: a thin loader ASI plus a logic DLL it hot-reloads in place ---

  # Thin loader ASI: loads the logic DLL and reloads it from the staging/ subdir
  # on a Numpad 0 press. Links only the Win32 libraries (no DetourModKit).
  add_library(${PROJECT_NAME}-loader SHARED src/dev/loader.cpp)
  target_link_libraries(${PROJECT_NAME}-loader PRIVATE kernel32 user32)
  set_target_properties(${PROJECT_NAME}-loader PROPERTIES
    OUTPUT_NAME "KCD2_TPVToggle"
    SUFFIX ".asi"
    PREFIX ""
  )

  # Logic DLL: all mod logic plus the exported Init/Shutdown entry points.
  add_library(${PROJECT_NAME}-logic SHARED ${COMMON_SOURCES} src/dev/logic_exports.cpp)
  configure_logic_target(${PROJECT_NAME}-logic)
  target_compile_definitions(${PROJECT_NAME}-logic PRIVATE TPVTOGGLE_DEV_BUILD)
  set_target_properties(${PROJECT_NAME}-logic PROPERTIES
    OUTPUT_NAME "KCD2_TPVToggle_Logic"
    PREFIX ""
  )
  # /Zi emits a PDB; /DEBUG /INCREMENTAL:NO keep each rebuild a clean, reloadable DLL.
  target_compile_options(${PROJECT_NAME}-logic PRIVATE /Zi)
  target_link_options(${PROJECT_NAME}-logic PRIVATE /DEBUG /INCREMENTAL:NO)

  if(TPVTOGGLE_GAME_DIR)
    # Loader ASI deploys straight into the game directory. copy_if_different
    # tolerates a running game holding it open; the loader changes rarely, so it
    # is expected to land while the game is closed.
    add_custom_command(TARGET ${PROJECT_NAME}-loader POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PROJECT_NAME}-loader>
        "${TPVTOGGLE_GAME_DIR}/KCD2_TPVToggle.asi"
      COMMENT "Deploying loader ASI to ${TPVTOGGLE_GAME_DIR}"
    )

    # Logic DLL (+PDB) deploys into staging/. The loader promotes it into the game
    # directory on startup and on each Numpad 0 reload, so the build never writes a
    # file the running game holds open.
    add_custom_command(TARGET ${PROJECT_NAME}-logic POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${TPVTOGGLE_GAME_DIR}/staging"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PROJECT_NAME}-logic>
        "${TPVTOGGLE_GAME_DIR}/staging/KCD2_TPVToggle_Logic.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:${PROJECT_NAME}-logic>
        "${TPVTOGGLE_GAME_DIR}/staging/KCD2_TPVToggle_Logic.pdb"
      COMMENT "Deploying logic DLL+PDB to staging (loader promotes on startup / Numpad 0)"
    )
  else()
    message(STATUS "TPVTOGGLE_GAME_DIR not set: dev build will not auto-deploy. "
                   "Set -DTPVTOGGLE_GAME_DIR=<game plugins dir> to enable hot-reload deploy.")
  endif()

  message(STATUS "KCD2_TPVToggle (${PROJECT_VERSION}) -- DEV (two-DLL hot-reload) build")
  message(STATUS "    - ${PROJECT_NAME}-loader -> KCD2_TPVToggle.asi (thin loader)")
  message(STATUS "    - ${PROJECT_NAME}-logic  -> KCD2_TPVToggle_Logic.dll (mod logic)")
endif()
