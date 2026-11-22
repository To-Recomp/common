cmake_minimum_required(VERSION 3.20)
project(torecomp_common LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(torecomp_common STATIC
  src/sha1.cpp
  src/gcm.cpp
  src/disc_source.cpp
  src/disc_wia.cpp
  src/codecs.cpp
  src/content.cpp
  src/hostpath.cpp
  src/dumps.cpp)
target_include_directories(torecomp_common PUBLIC include)
target_include_directories(torecomp_common PRIVATE third_party/tomlplusplus src)

# Decompression libraries for compressed disc images. Each one is optional at configure time: a
# missing library only disables the formats that need it (reading such an image then fails with
# an error naming the library). Set -DTORECOMP_REQUIRE_CODECS=ON to make a missing one fatal.
#   GCZ: zlib   WIA: bzip2, liblzma   RVZ: zstd (and bzip2, liblzma)   CISO: nothing
option(TORECOMP_REQUIRE_CODECS "Fail configuration when a decompression library is missing" OFF)
function(torecomp_codec name define)
  if(${name}_FOUND)
    target_compile_definitions(torecomp_common PUBLIC ${define})
    foreach(lib ${ARGN})
      target_link_libraries(torecomp_common PUBLIC ${lib})
    endforeach()
    message(STATUS "torecomp: ${name} found, enabled")
  elseif(TORECOMP_REQUIRE_CODECS)
    message(FATAL_ERROR "torecomp: ${name} not found (TORECOMP_REQUIRE_CODECS=ON). "
                        "Install its development package (e.g. zlib1g-dev libbz2-dev liblzma-dev libzstd-dev).")
  else()
    message(WARNING "torecomp: ${name} not found; images that need it cannot be read")
  endif()
endfunction()

find_package(ZLIB QUIET)
find_package(BZip2 QUIET)
find_package(LibLZMA QUIET)
find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd zstd_static libzstd libzstd_static)
if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
  set(ZSTD_FOUND TRUE)
  add_library(torecomp_zstd INTERFACE)
  target_include_directories(torecomp_zstd INTERFACE ${ZSTD_INCLUDE_DIR})
  target_link_libraries(torecomp_zstd INTERFACE ${ZSTD_LIBRARY})
endif()
torecomp_codec(ZLIB TORECOMP_HAVE_ZLIB ZLIB::ZLIB)
torecomp_codec(BZip2 TORECOMP_HAVE_BZIP2 BZip2::BZip2)
torecomp_codec(LibLZMA TORECOMP_HAVE_LZMA LibLZMA::LibLZMA)
torecomp_codec(ZSTD TORECOMP_HAVE_ZSTD torecomp_zstd)
if(MSVC)
  target_compile_options(torecomp_common PRIVATE /W4)
else()
  target_compile_options(torecomp_common PRIVATE -Wall -Wextra)
endif()

add_executable(torecomp-disc tools/disc_tool.cpp)
target_link_libraries(torecomp-disc PRIVATE torecomp_common)

include(CTest)
if(BUILD_TESTING)
  add_executable(torecomp_tests tests/test_main.cpp)
  target_link_libraries(torecomp_tests PRIVATE torecomp_common)
  add_test(NAME torecomp_tests COMMAND torecomp_tests)
endif()
