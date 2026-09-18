cmake_minimum_required(VERSION 3.16)
project(cpp-harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# System libgit2 via pkg-config (apt libgit2-dev on enigma, brew on macOS).
# Pass -DHARNESS_USE_LIBGIT2=ON to link; when absent/OFF the build falls
# back to shell-git with identical GateCode behavior.
option(HARNESS_USE_LIBGIT2 "Use libgit2 for worktree add/diff (fallback: shell git)" OFF)

add_library(harness_middleware STATIC
  src/middleware.cpp
  src/sandbox.cpp
  src/sister_shm.cpp
  src/git_native.cpp
  src/harness_c.cpp
)
target_include_directories(harness_middleware PUBLIC include)
target_compile_options(harness_middleware PRIVATE -Wall -Wextra -Werror)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # glibc hides shm_unlink/waitpid under strict -std=c++20 (__STRICT_ANSI__)
  # unless feature macros are defined before ANY system header. PUBLIC so
  # harness-demo/harnessd inherit regardless of their include order.
  target_compile_definitions(harness_middleware PUBLIC
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
  target_link_libraries(harness_middleware PUBLIC pthread rt)
endif()

if(HARNESS_USE_LIBGIT2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBGIT2 REQUIRED libgit2)
  target_compile_definitions(harness_middleware PUBLIC HARNESS_USE_LIBGIT2)
  # pkg-config --cflags --libs libgit2: shared link against system libgit2.
  target_include_directories(harness_middleware PUBLIC ${LIBGIT2_INCLUDE_DIRS})
  target_compile_options(harness_middleware PUBLIC ${LIBGIT2_CFLAGS_OTHER})
  target_link_directories(harness_middleware PUBLIC ${LIBGIT2_LIBRARY_DIRS})
  target_link_libraries(harness_middleware PUBLIC ${LIBGIT2_LIBRARIES})
endif()

add_executable(harness-demo src/main.cpp)
target_link_libraries(harness-demo PRIVATE harness_middleware)

add_executable(harnessd src/harnessd.cpp)
target_link_libraries(harnessd PRIVATE harness_middleware)

enable_testing()
add_test(NAME harness-demo COMMAND harness-demo ${CMAKE_SOURCE_DIR}/../..)
