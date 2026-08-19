include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# Windows-only knob; harmless elsewhere. Keeps the parent project's runtime
# library choice from being overridden by googletest's own default.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lob_tests
  test_fills.cpp
  test_priority.cpp
  test_cancel.cpp
  test_self_crossing.cpp
  test_empty_book.cpp
  test_market_sweep.cpp
  test_modify.cpp
  test_reduce_and_side_of.cpp
  test_top_levels.cpp
  test_full_book.cpp
  test_fuzz_apply_op.cpp
  test_fuzz_invariants.cpp
  test_fuzz_id_pool.cpp
  test_fuzz_generator.cpp
  test_fuzz_differential.cpp
  test_fuzz_differential_optimized.cpp
  test_fuzz_buggy_order_book.cpp
  test_fuzz_shrinker.cpp
  test_fuzz_shrunk_regressions.cpp
  test_itch_decode.cpp
  test_itch_integrity_checker.cpp
  test_bench_stats.cpp
  test_optimized_level_orders.cpp
  test_arena.cpp
)
target_link_libraries(lob_tests PRIVATE lob_itch lob_fuzz lob_bench lob_optimized_engine GTest::gtest_main)
target_compile_options(lob_tests PRIVATE -Wall -Wextra -Wpedantic)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_tests PRIVATE -fsanitize=address,undefined)
endif()

include(GoogleTest)
gtest_discover_tests(lob_tests)
