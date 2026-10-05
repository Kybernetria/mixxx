#!/usr/bin/env bash
set -euo pipefail

build_root=${1:?Usage: build_sanitizer_qsqlite.sh BUILD_ROOT}
qt_prefix=${QT_ROOT_DIR:?QT_ROOT_DIR must name the Qt 6.8.3 SDK}
source_dir="$build_root/source"
qtbase_revision=c07c2d5a527a644d36e7853d55132ae38921682f
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
system_pkgconfig_path="/usr/lib/$(gcc -dumpmachine)/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
sqlite_libdir=$(PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$system_pkgconfig_path" pkg-config --variable=libdir sqlite3)
sqlite_includedir=$(PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$system_pkgconfig_path" pkg-config --variable=includedir sqlite3)

mkdir -p "$source_dir"
git -C "$source_dir" init
git -C "$source_dir" fetch --depth 1 https://github.com/qt/qtbase.git "$qtbase_revision"
git -C "$source_dir" checkout --detach FETCH_HEAD
test "$(git -C "$source_dir" rev-parse HEAD)" = "$qtbase_revision"
test "$("$qt_prefix/bin/qmake" -query QT_VERSION)" = 6.8.3

"$qt_prefix/bin/qt-cmake" -S "$source_dir/src/plugins/sqldrivers" -B "$build_root/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX="$qt_prefix" \
    -DFEATURE_system_sqlite=ON \
    -DFEATURE_sql_sqlite=ON \
    -DFEATURE_sql_db2=OFF -DFEATURE_sql_ibase=OFF -DFEATURE_sql_mysql=OFF \
    -DFEATURE_sql_oci=OFF -DFEATURE_sql_odbc=OFF -DFEATURE_sql_psql=OFF \
    -DFEATURE_sql_mimer=OFF \
    -DSQLite3_INCLUDE_DIR:PATH="$sqlite_includedir" \
    -DSQLite3_LIBRARY:FILEPATH="$sqlite_libdir/libsqlite3.so" \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS_DEBUG=-g1 -DCMAKE_C_FLAGS_DEBUG=-g1 \
    -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined \
    -DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build "$build_root/build" --parallel 2 --target QSQLiteDriverPlugin

mapfile -t built_plugins < <(find "$build_root/build" -type f -name libqsqlite.so)
test "${#built_plugins[@]}" -eq 1
install -m 755 "${built_plugins[0]}" "$qt_prefix/plugins/sqldrivers/libqsqlite.so"
plugin_dependencies=$(readelf --dynamic "$qt_prefix/plugins/sqldrivers/libqsqlite.so")
[[ "$plugin_dependencies" == *'Shared library: [libsqlite3.so.'* ]]

g++ -fPIC -g1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$script_dir/sanitizer_qsqlite_probe.cpp" \
    -I"$qt_prefix/include" -I"$qt_prefix/include/QtCore" -I"$qt_prefix/include/QtSql" \
    -I"$sqlite_includedir" -L"$qt_prefix/lib" -L"$sqlite_libdir" \
    -Wl,-rpath,"$qt_prefix/lib" -lQt6Core -lQt6Sql -lsqlite3 \
    -o "$build_root/qsqlite-handle-probe"
ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    "$build_root/qsqlite-handle-probe"
