#!/bin/bash
# Cross-compiles PDI.plg (32-bit) and PDI.plg64 (64-bit) with llvm-mingw on Linux.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TC=$(echo /home/claude/tc/llvm-mingw-*)
ZSTD=/home/claude/src/zstd/lib
OUT="${OUT:-$HERE/out}"
ZSRC="$ZSTD/common/debug.c $ZSTD/common/entropy_common.c $ZSTD/common/error_private.c $ZSTD/common/fse_decompress.c $ZSTD/common/xxhash.c $ZSTD/common/zstd_common.c $ZSTD/decompress/huf_decompress.c $ZSTD/decompress/zstd_ddict.c $ZSTD/decompress/zstd_decompress.c $ZSTD/decompress/zstd_decompress_block.c"
CFLAGS="-O2 -Wall -Wno-unused-function -ffreestanding -fno-stack-protector -fno-math-errno -fno-asynchronous-unwind-tables -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -DDYNAMIC_BMI2=0 -DZSTD_STRIP_ERROR_STRINGS=1 -DNDEBUG -I$ZSTD -I$ZSTD/common -I$HERE/src $EXTRA_CFLAGS"

build() { # arch triple ext builtins
	local arch=$1 triple=$2 ext=$3 rt=$4 subsys=$5 dir="$OUT/$1"
	rm -rf "$dir"; mkdir -p "$dir/obj"
	for f in $HERE/src/pdi_core.c $HERE/src/pdi_plugin.c $ZSRC; do
		"$TC/bin/clang" -target $triple $CFLAGS -c "$f" -o "$dir/obj/$(basename "$f" .c).o"
	done
	"$TC/bin/clang" -target $triple $CFLAGS -fno-builtin -c "$HERE/src/rt.c" -o "$dir/obj/rt.o"
	"$TC/bin/clang" -target $triple -nostdlib -shared -fuse-ld=lld -s \
		-Wl,--kill-at -Wl,--subsystem,windows:$subsys -Wl,--major-os-version=5 -Wl,--minor-os-version=1 -Wl,--file-alignment,512 \
		-o "$dir/PDI.$ext" "$dir"/obj/*.o "$TC/lib/clang/19/lib/windows/$rt" \
		-lkernel32 -luser32 -lgdi32
}
build x86 i686-w64-mingw32 plg libclang_rt.builtins-i386.a 5.01
build x64 x86_64-w64-mingw32 plg64 libclang_rt.builtins-x86_64.a 5.02
ls -la "$OUT/x86/PDI.plg" "$OUT/x64/PDI.plg64"
