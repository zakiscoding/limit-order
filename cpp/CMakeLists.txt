cmake_minimum_required(VERSION 3.20)
project(lob_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOB_INSTRUMENT "Accumulate rdtsc phase timers inside the book" OFF)
option(LOB_COUNT_ALLOC "Replace global operator new/delete with counting versions (glibc only)" OFF)
option(LOB_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang only)" OFF)

add_executable(lob_engine
    src/main.cpp
    src/book.cpp
    src/engine.cpp
    src/latency.cpp
    src/alloc_counter.cpp
    src/perf_loop.cpp
    src/wire.cpp
    src/net.cpp
    src/net_selftest.cpp
)
target_include_directories(lob_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(LOB_INSTRUMENT)
    target_compile_definitions(lob_engine PRIVATE LOB_INSTRUMENT=1)
endif()

if(MSVC)
    target_compile_options(lob_engine PRIVATE /W4)
    target_compile_options(lob_engine PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(lob_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(lob_engine PRIVATE $<$<CONFIG:Release>:-O3>)
    target_compile_options(lob_engine PRIVATE $<$<CONFIG:RelWithDebInfo>:-O3>)
endif()

if(LOB_COUNT_ALLOC)
    if(LOB_SANITIZE)
        message(FATAL_ERROR "LOB_COUNT_ALLOC and LOB_SANITIZE both replace operator new; enable one")
    endif()
    target_compile_definitions(lob_engine PRIVATE LOB_COUNT_ALLOC=1)
endif()

if(LOB_SANITIZE AND NOT MSVC)
    target_compile_options(lob_engine PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lob_engine PRIVATE -fsanitize=address,undefined)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT lob_ipo_ok OUTPUT lob_ipo_error)
if(lob_ipo_ok)
    set_target_properties(lob_engine PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO disabled: ${lob_ipo_error}")
endif()
