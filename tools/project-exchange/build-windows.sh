#!/usr/bin/env bash
# Run inside an MSYS2 UCRT64 login shell. Keep generated evidence outside Git.
set -euo pipefail

if [[ ${MSYSTEM:-} != UCRT64 ]]; then
    echo 'Use an MSYS2 UCRT64 shell.' >&2
    exit 1
fi

cd "$(dirname "$0")/../.."
root=$(pwd -P)
bash tools/project-exchange/build-lilv-windows.sh
dependency_prefix=${ARDOUR_BUILD_ROOT:-"$root/../Ardour_Build"}/dependency-runtime/ucrt64
export PKG_CONFIG_PATH="$dependency_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH="$dependency_prefix/bin:$PATH"
mkdir -p build-evidence
pacman -Q > build-evidence/msys2-packages.txt
git rev-parse HEAD > build-evidence/source-commit.txt
git status --short > build-evidence/source-changes.txt
git diff --stat HEAD >> build-evidence/source-changes.txt
/ucrt64/bin/gcc --version > build-evidence/compiler.txt

# -Og also keeps large debug translation units within MinGW object limits.
export CFLAGS='-Og -mstackrealign'
# LRDF 0.5.0's MSYS2 pkg-config file embeds a non-relocatable /ucrt64 path.
# Raptor's own package file supplies the native include path required by GCC.
export CXXFLAGS="-Og -mstackrealign $(pkg-config --cflags raptor2)"
/ucrt64/bin/python waf configure \
    --check-c-compiler=gcc --check-cxx-compiler=g++ \
    --dist-target=mingw --configdir=share \
    --no-execstack --no-dr-mingw --no-phone-home \
    --cxx17 --keepflags --ptformat --test --single-tests \
    --with-backends=portaudio,dummy "$@" 2>&1 | tee build-evidence/configure.log

/ucrt64/bin/python waf -j"${ARDOUR_BUILD_JOBS:-4}" 2>&1 | tee build-evidence/build.log

# Isolate user configuration and mutable fixtures, and propagate every suite's
# exit status. The runner sets native DLL paths without relying on the GUI.
bash tools/project-exchange/test-session.sh --all
