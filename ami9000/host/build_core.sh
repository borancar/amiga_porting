#!/bin/sh
# Build Engine9000's PUAE core with the host patches in patches/ami9000-*.patch into
# build/system/ami9000.so. The pinned engine9000-src is left untouched:
# the build runs in build/e9k, a patched copy. Needs about a minute on 12 cores.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
# AMI9000_PATCHES: the patch files to apply (default: those listed in patches/series, in order); AMI9000_OUT: the library to write
# (default build/system/ami9000.so).  A subset builds a separate core, e.g. one with the custom-write observer.
patches=${AMI9000_PATCHES:-$(sed "s#^#$root/patches/#" "$root/patches/series")}
out=${AMI9000_OUT:-$root/build/system/ami9000.so}
work="$root/build/e9k${AMI9000_TAG:+-$AMI9000_TAG}"
rm -rf "$work"
mkdir -p "$work"
cp -r "$root/engine9000-src/ami9000" "$work/ami9000"
cp -r "$root/engine9000-src/e9k-lib" "$work/e9k-lib"
for p in $patches; do
    (cd "$work" && patch -p1 < "$p")
done
make -C "$work/ami9000" -j"$(nproc)" platform=unix >"$work/build.log" 2>&1 || { tail -40 "$work/build.log"; exit 1; }
mkdir -p "$(dirname "$out")"
cp "$work/ami9000/build/unix/puae_libretro.so" "$out"
echo "installed $out"
