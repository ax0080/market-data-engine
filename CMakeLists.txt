cmake_minimum_required(VERSION 3.20)
project(MarketDataEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

option(MDE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(MDE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# Header-only core
add_library(mde INTERFACE)
target_include_directories(mde INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Tests (no external framework)
add_executable(mde_tests tests/test_main.cpp tests/test_books.cpp tests/test_feeds.cpp)
target_link_libraries(mde_tests mde)
enable_testing()
add_test(NAME mde_tests COMMAND mde_tests)

# Replay tools
add_executable(replay_itch tools/replay_itch.cpp)
target_link_libraries(replay_itch mde)
add_executable(replay_crypto tools/replay_crypto.cpp)
target_link_libraries(replay_crypto mde)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(udp_bench tools/udp_bench.cpp)
    target_link_libraries(udp_bench mde Threads::Threads)
endif()
