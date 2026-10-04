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
add_library(trekker_engine STATIC
  src/engine/interpolate.cpp
  src/engine/deck.cpp
  src/engine/mixer.cpp
  src/engine/track_loader.cpp
  src/engine/audio_device.cpp
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
)

target_include_directories(trekker_engine
  PUBLIC  src/engine
  PRIVATE third_party third_party/miniz
)

# Our own code gets warnings; vendored miniz is compiled silently.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(
    src/engine/interpolate.cpp
    src/engine/deck.cpp
    src/engine/mixer.cpp
    src/engine/track_loader.cpp
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

# --- console front-end (M1; the ImGui UI arrives in M3) ---
add_executable(trekker-ng src/console/main.cpp)
target_include_directories(trekker-ng PRIVATE third_party) # miniaudio.h for --render
target_link_libraries(trekker-ng PRIVATE trekker_engine)

if(WIN32)
  # ole32: COM pieces miniaudio may bind directly on WASAPI.
  target_link_libraries(trekker-ng PRIVATE ole32)
endif()

if(MINGW)
  # Fully static exe so dist/ is one self-contained file (SPEC §10.2).
  target_link_options(trekker-ng PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# --- tests (SPEC §9) - added in M2; option exists from M1 so build.sh is stable ---
option(TREKKER_BUILD_TESTS "Build the engine test suite" ON)

if(TREKKER_BUILD_TESTS)
  enable_testing()
  add_executable(trekker_tests
    tests/test_main.cpp
    tests/test_interpolate.cpp
    tests/test_deck.cpp
  )
  target_include_directories(trekker_tests PRIVATE third_party tests)
  target_link_libraries(trekker_tests PRIVATE trekker_engine)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
      tests/test_main.cpp
      tests/test_interpolate.cpp
      tests/test_deck.cpp
      PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
    )
  endif()
  add_test(NAME engine COMMAND trekker_tests)
endif()
