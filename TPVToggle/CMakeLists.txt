cmake_minimum_required(VERSION 3.16)

project(KCD2_TPVToggle VERSION 0.5.1 LANGUAGES CXX)

# --- Standard and Compiler Options ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Compiler Flags ---
if(MSVC)
  add_compile_options(/W4)
else()
  # GCC/Clang/MinGW specific flags
  add_compile_options(-Wall -Wextra -Os -ffunction-sections -fdata-sections)
endif()

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

# --- DetourModKit Dependency ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/DetourModKit/CMakeLists.txt")
  message(STATUS "Configuring DetourModKit from: external/DetourModKit")
  add_subdirectory(external/DetourModKit)

  # Use the exact target name from DetourModKit
  if(TARGET DetourModKit)
    set(DETOURMODKIT_TARGET DetourModKit)
    message(STATUS "DetourModKit target found: ${DETOURMODKIT_TARGET}")
  else()
    message(FATAL_ERROR "DetourModKit target not created by subdirectory")
  endif()
else()
  message(FATAL_ERROR "DetourModKit not found at 'external/DetourModKit'. "
    "Please ensure the submodule is initialized: "
    "'git submodule update --init --recursive'")
endif()

# --- nlohmann/json Dependency ---
set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/json/include")

if(NOT EXISTS "${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "nlohmann/json not found at 'external/json'. "
    "Please ensure the submodule is initialized: "
    "'git submodule update --init --recursive'")
endif()

# --- Source Files ---
set(COMMON_SOURCES
  src/camera_profile_thread.cpp
  src/camera_profile.cpp
  src/config.cpp
  src/dllmain.cpp
  src/game_interface.cpp
  src/global_state.cpp
  src/math_utils.cpp
  src/toggle_thread.cpp
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

set(ASI_SOURCES
  ${COMMON_SOURCES}
)

# --- Include Directories ---
set(COMMON_INCLUDE_DIRS
  src
  ${JSON_INCLUDE_DIR}
)

# --- ASI Target (.asi file) ---
add_library(${PROJECT_NAME}-ASI SHARED ${ASI_SOURCES})

target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
  DetourModKit
  psapi
  user32
  kernel32
)

# Set output name and properties for ASI
set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
  OUTPUT_NAME "KCD2_TPVToggle-temp-asi"
  SUFFIX ".asi"
  PREFIX ""
)

# Linker flags for size optimization and static linking (MinGW)
if(NOT MSVC)
  target_link_options(${PROJECT_NAME}-ASI PRIVATE
    -Wl,--gc-sections
    -static-libgcc
    -static-libstdc++
    -static
  )
endif()

# Post-build steps to rename to desired names
add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
  $<TARGET_FILE:${PROJECT_NAME}-ASI>
  ${CMAKE_CURRENT_BINARY_DIR}/KCD2_TPVToggle.asi
  COMMENT "Renaming ASI output to final name"
)

# --- Build Summary ---
message(STATUS "---------------------------------------------------------------------")
message(STATUS "KCD2_TPVToggle (${PROJECT_VERSION}) Configuration Summary:")
message(STATUS "  Generator:                   ${CMAKE_GENERATOR}")
message(STATUS "  Build Type:                  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Targets:")
message(STATUS "    - ${PROJECT_NAME}-ASI      -> KCD2_TPVToggle.asi (via post-build copy)")
message(STATUS "---------------------------------------------------------------------")
