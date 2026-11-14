cmake_minimum_required(VERSION 3.20)

project(kestrel VERSION 0.1.0 LANGUAGES CXX)

# Phase 0 host this file was brought up on:
#   Intel Core i7-8650U, 4 physical cores / 8 logical, SMT pairs (0,1) (2,3) (4,5) (6,7)
#   L1d 32 KiB, L2 256 KiB, L3 8 MiB, cache line 64 B, no AVX-512
#   g++ 13.3, kernel 6.6 WSL2, liburing 2.5 present but not linked yet
# -march=native on this CPU is AVX2 + FMA + BMI2 + SSE4.2. It is not AVX-512.
# Container builds that must be reproducible should pass -DKESTREL_MARCH=x86-64-v3
# (that is the AVX2 baseline, and this CPU implements it).

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(KESTREL_MARCH native CACHE STRING "Argument to -march=")
set(KESTREL_L2_BYTES 262144 CACHE STRING "Per-core L2 budget in bytes; rings larger than this fail to compile")
set(KESTREL_SANITIZER "" CACHE STRING "Sanitizer: empty, address, thread, or undefined")
set_property(CACHE KESTREL_SANITIZER PROPERTY STRINGS "" address thread undefined)

if(NOT KESTREL_SANITIZER STREQUAL ""
   AND NOT KESTREL_SANITIZER STREQUAL "address"
   AND NOT KESTREL_SANITIZER STREQUAL "thread"
   AND NOT KESTREL_SANITIZER STREQUAL "undefined")
  message(FATAL_ERROR "KESTREL_SANITIZER must be empty, address, thread, or undefined")
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBURING QUIET liburing)
endif()
if(LIBURING_FOUND)
  message(STATUS "liburing ${LIBURING_VERSION} detected, not linked until Phase 1")
else()
  message(STATUS "liburing not found; Phase 1 will fall back to epoll")
endif()

add_library(kestrel_core STATIC src/cpu.cpp)
target_include_directories(kestrel_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(kestrel_core PUBLIC cxx_std_20)
target_compile_definitions(kestrel_core
  PUBLIC
    KESTREL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KESTREL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KESTREL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    KESTREL_L2_BYTES=${KESTREL_L2_BYTES}
)
target_link_libraries(kestrel_core PUBLIC Threads::Threads)

target_compile_options(kestrel_core PUBLIC
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wconversion
  -Wshadow
  -Wstrict-aliasing=2
  -fno-exceptions
  -fno-rtti
  -fno-omit-frame-pointer
  -march=${KESTREL_MARCH}
)

# Sanitizer builds force -O1. ASan and TSan cannot be combined.
# The address target also enables UBSan: the two catch different bugs
# and they do compose. TSan does not.
if(KESTREL_SANITIZER STREQUAL "")
  target_compile_options(kestrel_core PUBLIC
    $<$<CONFIG:Release>:-O3 -fno-unwind-tables -fno-asynchronous-unwind-tables>
    $<$<CONFIG:RelWithDebInfo>:-O3 -g>
    $<$<CONFIG:Debug>:-Og -g>
  )
elseif(KESTREL_SANITIZER STREQUAL "address")
  target_compile_options(kestrel_core PUBLIC
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
  )
  target_link_options(kestrel_core PUBLIC -fsanitize=address,undefined)
elseif(KESTREL_SANITIZER STREQUAL "thread")
  target_compile_options(kestrel_core PUBLIC
    -O1 -g -fsanitize=thread -fno-sanitize-recover=all
  )
  target_link_options(kestrel_core PUBLIC -fsanitize=thread)
elseif(KESTREL_SANITIZER STREQUAL "undefined")
  target_compile_options(kestrel_core PUBLIC
    -O1 -g -fsanitize=undefined -fno-sanitize-recover=all
  )
  target_link_options(kestrel_core PUBLIC -fsanitize=undefined)
endif()

message(STATUS "Kestrel ${PROJECT_VERSION}  march=${KESTREL_MARCH}  "
               "sanitizer='${KESTREL_SANITIZER}'  "
               "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

enable_testing()

# This WSL2 kernel places ASLR mappings inside the TSan shadow
# ("unexpected memory mapping"). ADDR_NO_RANDOMIZE is a loader flag,
# not a change to the instrumentation: the detector still sees the races.
set(KESTREL_TEST_WRAPPER "")
if(KESTREL_SANITIZER STREQUAL "thread")
  find_program(KESTREL_SETARCH setarch)
  if(KESTREL_SETARCH)
    set(KESTREL_TEST_WRAPPER ${KESTREL_SETARCH} ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
    message(STATUS "TSan tests run under setarch -R")
  else()
    message(WARNING "setarch not found; TSan may abort with 'unexpected memory mapping' on WSL2")
  endif()
endif()

function(kestrel_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE kestrel_core)
  if(KESTREL_TEST_WRAPPER)
    add_test(NAME ${name} COMMAND ${KESTREL_TEST_WRAPPER} $<TARGET_FILE:${name}>)
  else()
    add_test(NAME ${name} COMMAND ${name})
  endif()
endfunction()

kestrel_test(spsc_ring_test tests/spsc_ring_test.cpp)
kestrel_test(arena_slab_test tests/arena_slab_test.cpp)
kestrel_test(frame_test tests/frame_test.cpp)
kestrel_test(cpu_test tests/cpu_test.cpp)
