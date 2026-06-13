cmake_minimum_required(VERSION 3.20)
project(natska_engine VERSION 1.0.0 LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Architecture detection
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    message(FATAL_ERROR "INVENT 2027 requires x86-64 architecture")
endif()

# FIX: Position-independent code REQUIRED for modern linkers
# Remove -fno-pic -fno-pie, keep other optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-stack-protector -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG -DNATSKA_ENGINE_BUILD")

# Assembly flags
set(CMAKE_ASM_FLAGS_RELEASE "-O3 -march=native")

# Link-time optimization
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Source files
set(NATSKA_SOURCES
    src/main.cpp
    src/natska_asm.S
)

set(NATSKA_HEADERS
    include/natska/ring_buffer.hpp
    include/natska/asm.hpp
    include/natska/engine.hpp
)

# Executable
add_executable(natska_engine ${NATSKA_SOURCES} ${NATSKA_HEADERS})

target_include_directories(natska_engine PRIVATE include)

# Link libraries
target_link_libraries(natska_engine PRIVATE pthread numa)

# Installation
install(TARGETS natska_engine DESTINATION bin)
install(FILES ${NATSKA_HEADERS} DESTINATION include/natska)
