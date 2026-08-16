cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_library(lob_engine
  src/order_book.cpp
)
target_include_directories(lob_engine PUBLIC include)
target_compile_features(lob_engine PUBLIC cxx_std_20)
target_compile_options(lob_engine PRIVATE -Wall -Wextra -Wpedantic)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_engine PUBLIC -fsanitize=address,undefined)
endif()

# ITCH wire parsing/validation is a separate library from the engine
# itself: it depends on nothing engine-specific (the integrity checker is
# pure order-reference bookkeeping, no price levels), and keeping it
# separate means a future consumer of just the parser -- e.g. a different
# venue's replay tool -- doesn't have to link the matching engine at all.
add_library(lob_itch
  src/itch/integrity_checker.cpp
  src/itch/mapped_file.cpp
)
target_include_directories(lob_itch PUBLIC include)
target_compile_features(lob_itch PUBLIC cxx_std_20)
target_compile_options(lob_itch PRIVATE -Wall -Wextra -Wpedantic)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_itch PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_itch PUBLIC -fsanitize=address,undefined)
endif()

# Phase 3's differential-fuzzing generator. Depends on lob_engine (its
# internal bookkeeping engine is the reference OrderBook) but not lob_itch
# -- the fuzzer generates its own synthetic op streams and has nothing to
# do with ITCH parsing.
add_library(lob_fuzz
  src/fuzz/generator.cpp
  src/fuzz/buggy_order_book.cpp
  src/fuzz/shrinker.cpp
)
target_include_directories(lob_fuzz PUBLIC include)
target_compile_features(lob_fuzz PUBLIC cxx_std_20)
target_compile_options(lob_fuzz PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lob_fuzz PUBLIC lob_engine)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_fuzz PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_fuzz PUBLIC -fsanitize=address,undefined)
endif()

# Phase 4's PMU-based benchmarking. macOS/Apple Silicon only (dlopen's
# private frameworks that don't exist on Linux) -- deliberately not
# behind an option/ifdef here since this whole project targets this one
# platform; see docs/benchmark_methodology.md for why.
add_library(lob_bench
  src/bench/pmu.cpp
  src/bench/stats.cpp
  src/bench/scheduling.cpp
  src/bench/frequency.cpp
)
target_include_directories(lob_bench PUBLIC include)
target_compile_features(lob_bench PUBLIC cxx_std_20)
target_compile_options(lob_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_replay
  tools/itch_replay.cpp
)
target_link_libraries(itch_replay PRIVATE lob_engine lob_itch)
target_compile_options(itch_replay PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_snapshot_dump
  tools/itch_snapshot_dump.cpp
)
target_link_libraries(itch_snapshot_dump PRIVATE lob_engine lob_itch)
target_compile_options(itch_snapshot_dump PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fuzz_mutation_test
  tools/fuzz_mutation_test.cpp
)
target_link_libraries(fuzz_mutation_test PRIVATE lob_fuzz)
target_compile_options(fuzz_mutation_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fuzz_soak
  tools/fuzz_soak.cpp
)
target_link_libraries(fuzz_soak PRIVATE lob_fuzz)
target_compile_options(fuzz_soak PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pmu_validate
  tools/pmu_validate.cpp
)
target_link_libraries(pmu_validate PRIVATE lob_bench)
target_compile_options(pmu_validate PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)
