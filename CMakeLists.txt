cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Level/order-storage churn counters (see include/lob/bench/churn.hpp),
# OFF by default so the N=10/N=20 PMU cycle comparisons this project's
# benchmark docs cite are never silently carrying this instrumentation's
# overhead. Turn ON only for a dedicated churn-diagnostic run.
option(LOB_TRACK_CHURN "Instrument OptimizedOrderBook's level/order-storage allocation churn" OFF)

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

# Phase 4's benchmarking support, split in two by PLATFORM PORTABILITY --
# not an arbitrary organizational choice, and worth stating why since it
# used to be one library.
#
# lob_bench (this one) is the portable half: churn counters and
# percentile stats, both dependency-free (<cstdint>, <algorithm>). It is
# on the critical path for things that MUST build everywhere:
# lob_optimized_engine includes bench/churn.hpp, so every consumer of the
# optimized engine -- lob_tests, and Phase 5's forthcoming sanitizer CI
# job on the fuzz corpus -- transitively needs this to compile on Linux.
#
# lob_bench_darwin (below) is the macOS/Apple-Silicon-only half: PMU
# counters (dlopen'd private frameworks), QOS/core pinning
# (<pthread/qos.h>), and mach timebase (<mach/mach_time.h>).
#
# Keeping them merged is what took CI down: because lob_optimized_engine
# -> lob_bench -> pmu.cpp, GitHub's ubuntu runners failed at
# `#include <mach/mach_time.h>` before compiling a single line of engine
# code, so BOTH the Release and the ASan/UBSan jobs were dead for three
# pushes -- including the whole window in which step 2's hand-rolled
# LevelOrders storage was written. The sanitizer job is the specific tool
# docs/phase5_readiness.md names for the arena work in step 4, so it has
# to be able to build on Linux, which means the engine's dependency edge
# must land on portable code only.
add_library(lob_bench
  src/bench/stats.cpp
  src/bench/churn.cpp
)
target_include_directories(lob_bench PUBLIC include)
target_compile_features(lob_bench PUBLIC cxx_std_20)
target_compile_options(lob_bench PRIVATE -Wall -Wextra -Wpedantic)
# PUBLIC: propagates to lob_optimized_engine (LevelOrders calls these) and
# to bench_matching_engine (reads ReadChurn()) so both always see the SAME
# value this was configured with -- a mismatch between how churn.cpp and
# optimized_order_book.cpp were each compiled would be a link error (the
# real counter functions only exist in the ON branch).
target_compile_definitions(lob_bench PUBLIC LOB_TRACK_CHURN=$<BOOL:${LOB_TRACK_CHURN}>)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_bench PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_bench PUBLIC -fsanitize=address,undefined)
endif()

# The Apple-only half. Guarded rather than removed: this project's
# benchmark numbers are all produced on this platform (see
# docs/benchmark_methodology.md for why that's deliberate), so these
# stay first-class here -- they just no longer block a Linux build of
# anything else.
if(APPLE)
  add_library(lob_bench_darwin
    src/bench/pmu.cpp
    src/bench/scheduling.cpp
    src/bench/frequency.cpp
  )
  target_include_directories(lob_bench_darwin PUBLIC include)
  target_compile_features(lob_bench_darwin PUBLIC cxx_std_20)
  target_compile_options(lob_bench_darwin PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(lob_bench_darwin PUBLIC lob_bench)

  if(LOB_ENABLE_SANITIZERS)
    target_compile_options(lob_bench_darwin PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lob_bench_darwin PUBLIC -fsanitize=address,undefined)
  endif()
endif()

# Phase 5's optimized engine (step 2 onward) -- see
# include/lob/optimized_order_book.hpp's class comment for why this is a
# separate library/class from lob_engine rather than more changes to
# order_book.cpp. Depends on lob_bench for the churn counters LevelOrders
# calls; lob_engine (the reference) deliberately does not and never will.
add_library(lob_optimized_engine
  src/optimized_order_book.cpp
)
target_include_directories(lob_optimized_engine PUBLIC include)
target_compile_features(lob_optimized_engine PUBLIC cxx_std_20)
target_compile_options(lob_optimized_engine PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lob_optimized_engine PUBLIC lob_bench)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_optimized_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_optimized_engine PUBLIC -fsanitize=address,undefined)
endif()

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

# Portable (lob_itch only, no engine, no PMU) -- deliberately buildable
# on Linux CI: it is the committed evidence for docs/phase5_plan.md's
# step-3 removal and step-6 sizing constraints, so it should not rot.
add_executable(tick_granularity_scan
  tools/tick_granularity_scan.cpp
)
target_link_libraries(tick_granularity_scan PRIVATE lob_itch)
target_compile_options(tick_granularity_scan PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fuzz_mutation_test
  tools/fuzz_mutation_test.cpp
)
target_link_libraries(fuzz_mutation_test PRIVATE lob_fuzz)
target_compile_options(fuzz_mutation_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fuzz_soak
  tools/fuzz_soak.cpp
)
# lob_optimized_engine as well as lob_fuzz: the soak gained a differential
# mode (OrderBook vs OptimizedOrderBook) and an invariant mode pointed at
# the optimized engine. Before that it linked only lob_fuzz, which is why
# it could not diff against Phase 5's engine at all -- see the header
# comment in tools/fuzz_soak.cpp.
target_link_libraries(fuzz_soak PRIVATE lob_fuzz lob_optimized_engine)
target_compile_options(fuzz_soak PRIVATE -Wall -Wextra -Wpedantic)

# The three PMU-dependent tools. Apple-only for the same reason
# lob_bench_darwin is: they read hardware performance counters through
# private frameworks that only exist here. A Linux build simply doesn't
# produce them -- it isn't a platform where this project's benchmark
# numbers are meaningful anyway, and CI's job there is to build and test
# the ENGINE (including under sanitizers), not to measure it.
if(APPLE)
  add_executable(pmu_validate
    tools/pmu_validate.cpp
  )
  target_link_libraries(pmu_validate PRIVATE lob_bench_darwin)
  target_compile_options(pmu_validate PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(bench_matching_engine
    tools/bench_matching_engine.cpp
  )
  # lob_optimized_engine, not lob_engine -- this tool targets Phase 5's
  # optimized engine (see optimized_order_book.hpp's class comment on why
  # that's a separate class from the reference OrderBook). lob_bench is
  # linked PUBLIC by lob_optimized_engine already (for the churn counters),
  # which also carries the correct LOB_TRACK_CHURN compile definition here;
  # lob_bench_darwin adds the PMU/timing/pinning half this tool also needs.
  target_link_libraries(bench_matching_engine PRIVATE lob_optimized_engine lob_bench_darwin lob_itch)
  target_compile_options(bench_matching_engine PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(cache_line_probe
    tools/cache_line_probe.cpp
  )
  target_link_libraries(cache_line_probe PRIVATE lob_bench_darwin)
  target_compile_options(cache_line_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_subdirectory(tests)
