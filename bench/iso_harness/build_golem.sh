#!/usr/bin/env bash
# build_golem.sh — compile golem_runner.cpp against the existing
# dsh-lite-cpp build tree (reuses its include dirs + libdshlite.a).
# The core CMake tree is NOT touched (bench hygiene): the runner is
# compiled in-place with the same warning law (-Wall -Wextra -Werror).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
CPP="$REPO/dsh-lite-cpp"
BUILD="$CPP/build"

[ -f "$BUILD/libdshlite.a" ] || { echo "no $BUILD/libdshlite.a — run cmake --build build in dsh-lite-cpp first"; exit 1; }

# Resolve include dirs from the existing CMake build tree (no machine-
# specific hardcoding): nlohmann_json_DIR from the cache, httplib from
# FetchContent _deps.
CACHE="$BUILD/CMakeCache.txt"
JSON_INC=""
if [ -f "$CACHE" ]; then
  NJ_DIR=$(grep '^nlohmann_json_DIR' "$CACHE" | cut -d= -f2 || true)
  # <prefix>/share/cmake/nlohmann_json -> <prefix>/include
  if [ -n "${NJ_DIR:-}" ]; then
    cand="$(cd "$NJ_DIR/../../.." 2>/dev/null && pwd)/include"
    [ -f "$cand/nlohmann/json.hpp" ] && JSON_INC="$cand"
  fi
fi
[ -n "$JSON_INC" ] || JSON_INC="/opt/homebrew/include"
HTTPLIB_INC="$BUILD/_deps/httplib-src"
OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)"

INCS=(-I"$CPP/include")
[ -f "$JSON_INC/nlohmann/json.hpp" ] && INCS+=(-isystem "$JSON_INC")
[ -f "$HTTPLIB_INC/httplib.h" ] && INCS+=(-isystem "$HTTPLIB_INC")
INCS+=(-isystem "$OPENSSL_PREFIX/include")

c++ -std=c++20 -O2 -Wall -Wextra -Werror "${INCS[@]}" \
    "$HERE/golem_runner.cpp" \
    "$BUILD/libdshlite.a" \
    -L"$OPENSSL_PREFIX/lib" -L/opt/homebrew/lib -lssl -lcrypto \
    -lbrotlicommon -lbrotlidec -lbrotlienc -lz \
    -framework Security -framework CoreFoundation -framework SystemConfiguration \
    -o "$HERE/golem-runner"
echo "built $HERE/golem-runner"
