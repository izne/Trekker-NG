cmake_minimum_required(VERSION 3.20)
project(trekker-ng LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- engine: no UI dependencies, builds as a static library (SPEC §3.1) ---

# M9: SoundTouch BPMDetect (LGPL 2.1) estimates BPM for tracks that carry
# none - pacman mingw-w64-x86_64-soundtouch; the DLL ships next to the
# exes (same pattern as SDL2, see build.sh).
find_path(SOUNDTOUCH_INCLUDE_DIR soundtouch/BPMDetect.h)
find_library(SOUNDTOUCH_LIBRARY SoundTouch)
if(NOT SOUNDTOUCH_INCLUDE_DIR OR NOT SOUNDTOUCH_LIBRARY)
  message(FATAL_ERROR
    "SoundTouch not found - install it first: pacman -S mingw-w64-x86_64-soundtouch")
endif()
find_file(SOUNDTOUCH_DLL libSoundTouch-1.dll)

add_library(trekker_engine STATIC
  src/engine/interpolate.cpp
  src/engine/deck.cpp
  src/engine/eq.cpp
  src/engine/mixer.cpp
  src/engine/settings.cpp
  src/engine/track_loader.cpp
  src/engine/audio_device.cpp
  src/engine/recorder.cpp
  src/engine/bpm_detect.cpp
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
)

target_include_directories(trekker_engine
  PUBLIC  src/engine
  PRIVATE third_party third_party/miniz ${SOUNDTOUCH_INCLUDE_DIR}
)
target_link_libraries(trekker_engine PUBLIC ${SOUNDTOUCH_LIBRARY})

# Stage the SoundTouch runtime next to every runnable (UI, console, tests):
# the engine links it, so nothing starts without the DLL present.
function(trekker_stage_soundtouch target)
  if(SOUNDTOUCH_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SOUNDTOUCH_DLL}" "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Staging libSoundTouch-1.dll next to ${target}")
  endif()
endfunction()

# Our own code gets warnings; vendored miniz is compiled silently.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(
    src/engine/interpolate.cpp
    src/engine/deck.cpp
    src/engine/eq.cpp
    src/engine/mixer.cpp
    src/engine/settings.cpp
    src/engine/track_loader.cpp
    src/engine/recorder.cpp
    src/engine/bpm_detect.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
  )
  set_source_files_properties(
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
    PROPERTIES COMPILE_OPTIONS "-w"
  )
endif()

# --- Dear ImGui, vendored + SDL2 (SPEC §3) ---
find_package(SDL2 CONFIG REQUIRED)

add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PUBLIC SDL_MAIN_HANDLED) # our own entry point
target_link_libraries(imgui PUBLIC SDL2::SDL2)

# --- console front-end (M1-M2; kept as trekker-console in M3) ---
add_executable(trekker-console src/console/main.cpp)
target_include_directories(trekker-console PRIVATE third_party) # miniaudio.h for --render
target_link_libraries(trekker-console PRIVATE trekker_engine)
trekker_stage_soundtouch(trekker-console)

if(WIN32)
  # ole32: COM pieces miniaudio may bind directly on WASAPI.
  target_link_libraries(trekker-console PRIVATE ole32)
endif()

if(MINGW)
  # Fully static exe so dist/ is one self-contained file (SPEC §10.2).
  target_link_options(trekker-console PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# --- ImGui UI front-end (M3, SPEC §6/§8) ---
add_executable(trekker-ng
  src/ui/main.cpp
  src/ui/deck_view.cpp
)
  # comdlg32: GetOpenFileNameW zip picker for the Load... button (M5e).
  target_link_libraries(trekker-ng PRIVATE trekker_engine imgui ole32 opengl32 comdlg32)
trekker_stage_soundtouch(trekker-ng)
if(MINGW)
  # -static covers the driver's implicit winpthread/libstdc++ (no extra DLLs);
  # SDL2 stays a DLL - its import lib is referenced by full path above.
  target_link_options(trekker-ng PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(
    src/ui/main.cpp
    src/ui/deck_view.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
  )
endif()

# --- tests (SPEC §9) - added in M2; option exists from M1 so build.sh is stable ---
option(TREKKER_BUILD_TESTS "Build the engine test suite" ON)

if(TREKKER_BUILD_TESTS)
  enable_testing()
  add_executable(trekker_tests
    tests/test_main.cpp
    tests/test_interpolate.cpp
    tests/test_deck.cpp
    tests/test_eq.cpp
    tests/test_mixer.cpp
    tests/test_loader.cpp
    tests/test_settings.cpp
    tests/test_recorder.cpp
    tests/test_flat.cpp
    tests/test_bpm.cpp
  )
  target_include_directories(trekker_tests PRIVATE third_party tests)
  target_link_libraries(trekker_tests PRIVATE trekker_engine)
  trekker_stage_soundtouch(trekker_tests)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
      tests/test_main.cpp
      tests/test_interpolate.cpp
      tests/test_deck.cpp
      tests/test_eq.cpp
      tests/test_mixer.cpp
      tests/test_loader.cpp
      tests/test_settings.cpp
      tests/test_recorder.cpp
      tests/test_flat.cpp
      tests/test_bpm.cpp
      PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
    )
  endif()
  add_test(NAME engine COMMAND trekker_tests)
endif()
