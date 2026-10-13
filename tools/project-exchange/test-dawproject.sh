#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build-evidence
platform_flags=()
case "${MSYSTEM:-}" in UCRT64|MINGW64|CLANG64) platform_flags+=(-municode);; esac
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror "${platform_flags[@]}" \
    -Ilibs/ardour libs/ardour/project_exchange.cc libs/ardour/project_archive.cc libs/ardour/dawproject.cc libs/ardour/studio_one_song.cc libs/ardour/studio_one_song_writer.cc \
    tools/project-exchange/tests/dawproject_probe.cc \
    $(pkg-config --cflags --libs gio-2.0 libarchive libxml-2.0 sndfile) -o build-evidence/dawproject-probe.exe
"${DAWPROJECT_TEST_PYTHON:-python}" tools/project-exchange/tests/test_dawproject.py \
    --probe build-evidence/dawproject-probe.exe "$@" 2>&1 | tee build-evidence/dawproject-tests.log
"${DAWPROJECT_TEST_PYTHON:-python}" tools/project-exchange/tests/test_studio_one.py \
    --probe build-evidence/dawproject-probe.exe 2>&1 | tee build-evidence/native-song-tests.log
