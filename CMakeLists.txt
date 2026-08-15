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
