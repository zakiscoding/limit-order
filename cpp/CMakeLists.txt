cmake_minimum_required(VERSION 3.20)
project(lob_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOB_INSTRUMENT "Accumulate rdtsc phase timers inside the book" OFF)

add_executable(lob_engine
    src/main.cpp
    src/book.cpp
    src/engine.cpp
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

include(CheckIPOSupported)
check_ipo_supported(RESULT lob_ipo_ok OUTPUT lob_ipo_error)
if(lob_ipo_ok)
    set_target_properties(lob_engine PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO disabled: ${lob_ipo_error}")
endif()
