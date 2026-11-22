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
  src/content.cpp
  src/dumps.cpp)
target_include_directories(torecomp_common PUBLIC include)
target_include_directories(torecomp_common PRIVATE third_party/tomlplusplus)
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
