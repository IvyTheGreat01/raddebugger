#!/bin/bash

set -xe

clang_common="-I../src/ -I../local/ -maes -mssse3 -msse4 -gcodeview -fdiagnostics-absolute-paths -Wall -Wno-missing-braces -Wno-unused-function -Wno-writable-strings -Wno-unused-value -Wno-unused-variable -Wno-unused-local-typedef -Wno-deprecated-register -Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-bitfield-constant-conversion -Xclang -flto-visibility-public-std -D_USE_MATH_DEFINES -Dstrdup=_strdup -Dgnu_printf=printf"

mkdir -p build
mkdir -p local

cd build
clang $clang_common -gdwarf-5 -g3 -O0 -D_DEBUG ../src/metagen/metagen_main.c -o metagen
./metagen
clang $clang_common -gdwarf-5 -g3 -O0 -D_DEBUG ../src/raddbg/raddbg.c -o raddbg
cd ..

