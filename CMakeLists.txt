cmake_minimum_required(VERSION 3.25)
project(parkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Engine code with no platform dependencies: data loaders and game logic.
add_library(parkan_core STATIC
  src/core/bytes.cpp
  src/core/paths.cpp
  src/res/nres.cpp
  src/res/texm.cpp
  src/res/mesh.cpp
  src/res/material.cpp
  src/res/library.cpp
  src/res/ctld.cpp
  src/res/swav.cpp
  src/res/fx.cpp
  src/control/motion_graph.cpp
  src/control/unit_motion.cpp
  src/control/turret.cpp
  src/scene/model.cpp
  src/scene/render_mesh.cpp
  src/scene/mission_scene.cpp
  src/scene/sky.cpp
  src/world/ground.cpp
  src/world/collision.cpp
  src/world/live_unit.cpp
  src/res/terrain.cpp
  src/ai/script_program.cpp
  src/ai/formula.cpp
  src/ai/script_vm.cpp
  src/mission/research_tree.cpp
  src/mission/tma.cpp
  src/mission/scheme.cpp
  src/mission/ske.cpp
  src/input/scancodes.cpp
  src/input/translate_table.cpp
  src/input/man_manager.cpp
  src/input/keymap.cpp
)
target_include_directories(parkan_core PUBLIC src)
target_compile_options(parkan_core PRIVATE -Wall -Wextra -Wpedantic)

# Ogg Vorbis decoder (third_party/stb_vorbis/README.md), warnings off for the vendored code.
add_library(stb_vorbis STATIC third_party/stb_vorbis/stb_vorbis_impl.cpp)
target_include_directories(stb_vorbis SYSTEM PUBLIC third_party/stb_vorbis)
target_compile_options(stb_vorbis PRIVATE -w)

add_executable(parkan-check tools/parkan_check.cpp)
target_link_libraries(parkan-check PRIVATE parkan_core stb_vorbis)

add_executable(parkan-dump tools/parkan_dump.cpp)
target_link_libraries(parkan-dump PRIVATE parkan_core)

add_executable(parkan-tests tools/parkan_tests.cpp)
target_link_libraries(parkan-tests PRIVATE parkan_core)
enable_testing()
add_test(NAME unit COMMAND parkan-tests)

# The game itself (SDL3: window, input, audio; SDL_GPU renders through Metal on macOS).
find_package(SDL3 REQUIRED CONFIG)

# Sound and music output on SDL audio streams (notes/audio.md).
add_library(parkan_audio STATIC src/audio/audio_engine.cpp)
target_link_libraries(parkan_audio PUBLIC parkan_core PRIVATE SDL3::SDL3 stb_vorbis)
target_compile_options(parkan_audio PRIVATE -Wall -Wextra -Wpedantic)

add_executable(parkan MACOSX_BUNDLE src/app/main.cpp src/app/scene_viewer.cpp src/app/play.cpp)
target_link_libraries(parkan PRIVATE parkan_audio parkan_core SDL3::SDL3)
set_target_properties(parkan PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER org.parkan.port
  MACOSX_BUNDLE_BUNDLE_NAME "Parkan")
