# krono: clock sources and clock sync for the rover / ground stations.
# Today: synch (rtt and wire-pulse sync between two hosts over any datagram
# transport). On ESP-IDF use the component in idf/krono instead.
cmake_minimum_required(VERSION 3.16)
project(krono CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(krono_synch STATIC src/synch/Synch.cpp)
target_include_directories(krono_synch PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(krono_synch PUBLIC Threads::Threads)
target_compile_options(krono_synch PRIVATE -Wall -Wextra)

option(KRONO_BUILD_TESTS "Build host tests (GoogleTest)" OFF)
if(KRONO_BUILD_TESTS)
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(krono_tests tests/SynchTest.cpp)
    target_link_libraries(krono_tests PRIVATE krono_synch GTest::gtest_main)
    add_test(NAME krono_tests COMMAND krono_tests)
endif()
