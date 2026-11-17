cmake_minimum_required(VERSION 3.20)

project(kestrel VERSION 0.4.0 LANGUAGES CXX)

# Phase 0 host this file was brought up on:
#   Intel Core i7-8650U, 4 physical cores / 8 logical, SMT pairs (0,1) (2,3) (4,5) (6,7)
#   L1d 32 KiB, L2 256 KiB, L3 8 MiB, cache line 64 B, no AVX-512
#   g++ 13.3, kernel 6.6 WSL2, liburing 2.5 linked when pkg-config finds it
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

set(KESTREL_MARCH native CACHE STRING "Argument to -march=. Container builds use x86-64-v3 or armv8-a+simd.")
option(KESTREL_STATIC "Link kestreld -static. For the musl Distroless image, not for sanitizers." OFF)
set(KESTREL_L2_BYTES 262144 CACHE STRING "Per-core L2 budget in bytes; rings larger than this fail to compile")
set(KESTREL_SANITIZER "" CACHE STRING "Sanitizer: empty, address, thread, or undefined")
set_property(CACHE KESTREL_SANITIZER PROPERTY STRINGS "" address thread undefined)

if(NOT KESTREL_SANITIZER STREQUAL ""
   AND NOT KESTREL_SANITIZER STREQUAL "address"
   AND NOT KESTREL_SANITIZER STREQUAL "thread"
   AND NOT KESTREL_SANITIZER STREQUAL "undefined")
  message(FATAL_ERROR "KESTREL_SANITIZER must be empty, address, thread, or undefined")
endif()
if(KESTREL_STATIC AND NOT KESTREL_SANITIZER STREQUAL "")
  message(FATAL_ERROR "KESTREL_STATIC cannot be combined with a sanitizer")
endif()
if(KESTREL_STATIC)
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
    OUTPUT_VARIABLE KESTREL_DUMPMACHINE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT KESTREL_DUMPMACHINE MATCHES "musl")
    message(WARNING
      "KESTREL_STATIC with ${KESTREL_DUMPMACHINE} links glibc. "
      "glibc warns about NSS in a static binary. The data plane calls inet_pton, not getaddrinfo. "
      "The Distroless static image is built with musl.")
  endif()
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBURING IMPORTED_TARGET QUIET liburing)
endif()
if(TARGET PkgConfig::LIBURING)
  message(STATUS "liburing ${LIBURING_VERSION} linked")
else()
  message(STATUS "liburing not found; the reactor uses epoll")
endif()

add_library(kestrel_core STATIC
  src/cpu.cpp
  src/hnsw.cpp
  src/wal.cpp
  src/feature_hash.cpp
  src/http.cpp
  src/server.cpp
  src/hash_ring.cpp
  src/swim.cpp
  src/core_mail.cpp
  src/metrics.cpp
)
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
if(TARGET PkgConfig::LIBURING)
  target_compile_definitions(kestrel_core PUBLIC KESTREL_HAS_LIBURING=1)
  if(KESTREL_STATIC)
    find_library(KESTREL_LIBURING_A NAMES liburing.a
      HINTS ${LIBURING_STATIC_LIBRARY_DIRS} ${LIBURING_LIBRARY_DIRS})
    if(NOT KESTREL_LIBURING_A)
      message(FATAL_ERROR "KESTREL_STATIC found liburing headers but not liburing.a. Install the static archive.")
    endif()
    target_link_libraries(kestrel_core PUBLIC "${KESTREL_LIBURING_A}")
  else()
    target_link_libraries(kestrel_core PUBLIC PkgConfig::LIBURING)
  endif()
endif()

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

# AArch64 GCC outlines atomics into libgcc helpers. A -static link of libstdc++
# does not always rescan libgcc for those calls. Inlining keeps the image linkable.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-mno-outline-atomics KESTREL_NO_OUTLINE_ATOMICS)
if(KESTREL_NO_OUTLINE_ATOMICS)
  target_compile_options(kestrel_core PUBLIC -mno-outline-atomics)
endif()

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
               "sanitizer='${KESTREL_SANITIZER}'  static=${KESTREL_STATIC}  "
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

add_executable(kestreld src/kestreld.cpp)
target_link_libraries(kestreld PRIVATE kestrel_core)
if(KESTREL_STATIC)
  target_link_options(kestreld PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(kestreld PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
  if(KESTREL_NO_OUTLINE_ATOMICS)
    target_link_libraries(kestreld PRIVATE gcc)
  endif()
endif()

kestrel_test(spsc_ring_test tests/spsc_ring_test.cpp)
kestrel_test(arena_slab_test tests/arena_slab_test.cpp)
kestrel_test(frame_test tests/frame_test.cpp)
kestrel_test(cpu_test tests/cpu_test.cpp)
kestrel_test(hnsw_test tests/hnsw_test.cpp)
kestrel_test(wal_test tests/wal_test.cpp)
kestrel_test(http_test tests/http_test.cpp)
kestrel_test(server_test tests/server_test.cpp)
kestrel_test(circuit_test tests/circuit_test.cpp)
kestrel_test(hash_ring_test tests/hash_ring_test.cpp)
kestrel_test(swim_test tests/swim_test.cpp)
kestrel_test(cluster_test tests/cluster_test.cpp)
kestrel_test(hdr_test tests/hdr_test.cpp)
kestrel_test(metrics_test tests/metrics_test.cpp)
