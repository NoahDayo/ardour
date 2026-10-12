#!/usr/bin/env bash
# Lilv 0.26.4 overwrites the Windows heap when an LV2 world has no subclasses.
# Build the fixed release in an isolated prefix without replacing MSYS packages.
set -euo pipefail

if [[ ${MSYSTEM:-} != UCRT64 ]]; then
    echo 'Use an MSYS2 UCRT64 shell.' >&2
    exit 1
fi
root=$(cd "$(dirname "$0")/../.." && pwd -P)
private=${ARDOUR_BUILD_ROOT:-"$root/../Ardour_Build"}
mkdir -p "$private/source-deps" "$private/dependency-runtime/ucrt64"
private=$(cd "$private" && pwd -P)
prefix="$private/dependency-runtime/ucrt64"
source="$private/source-deps/lilv-0.28.0"
build="$private/source-deps/lilv-0.28.0-build"
commit=2feec6a15d3300db3ac0337bafb863308a03cab6

if [[ ! -d "$source" ]]; then
    git clone --config core.autocrlf=false --depth 1 --branch v0.28.0 https://github.com/lv2/lilv.git "$source"
fi
if [[ $(git -C "$source" rev-parse HEAD) != "$commit" || -n $(git -C "$source" status --porcelain) ]]; then
    echo "Expected a clean Lilv v0.28.0 checkout at $source" >&2
    exit 1
fi

setup=()
if [[ -f "$build/meson-private/coredata.dat" ]]; then
    setup+=(--reconfigure)
fi
meson setup "${setup[@]}" "$build" "$source" \
    --prefix="$(cygpath -m "$prefix")" --libdir=lib \
    --buildtype=debugoptimized --default-library=shared --wrap-mode=nofallback \
    -Ddocs=disabled -Dbindings_py=disabled -Dbindings_cpp=disabled \
    -Dtests=enabled -Dtools=disabled
meson compile -C "$build" -j "${ARDOUR_BUILD_JOBS:-4}"
meson test -C "$build" --print-errorlogs
meson install -C "$build"
mkdir -p "$prefix/share/licenses/lilv-0.28.0"
cp "$source/COPYING" "$prefix/share/licenses/lilv-0.28.0/COPYING"
printf '%s\n' "$commit" > "$prefix/share/licenses/lilv-0.28.0/source-commit.txt"
echo "Fixed Lilv runtime installed at $prefix"
