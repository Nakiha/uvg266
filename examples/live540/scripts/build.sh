#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
dep=../..
obj=build/uvg266
cmake -S "$dep" -B "$obj" -DCMAKE_BUILD_TYPE=Release -DUVG_BIT_DEPTH=8 \
    -DUVG_SYNC_SUBMODULES=OFF -DUVG_BUILD_TESTS=OFF -DUVG_BUILD_EXECUTABLE=OFF
cmake --build "$obj" --target libuvg266_static --parallel "${JOBS:-4}"
"${CC:-cc}" -O2 -std=c11 -Wall -Wextra -DUVG_BIT_DEPTH=8 -DUVG_STATIC_LIB \
    -I"$dep/src" src/live.c "$obj/lib/libuvg266.a" -pthread -lm -o build/live266
