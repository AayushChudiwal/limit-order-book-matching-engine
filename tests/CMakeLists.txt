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
)
target_link_libraries(lob_tests PRIVATE lob_engine GTest::gtest_main)
target_compile_options(lob_tests PRIVATE -Wall -Wextra -Wpedantic)

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_tests PRIVATE -fsanitize=address,undefined)
endif()

include(GoogleTest)
gtest_discover_tests(lob_tests)
