#!/usr/bin/env bash
# Install requirements-test.txt into a test-only Python environment first.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build-evidence
"${CC:-gcc}" -std=c11 -O1 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=500 \
    -Ilibs/aaf libs/aaf/*.c tools/project-exchange/tests/aaf_probe.c \
    -lm -o build-evidence/aaf-probe.exe
"${AAF_TEST_PYTHON:-python}" tools/project-exchange/tests/test_aaf_writer.py \
    --probe build-evidence/aaf-probe.exe 2>&1 | tee build-evidence/aaf-writer-tests.log
