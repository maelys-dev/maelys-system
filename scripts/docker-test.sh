#!/bin/sh
set -eu

image=maelys-system-test:local
docker build -t "$image" -f Dockerfile.test .
docker run --rm "$image" sh -lc 'make clean check CC=gcc CXX=g++'
docker run --rm "$image" sh -lc 'make clean check CC=clang CXX=clang++'
docker run --rm "$image" sh -lc 'make clean asan-ubsan CC=clang CXX=clang++'
docker run --rm "$image" sh -lc 'make clean tsan CC=clang CXX=clang++'
