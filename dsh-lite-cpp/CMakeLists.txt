cmake_minimum_required(VERSION 3.20)
project(golem LANGUAGES CXX)

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
  src/grammar.cpp
  src/router.cpp
  src/ledger.cpp
  src/usage_probe.cpp
  src/nudge.cpp
  src/payload_gate.cpp
  src/postcondition.cpp
  src/compaction.cpp
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

# Gap 4 driver: live uncapped run against a local coli engine (not a ctest
# member — needs a running engine; invoked manually, ledger to /tmp).
add_executable(g4-run tests/g4_run.cpp)
target_link_libraries(g4-run PRIVATE dshlite)

# bench/h2h side A: dsh-lite-cpp vs smolagents head-to-head runner (not a
# ctest member — needs the referee proxy + live engine; see bench/h2h).
add_executable(h2h-ours tests/h2h_ours.cpp)
target_link_libraries(h2h-ours PRIVATE dshlite)

# C ABI feasibility spike: in-process greedy decode through Colibri's
# public segment/edge C ABI (libcolibri_segment_edge.a) — no coli serve,
# no Python, no HTTP. Not a ctest member: needs the real OLMoE model dir.
# Build the library first:  make -C ../colibri/c segment-edge-library
set(COLIBRI_C_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../colibri/c"
    CACHE PATH "Colibri C engine source dir (headers + build/segment)")
set(COLIBRI_SEGMENT_LIB "${COLIBRI_C_DIR}/build/segment/libcolibri_segment_edge.a")
if(EXISTS "${COLIBRI_SEGMENT_LIB}")
  add_executable(abi-probe src/abi_probe.cpp)
  target_include_directories(abi-probe PRIVATE "${COLIBRI_C_DIR}")
  target_compile_options(abi-probe PRIVATE -Wall -Wextra -Werror)
  find_library(OMP_LIB omp HINTS /opt/homebrew/opt/libomp/lib)
  target_link_libraries(abi-probe PRIVATE "${COLIBRI_SEGMENT_LIB}" m)
  if(OMP_LIB)
    target_link_libraries(abi-probe PRIVATE "${OMP_LIB}")
  endif()

  # Native C ABI backend (AbiClient : ILlmPoster) — separate library so
  # core dshlite never gains a Colibri link dependency. Same flags law.
  add_library(dshlite-abi STATIC src/abi_client.cpp)
  target_include_directories(dshlite-abi PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${COLIBRI_C_DIR}")
  target_compile_options(dshlite-abi PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(dshlite-abi PUBLIC dshlite "${COLIBRI_SEGMENT_LIB}" m)
  if(OMP_LIB)
    target_link_libraries(dshlite-abi PUBLIC "${OMP_LIB}")
  endif()
else()
  message(STATUS "abi-probe disabled: ${COLIBRI_SEGMENT_LIB} not built yet "
                 "(make -C colibri/c segment-edge-library)")
endif()

enable_testing()
foreach(t IN ITEMS sanitizer spawner brain llm router ledger nudge stall f33_p1 g4_stress grammar verify_promotion)
  add_executable(test-${t} tests/test_${t}.cpp)
  target_link_libraries(test-${t} PRIVATE dshlite)
  add_test(NAME test-${t} COMMAND test-${t})
endforeach()
if(TARGET dshlite-abi)
  add_executable(test-abi tests/test_abi.cpp)
  target_link_libraries(test-abi PRIVATE dshlite-abi)
  # Offline invariants always; live engine section only via ABI_MODEL_DIR
  # (ctest stays hermetic — no model dir in the default environment).
  add_test(NAME test-abi COMMAND test-abi)

  # In-process vs HTTP lane bench (manual, not ctest: needs the model +
  # a serve engine for the http arm; runs sequentially per F70).
  add_executable(abi-bench tests/abi_bench.cpp)
  target_link_libraries(abi-bench PRIVATE dshlite-abi)

  # Mixed-lane production run driver (manual: ABI brain + HTTP workers).
  add_executable(mixed-lane-run tests/mixed_lane_run.cpp)
  target_link_libraries(mixed-lane-run PRIVATE dshlite-abi)
endif()
add_executable(bench-sanitizer tests/bench_sanitizer.cpp)
target_link_libraries(bench-sanitizer PRIVATE dshlite)
add_test(NAME bench-sanitizer COMMAND bench-sanitizer)
