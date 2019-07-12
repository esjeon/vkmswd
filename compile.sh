#!/bin/sh

set -x

CC=clang
CFLAGS="-Wall -DDEBUG=1 $(pkg-config --cflags libevdev)"
LDFLAGS="-ggdb $(pkg-config --libs libevdev)"

${CC} ${CFLAGS} ${LDFLAGS} vkmswd.c -o vkmswd
