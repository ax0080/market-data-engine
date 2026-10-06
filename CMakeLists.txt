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
add_executable(mde_tests tests/test_main.cpp tests/test_books.cpp tests/test_feeds.cpp tests/test_order_entry.cpp tests/test_fix.cpp)
target_link_libraries(mde_tests mde)
enable_testing()
add_test(NAME mde_tests COMMAND mde_tests)

# Replay tools
add_executable(replay_itch tools/replay_itch.cpp)
target_link_libraries(replay_itch mde)
add_executable(replay_crypto tools/replay_crypto.cpp)
target_link_libraries(replay_crypto mde)
add_executable(oe_codec_bench tools/oe_codec_bench.cpp)
target_link_libraries(oe_codec_bench mde)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(udp_bench tools/udp_bench.cpp)
    target_link_libraries(udp_bench mde Threads::Threads)
endif()

# Exchange simulator + tick-to-trade client. The matching engine is
# github.com/ax0080/orderbook-engine: a local checkout (MDE_ORDERBOOK_DIR) or a
# pinned commit fetched at configure time.
option(MDE_BUILD_SIM "Build the exchange simulator and tick-to-trade client" ON)
set(MDE_ORDERBOOK_DIR "" CACHE PATH "Local orderbook-engine checkout (empty = fetch from GitHub)")
if(MDE_BUILD_SIM)
    set(EXCHANGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(EXCHANGE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(EXCHANGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(MDE_ORDERBOOK_DIR)
        add_subdirectory(${MDE_ORDERBOOK_DIR} orderbook-engine EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(orderbook_engine
            GIT_REPOSITORY https://github.com/ax0080/orderbook-engine.git
            GIT_TAG        13c8dfe4f4d0030231e97aa820fddc8f47b29357)
        FetchContent_MakeAvailable(orderbook_engine)
    endif()

    add_library(mde_sim INTERFACE)
    target_include_directories(mde_sim INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sim)
    target_link_libraries(mde_sim INTERFACE mde exchange)

    add_executable(mde_sim_tests tests/test_main.cpp tests/test_gateway.cpp tests/test_fix_gateway.cpp)
    target_link_libraries(mde_sim_tests mde_sim)
    add_test(NAME mde_sim_tests COMMAND mde_sim_tests)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(exchange_sim sim/exchange_sim.cpp)
        target_link_libraries(exchange_sim mde_sim)
        add_executable(trader tools/trader.cpp)
        target_link_libraries(trader mde)
    endif()
endif()
