cmake_minimum_required(VERSION 3.20)
project(dsh-lite-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- OpenSSL (TLS for the Brain -> OpenRouter HTTPS path) ---
if(APPLE)
  # Homebrew OpenSSL 3 (LibreSSL headers in the SDK lack what httplib needs).
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/openssl@3")
  set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH "" FORCE)
endif()
find_package(OpenSSL REQUIRED)

# --- httplib + nlohmann/json: system first, FetchContent fallback ---
find_package(httplib QUIET)
if(NOT httplib_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(httplib)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(dshlite STATIC
  src/sanitizer.cpp
  src/spawner.cpp
  src/llm_client.cpp
  src/brain.cpp)
target_include_directories(dshlite PUBLIC include)
target_compile_options(dshlite PRIVATE -Wall -Wextra -Werror)
if(httplib_FOUND)
  target_link_libraries(dshlite PUBLIC httplib::httplib)
else()
  target_link_libraries(dshlite PUBLIC httplib)
endif()
target_link_libraries(dshlite PUBLIC nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
if(APPLE)
  # httplib links brotli/zlib + macOS system certs; PUBLIC so standalone
  # probes linking libdshlite.a get them transitively via CMake. (Manual
  # c++ ... libdshlite.a links need: -lbrotlicommon -lbrotlidec
  # -lbrotlienc -lz -framework Security -framework CoreFoundation
  # -framework SystemConfiguration.)
  find_library(BROTLI_COMMON_LIB brotlicommon REQUIRED)
  find_library(BROTLI_DEC_LIB brotlidec REQUIRED)
  find_library(BROTLI_ENC_LIB brotlienc REQUIRED)
  find_library(Z_LIB z REQUIRED)
  target_link_libraries(dshlite PUBLIC ${BROTLI_COMMON_LIB} ${BROTLI_DEC_LIB}
                                       ${BROTLI_ENC_LIB} ${Z_LIB})
  target_link_libraries(dshlite PUBLIC
    "-framework Security" "-framework CoreFoundation"
    "-framework SystemConfiguration")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(dshlite PUBLIC _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

add_executable(dsh-lite src/main.cpp)
target_link_libraries(dsh-lite PRIVATE dshlite)

enable_testing()
foreach(t IN ITEMS sanitizer spawner brain llm)
  add_executable(test-${t} tests/test_${t}.cpp)
  target_link_libraries(test-${t} PRIVATE dshlite)
  add_test(NAME test-${t} COMMAND test-${t})
endforeach()
add_executable(bench-sanitizer tests/bench_sanitizer.cpp)
target_link_libraries(bench-sanitizer PRIVATE dshlite)
add_test(NAME bench-sanitizer COMMAND bench-sanitizer)
