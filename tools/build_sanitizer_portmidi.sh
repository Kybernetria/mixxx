#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?Usage: build_sanitizer_portmidi.sh INSTALL_PREFIX}
build_root="${prefix}-build"
source_dir="$build_root/source"
archive="$build_root/portmidi-2.0.7.tar.gz"
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$source_dir"

curl --fail --location --retry 3 \
    https://github.com/PortMidi/portmidi/archive/refs/tags/v2.0.7.tar.gz \
    --output "$archive"
printf '%s  %s\n' \
    43fa65b4ed7ebaa68b0028a538ba8b2ca4dc9b86a7e22ec48842070e010f823f \
    "$archive" | sha256sum --check --strict
tar --extract --gzip --file "$archive" --directory "$source_dir" --strip-components=1

cmake -S "$source_dir" -B "$build_root/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_PORTMIDI_TESTS=OFF \
    -DBUILD_JAVA_NATIVE_INTERFACE=OFF \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS_DEBUG=-g1 \
    -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build "$build_root/build" --parallel 2
cmake --install "$build_root/build"

gcc -g1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$script_dir/portmidi_lifetime_probe.c" \
    -I"$prefix/include" -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    -lportmidi -o "$build_root/portmidi-lifetime-probe"
ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    "$build_root/portmidi-lifetime-probe"
