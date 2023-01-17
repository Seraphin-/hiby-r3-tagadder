# which compilers to use for C and C++
set(CMAKE_C_COMPILER   /usr/bin/mipsel-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/mipsel-linux-gnu-g++)

project(tagadder)
cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_EXE_LINKER_FLAGS "-static")
if(NOT BUILD_SHARED_LIBS)
  add_definitions(-DTAGLIB_STATIC)
endif()

# set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS "-g3")
set(WITH_ZLIB OFF)

# where is the target environment located
set(CMAKE_FIND_ROOT_PATH /home/s/hiby-firmware-tools/firmware-1.7-custom /usr/mipsel-linux-gnu)

add_subdirectory(taglib deps/taglib)
add_subdirectory(../tfblib deps/tfblib)
add_subdirectory(ssfn deps/ssfn)

# adjust the default behavior of the FIND_XXX() commands:
# search programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# search headers and libraries in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_library(sqlite3 STATIC ${CMAKE_CURRENT_SOURCE_DIR}/sqlite-autoconf-3400000/sqlite3.c)
target_link_libraries(sqlite3 dl)

include_directories(
  ${CMAKE_CURRENT_BINARY_DIR}/deps/taglib
  ${CMAKE_CURRENT_SOURCE_DIR}/taglib/taglib
  ${CMAKE_CURRENT_SOURCE_DIR}/taglib/taglib/toolkit
  ${CMAKE_CURRENT_SOURCE_DIR}/tfblib/include
  ${CMAKE_CURRENT_SOURCE_DIR}/ssfn
  ${CMAKE_CURRENT_SOURCE_DIR}/sqlite-autoconf-3400000
)

add_executable(tagadder tagadder.cpp semaphore.h)
target_link_libraries(tagadder tag tfb stdc++fs ssfn sqlite3)
find_package(Threads REQUIRED)
target_link_options(tagadder PUBLIC "-Wl,--whole-archive" "-lpthread" "-Wl,--no-whole-archive")
