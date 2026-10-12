#!/usr/bin/env bash
# Run real Ardour session tests using the dummy audio backend.
set -euo pipefail

cd "$(dirname "$0")/../.."
root=$(pwd -P)
libs="$root/build/libs"
mkdir -p build-evidence

build=false
mode=session
for argument in "$@"; do
    case "$argument" in
        --build) build=true ;;
        --debug) mode=debug ;;
        --exchange-only) mode=exchange ;;
        --all) mode=all ;;
        *) echo "Usage: $0 [--build] [--exchange-only|--all|--debug]" >&2; exit 2 ;;
    esac
done
if $build; then
    targets=unit-test-project-session,unit-test-session
    if [[ $mode == all ]]; then
        targets=libardour-tests,libpbd-tests,libtemporal-tests
    fi
    python waf --targets="$targets,dummy_audiobackend,libardour_panbalance,libardour_pan1in2out,libardour_pan2in2out" \
        -j"${ARDOUR_BUILD_JOBS:-4}"
fi

# This runner does not depend on the GUI wrapper being built. Keep its profile
# isolated from the user's installed DAW and retain it beside the test logs.
profile=$(mktemp -d "$root/build-evidence/session-profile.XXXXXX")
mkdir -p "$profile/cache" "$profile/no-surfaces" "$profile/no-vamp"
# Loading an old template can upgrade it in place. Use disposable fixtures.
cp -R "$root/libs/ardour/test/data" "$profile/test-data"
export XDG_CONFIG_HOME="$profile"
export XDG_CACHE_HOME="$profile/cache"
export ARDOUR_DLL_PATH="$libs"
export ARDOUR_CONFIG_PATH="$root:$root/gtk2_ardour:$root/build:$root/build/gtk2_ardour"
export ARDOUR_DATA_PATH="$root/share:$root/build:$root/gtk2_ardour:$root/build/gtk2_ardour"
export ARDOUR_BACKEND_PATH="$libs/backends/dummy"
export ARDOUR_PANNER_PATH="$libs/panners"
export ARDOUR_TEST_PATH="$profile/test-data"
export PBD_TEST_PATH="$root/libs/pbd/test"
export EVORAL_TEST_PATH="$root/libs/evoral/test/testdata"
export MIDIPP_TEST_PATH="$root/share/patchfiles"
export ARDOUR_SURFACES_PATH="$profile/no-surfaces"
export VAMP_PATH="$profile/no-vamp"

library_path=""
while IFS= read -r -d '' library; do
    directory=${library%/*}
    case ":$library_path:" in
        *":$directory:"*) ;;
        *) library_path+="${library_path:+:}${directory}" ;;
    esac
done < <(find "$libs" -type f \( -name '*.dll' -o -name '*.so' -o -name '*.so.*' -o -name '*.dylib' \) -print0)

case "$(uname -s)" in
    MINGW*|MSYS*) export PATH="${library_path}:${PATH}" ;;
    Darwin*) export DYLD_FALLBACK_LIBRARY_PATH="${library_path}${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}" ;;
    *) export LD_LIBRARY_PATH="${library_path}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac

if [[ $mode == debug ]]; then
    # Match normal Windows heap behavior while collecting crash stacks.
    export _NO_DEBUG_HEAP=1
    gdb --batch --return-child-result -ex run -ex 'thread apply all bt' --args "$libs/ardour/test_project_session" \
        2>&1 | tee build-evidence/project-session-debug.log
    exit 0
fi

if [[ $mode == all ]]; then
    result=0
    for suite in pbd temporal ardour; do
        if (cd "$libs/$suite" && ./run-tests) 2>&1 | tee "build-evidence/$suite-tests.log"; then
            :
        else
            result=1
        fi
    done
    exit "$result"
fi

if [[ $mode == session ]]; then
    "$libs/ardour/test_session" 2>&1 | tee build-evidence/baseline-session-tests.log
fi
"$libs/ardour/test_project_session" 2>&1 | tee build-evidence/project-session-tests.log
