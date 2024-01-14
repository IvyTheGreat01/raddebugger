#!/bin/bash

# --- Unpack Arguments ---------
raddbg=0
clang=0
gcc=0
debug=0
release=0
telemetry=0
asan=0
nometa=0

arg_count=$#
while [[ $# -gt 0 ]]; do
    case $1 in
        raddbg)            raddbg=1;;
        clang)             clang=1;;
        gcc)               gcc=1;;
        debug)             debug=1;;
        release)           release=1;;
        telemetry)         telemetry=1;;
        asan)              asan=1;;
        nometa)            nometa=1;;
    esac
    shift
done

if [ $clang -ne 1 ] && [ $gcc -ne 1 ]; then clang=1; fi
if [ $release -ne 1 ];   then debug=1; fi
if [ $debug -eq 1 ];     then release=0; echo "[debug mode]"; fi
if [ $release -eq 1 ];   then debug=0;   echo "[release mode]"; fi
if [ $clang -eq 1 ];     then gcc=0;     echo "[clang compile]"; fi
if [ $gcc -eq 1 ];       then clang=0;   echo "[gcc compile]"; fi
if [ $arg_count -eq 0 ]; then raddbg=1;  echo "[default mode, assuming 'raddbg' build]"; fi

# --- Unpack Command Line Build Arguments --------
auto_compile_flags=""
if [ $telemetry -eq 1 ]; then auto_compile_flags="$auto_compile_flags -DPROFILE_TELEMETRY=1"; echo "[telemetry profiling enabled]"; fi
if [ $asan -eq 1 ]; then auto_compile_flags="$auto_compile_flags -fsantize=address"; echo "[asan enabled]"; fi

# --- Compile/Link Line Definitions ----------
clang_common="-I../src/ -I../local/ -maes -mssse3 -msse4 -gcodeview -fdiagnostics-absolute-paths -Wall -Wno-missing-braces -Wno-unused-function -Wno-writable-strings -Wno-unused-value -Wno-unused-variable -Wno-unused-local-typedef -Wno-deprecated-register -Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-bitfield-constant-conversion -Xclang -flto-visibility-public-std -D_USE_MATH_DEFINES -Dstrdup=_strdup -Dgnu_printf=printf"
gcc_common="-I../src/ -I../local/ -maes -mssse3 -msse4 -gcodeview -fdiagnostics-absolute-paths -Wall -Wno-missing-braces -Wno-unused-function -Wno-writable-strings -Wno-unused-value -Wno-unused-variable -Wno-unused-local-typedef -Wno-deprecated-register -Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-bitfield-constant-conversion -Xclang -flto-visibility-public-std -D_USE_MATH_DEFINES -Dstrdup=_strdup -Dgnu_printf=printf"

clang_debug="clang -gdwarf-5 -g3 -O0 -D_DEBUG $clang_common"
clang_release="clang -gdwarf-5 -g3 -O3 -DNDEBUG $clang_common"
clang_link="-Xlinker /natvis:${pwd}/src/natvis/base.natvis"

gcc_debug="gcc -gdwarf-5 -g3 -O0 -D_DEBUG $gcc_common"
gcc_release="gcc -gdwarf-5 -g3 -O3 -DNDEBUG $gcc_common"
gcc_link="-Xlinker /natvis:${pwd}/src/natvis/base.natvis"

# --- Per-Build Settings -------
gfx="-DOS_FEATURE_GRAPHICAL=1"
net="-DOS_FEATURE_SOCKET=1"

# --- Choose Compile/Link Lines -----
compile_debug=""
compile_release=""
compile_link=""
if [ $clang -eq 1 ];   then compile_debug=$clang_debug; fi
if [ $clang -eq 1 ];   then compile_release=$clang_release; fi
if [ $clang -eq 1 ];   then compile_link=$clang_link; fi
if [ $gcc -eq 1 ];     then compile_debug=$gcc_debug; fi
if [ $gcc -eq 1 ];     then compile_release=$gcc_release; fi
if [ $gcc -eq 1 ];     then compile_link=$gcc_link; fi
if [ $debug -eq 1 ];   then compile=$compile_debug; fi
if [ $release -eq 1 ]; then compile=$compile_release; fi
compile="$compile $auto_compile_flags"

# --- Prep Directories ----
mkdir -p build
mkdir -p local

# --- Build & Run Metaprogram -----
if [ $nometa -eq 1 ]; then
    echo "[skipping metagen]";
else
    cd build;
    $compile_debug ../src/metagen/metagen_main.c $compile_link -o metagen;
    cd ..;
fi

# --- Build Everything (@build_targets) -----
cd build
if [ $raddbg -eq 1 ]; then $compile $gfx ../src/raddbg/raddbg_main.cpp $compile_link -o raddbg; fi
cd ..

