#!/bin/sh
# The loader's commands, on this computer (loader/test.c): the screen's rows, the sector map, the reset, and what the
# board asks of it. No toolchain: the host's C compiler.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Werror -Iloader/include loader/test.c loader/src/loader.c loader/src/core.c \
  -o build/loader-test
build/loader-test
