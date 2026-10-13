#!/usr/bin/env bash
# Run real Ardour session tests using the dummy audio backend.
set -euo pipefail

cd "$(dirname "$0")/../.."
root=$(pwd -P)
mkdir -p build-evidence

build=false
mode=session
for argument in "$@"; do
    case "$argument" in
        --build) build=true ;;
        --debug) mode=debug ;;
        --exchange-only) mode=exchange ;;
        --baseline-only) mode=baseline ;;
        --logic) mode=logic ;;
        --all) mode=all ;;
        *) echo "Usage: $0 [--build] [--baseline-only|--exchange-only|--logic|--all|--debug]" >&2; exit 2 ;;
    esac
done
# Read only the configured output path; never execute the lock file, which
# also contains the configuring process's environment. Custom Waf output
# directories must not make this runner execute stale tests from ./build.
configured_build_dir() {
    python - <<'PY'
import ast
import pathlib
import sys
path = pathlib.Path('.lock-waf_' + sys.platform + '_build')
if not path.is_file():
    candidates = list(pathlib.Path('.').glob('.lock-waf_*_build'))
    if len(candidates) != 1:
        raise SystemExit('Cannot identify the configured Waf output directory')
    path = candidates[0]
for node in ast.parse(path.read_text()).body:
    if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and target.id == 'out_dir' for target in node.targets):
        print(pathlib.Path(ast.literal_eval(node.value)).resolve().as_posix())
        break
else:
    raise SystemExit('Waf output directory is missing')
PY
}
configured=$(configured_build_dir)
build_dir=${ARDOUR_TEST_BUILD_DIR:-$configured}
case "$(uname -s)" in MINGW*|MSYS*) build_dir=$(cygpath -u -- "$build_dir");; esac
build_dir=$(cd "$build_dir" && pwd -P)
libs="$build_dir/libs"
if $build; then
    if [[ -n ${ARDOUR_TEST_BUILD_DIR:-} && $configured != "$(python -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve().as_posix())' "$build_dir")" ]]; then
        echo 'Configure Waf for ARDOUR_TEST_BUILD_DIR before requesting a build.' >&2
        exit 1
    fi
    targets=unit-test-project-session,unit-test-session
    if [[ $mode == logic ]]; then
        targets=unit-test-logic-session
    fi
    if [[ $mode == all ]]; then
        targets=libardour-tests,libpbd-tests,libtemporal-tests
    fi
    python waf --targets="$targets,dummy_audiobackend,libardour_panbalance,libardour_pan1in2out,libardour_pan2in2out" \
        -j"${ARDOUR_BUILD_JOBS:-4}"
    if [[ $(configured_build_dir) != "$configured" ]]; then
        echo 'Waf output directory changed during the build; refusing to run unverified test binaries.' >&2
        exit 1
    fi
fi
echo "Session test binaries: $libs"

# This runner does not depend on the GUI wrapper being built. Keep its profile
# isolated from the user's installed DAW and retain it beside the test logs.
profile=$(mktemp -d "$root/build-evidence/session-profile.XXXXXX")
mkdir -p "$profile/cache" "$profile/no-surfaces" "$profile/no-vamp"
# Loading an old template can upgrade it in place. Use disposable fixtures.
cp -R "$root/libs/ardour/test/data" "$profile/test-data"
export XDG_CONFIG_HOME="$profile"
export XDG_CACHE_HOME="$profile/cache"
export ARDOUR_DLL_PATH="$libs"
export ARDOUR_CONFIG_PATH="$root:$root/gtk2_ardour:$build_dir:$build_dir/gtk2_ardour"
export ARDOUR_DATA_PATH="$root/share:$build_dir:$root/gtk2_ardour:$build_dir/gtk2_ardour"
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
    MINGW*|MSYS*)
        dependency_prefix=${ARDOUR_BUILD_ROOT:-"$root/../Ardour_Build"}/dependency-runtime/ucrt64
        if [[ ! -f "$dependency_prefix/bin/liblilv-0.dll" ]]; then
            echo 'Build the fixed LV2 runtime with tools/project-exchange/build-lilv-windows.sh first.' >&2
            exit 1
        fi
        export PATH="${library_path}:$dependency_prefix/bin:${PATH}"
        ;;
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

if [[ $mode == logic ]]; then
    "$libs/ardour/test_logic_session" 2>&1 | tee build-evidence/logic-session-tests.log
    exit 0
fi
if [[ $mode == session || $mode == baseline ]]; then
    "$libs/ardour/test_session" 2>&1 | tee build-evidence/baseline-session-tests.log
fi
if [[ $mode == baseline ]]; then
    exit 0
fi
"$libs/ardour/test_project_session" 2>&1 | tee build-evidence/project-session-tests.log
