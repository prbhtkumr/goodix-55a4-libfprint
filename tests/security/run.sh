#!/usr/bin/env bash
# Security test driver for the 55a4 libfprint patch set.
#
# Builds ASan+UBSan harnesses (clang libFuzzer + deterministic tests)
# against a clang-sanitized build of the patched tree, then runs them in
# two phases:
#
#   prove   - run the harnesses that exercise the *fixed* defects and
#             show them detecting the original defects (non-zero exit /
#             ASan report). Use on an unfixed tree to reproduce.
#   verify  - full green run: deterministic test + bounded fuzzing of the
#             image-ingest guard, the USB reassembly/parsers and the
#             SIGFM print-blob deserializer, plus the existing doctest
#             suite. Every target must exit 0.
#
# Requirements: the tree configured with clang + sanitizers:
#   CC=clang CXX=clang++ meson setup build-asan <tree> \
#       -Db_sanitize=address,undefined -Db_lundef=false -Ddoc=false \
#       -Dintrospection=false && ninja -C build-asan
#
# Environment:
#   GX55A4_TREE                libfprint work tree (default: the local build tree)
#   GX55A4_PKGCONFIG_COMPAT    dir holding the opencv4.pc alias for Arch
#   SEC_OUT                    scratch dir for objects/binaries/logs
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
# Workroot convention matches scripts/setup-community.sh: a sibling dir
# next to the repository (the colon-containing repo dir itself is never
# used for builds). Run from repository root or tests/security/. Locate
# the repository root by walking up to .git. Override with GX55A4_TREE /
# GX55A4_WORKROOT.
_repo="$SRC"
while [ ! -e "$_repo/.git" ] && [ "$_repo" != "/" ]; do
  _repo="$(dirname "$_repo")"
done
WORKROOT="${GX55A4_WORKROOT:-$(dirname "$_repo")/gx55a4-work}"
TREE="${GX55A4_TREE:-$WORKROOT/goodix-27c6-55a4-fingerprint-linux/work/twd-libfprint}"
BUILD="$TREE/build-asan"
OUT="${SEC_OUT:-${TMPDIR:-/tmp}/goodix-secbuild}"
PKGCOMPAT="${GX55A4_PKGCONFIG_COMPAT:-$WORKROOT/pkgconfig-compat}"

export PKG_CONFIG_PATH="$PKGCOMPAT${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"

mkdir -p "$OUT"/{obj,bin,logs,corpus-image,corpus-usb,corpus-sigfm}

# Extract the exact meson compile flags for a TU, absolutizing -I paths.
cflags() {
  python3 - "$BUILD/compile_commands.json" "$1" <<'EOF'
import json, sys, shlex, os
cc = json.load(open(sys.argv[1]))
want = sys.argv[2]
for e in cc:
    if e["file"].endswith(want):
        d = e["directory"]
        out, skip = [], False
        for i, x in enumerate(shlex.split(e["command"])):
            if i == 0:
                continue  # the compiler itself
            if skip:
                skip = False
                continue
            if x == "-o":
                skip = True
                continue
            if x in ("-MF", "-MQ", "-MT", "-MJ"):
                skip = True  # its argument is a build-tree path
                continue
            if x in ("-MD", "-MMD", "-MP"):
                continue
            if x == "-c" or x == e["file"] or x.endswith(want):
                continue
            if x.startswith("-I") and len(x) > 2 and x[2] != "/":
                x = "-I" + os.path.normpath(os.path.join(d, x[2:]))
            out.append(x)
        # Tokens are final-form (meson's escaping is already resolved by
        # shlex); join verbatim so escaped quotes stay literal.
        print(" ".join(out))
        sys.exit(0)
sys.exit("no compile_commands entry for " + want)
EOF
}

GF_CFLAGS="$(cflags drivers/goodixtls/goodix.c)"
G55_CFLAGS="$(cflags drivers/goodixtls/goodix55x4.c)"
GTLS_CFLAGS="$(cflags drivers/goodixtls/goodixtls.c)"
GP_CFLAGS="$(cflags drivers/goodixtls/goodix_proto.c)"
SIG_CFLAGS="$(cflags sigfm/sigfm.cpp)"
# fpi-print.c's meson flags carry a C-only -std=; F5 compiles as C++.
FPI_PRINT_CFLAGS="$(cflags fpi-print.c | sed -E 's/-std=(gnu|c)[0-9]+//g')"

DRV="$TREE/libfprint/drivers/goodixtls"
GLIB_LIBS="$(pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 gusb libusb-1.0 libssl libcrypto)"
FP_LIB="-L$BUILD/libfprint -Wl,-rpath,$BUILD/libfprint -lfprint-private -lfprint-2 -lnbis"
# Only the modules sigfm uses; the full opencv4 set drags in viz/vtk.
OCV_LIBS="-lopencv_core -lopencv_imgproc -lopencv_features -lopencv_flann"

do_build() {
  echo "== compiling driver TUs (ASan/UBSan) =="
  $CC $SAN $GF_CFLAGS -c "$DRV/goodix.c" -o "$OUT/obj/goodix.o"
  $CC $SAN $G55_CFLAGS -c "$DRV/goodix55x4.c" -o "$OUT/obj/goodix55x4.o"
  $CC $SAN $GTLS_CFLAGS -c "$DRV/goodixtls.c" -o "$OUT/obj/goodixtls.o"
  $CC $SAN $GP_CFLAGS -c "$DRV/goodix_proto.c" -o "$OUT/obj/goodix_proto.o"
  $CXX $SAN $SIG_CFLAGS -c "$TREE/libfprint/sigfm/sigfm.cpp" -o "$OUT/obj/sigfm.o"

  echo "== linking harnesses =="
  # F1: image-ingest guard. TU includes goodix55x4.c; needs the other
  # driver TUs for the send/TLS/protocol symbols they reference.
  $CC $SAN -fsanitize=fuzzer $G55_CFLAGS "$SRC/fuzz_image_ingest.c" \
      "$OUT/obj/goodix.o" "$OUT/obj/goodixtls.o" "$OUT/obj/goodix_proto.o" \
      "$OUT/obj/sigfm.o" $FP_LIB $GLIB_LIBS $OCV_LIBS -lstdc++ -o "$OUT/bin/fuzz_image_ingest"
  # F2: USB parsing. TU includes goodix.c + goodix55x4.c.
  $CC $SAN -fsanitize=fuzzer $GF_CFLAGS "$SRC/fuzz_goodix_usb.c" \
      "$OUT/obj/goodixtls.o" "$OUT/obj/goodix_proto.o" "$OUT/obj/sigfm.o" \
      $FP_LIB $GLIB_LIBS $OCV_LIBS -lstdc++ -o "$OUT/bin/fuzz_goodix_usb"
  # F3: SIGFM print blob (C++, no libfprint dependency).
  $CXX $SAN -fsanitize=fuzzer $SIG_CFLAGS "$SRC/fuzz_sigfm_blob.cpp" \
      "$OUT/obj/sigfm.o" $OCV_LIBS -o "$OUT/bin/fuzz_sigfm_blob"
  # F4: TLS error conversion (deterministic).
  $CC $SAN $GTLS_CFLAGS "$SRC/test_err_from_ssl.c" \
      "$OUT/obj/sigfm.o" $FP_LIB $GLIB_LIBS $OCV_LIBS -lstdc++ -o "$OUT/bin/test_err_from_ssl"
  # F5: print<->image SigfmImgInfo ownership (deterministic double-free
  # guard; links fpi-print.c from libfprint-private for the internal API).
  # SIG_CFLAGS supplies the opencv -I (img-info.hpp pulls opencv2/core.hpp).
  $CXX $SAN $FPI_PRINT_CFLAGS $SIG_CFLAGS "$SRC/test_print_image_ownership.cpp" \
      "$OUT/obj/sigfm.o" $FP_LIB $GLIB_LIBS $OCV_LIBS -o "$OUT/bin/test_print_image_ownership"
  # F6: disconnect-teardown receive state (deterministic guard for the
  # straggler-completion crash; TU includes goodix.c +
  # goodix55x4.c like F2, links the remaining driver TUs).
  $CC $SAN $GF_CFLAGS "$SRC/test_receive_teardown.c" \
      "$OUT/obj/goodixtls.o" "$OUT/obj/goodix_proto.o" "$OUT/obj/sigfm.o" \
      $FP_LIB $GLIB_LIBS $OCV_LIBS -lstdc++ -o "$OUT/bin/test_receive_teardown"
  echo "== build OK =="
}

make_corpus() {
  # Image: one exactly-full-size frame (the harness syncs the baseline to
  # it, exercising the complete decode path) and short/empty payloads
  # (the guard path).
  python3 - "$OUT/corpus-image/full.bin" <<'EOF'
import sys
raw = 108 * 88 // 4 * 6  # GOODIX_RAW_FRAME_SIZE
open(sys.argv[1], "wb").write(bytes(((i * 31 + (i >> 8) * 7) & 0xff) for i in range(raw)))
EOF
  head -c 100 /dev/zero > "$OUT/corpus-image/short.bin"
  : > "$OUT/corpus-image/empty.bin"
  # USB: small garbage plus one oversized (> cap) input to exercise the
  # reassembly cap path.
  head -c 64 /dev/urandom > "$OUT/corpus-usb/rand64.bin"
  head -c 70000 /dev/urandom > "$OUT/corpus-usb/rand70k.bin"
  # SIGFM: well-formed seeds generated by the harness itself.
  "$OUT/bin/fuzz_sigfm_blob" --make-seed "$OUT/corpus-sigfm" >/dev/null
}

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

do_prove() {
  make_corpus
  local rc=0
  echo "== F4 err_from_ssl (expect failure on unfixed tree) =="
  if "$OUT/bin/test_err_from_ssl" >"$OUT/logs/f4.prove" 2>&1; then
    echo "  F4: PASSED (fix already applied?)"
  else
    echo "  F4: FAILED as expected; first lines:"
    grep -m2 -E "ERROR: (Address|Leak)Sanitizer|SEGV" "$OUT/logs/f4.prove" || tail -2 "$OUT/logs/f4.prove" | sed 's/^/    /'
    rc=1
  fi
  echo "== F5 print/image ownership (expect double-free on unfixed tree) =="
  if "$OUT/bin/test_print_image_ownership" >"$OUT/logs/f5.prove" 2>&1; then
    echo "  F5: PASSED (fix already applied?)"
  else
    echo "  F5: FAILED as expected; first lines:"
    grep -m2 -E "ERROR: (Address|Leak)Sanitizer|attempting double-free" "$OUT/logs/f5.prove" || tail -2 "$OUT/logs/f5.prove" | sed 's/^/    /'
    rc=1
  fi
  echo "== F6 disconnect-teardown receive state (expect UAF/contract fail on unfixed tree) =="
  if "$OUT/bin/test_receive_teardown" >"$OUT/logs/f6.prove" 2>&1; then
    echo "  F6: PASSED (fix already applied?)"
  else
    echo "  F6: FAILED as expected; first lines:"
    grep -m2 -E "ERROR: (Address|Leak)Sanitizer|FAIL:" "$OUT/logs/f6.prove" || tail -2 "$OUT/logs/f6.prove" | sed 's/^/    /'
    rc=1
  fi
  echo "== F3 SIGFM blob (expect crash on unfixed tree) =="
  ASAN_OPTIONS=detect_leaks=0 "$OUT/bin/fuzz_sigfm_blob" "$OUT/corpus-sigfm" \
      -runs=100000 -max_len=1048576 -rss_limit_mb=4096 \
      >"$OUT/logs/f3.prove" 2>&1 && { echo "  F3: PASSED unexpectedly"; } || {
    echo "  F3: CRASHED as expected; first lines:"
    grep -m2 -E "ERROR: AddressSanitizer|SUMMARY" "$OUT/logs/f3.prove" || tail -2 "$OUT/logs/f3.prove" | sed 's/^/    /'
    rc=1
  }
  echo "== F1 image ingest (expect crash on unfixed tree) =="
  ASAN_OPTIONS=detect_leaks=0 "$OUT/bin/fuzz_image_ingest" "$OUT/corpus-image" \
      -runs=5000 -rss_limit_mb=4096 \
      >"$OUT/logs/f1.prove" 2>/dev/null && { echo "  F1: PASSED unexpectedly"; } || {
    echo "  F1: CRASHED as expected; first lines:"
    grep -m2 -E "ERROR: AddressSanitizer|SUMMARY" "$OUT/logs/f1.prove" || tail -2 "$OUT/logs/f1.prove" | sed 's/^/    /'
    rc=1
  }
  if [ $rc -ne 0 ]; then
    echo "PROVE: harnesses detected the defects (see $OUT/logs/*.prove)"
  else
    echo "PROVE: nothing failed - tree may already carry the fixes"
  fi
  return 0
}

do_verify() {
  make_corpus
  local fail=0

  echo "== F4 err_from_ssl =="
  "$OUT/bin/test_err_from_ssl" >"$OUT/logs/f4.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f4.verify)"; fail=1; }

  echo "== F5 print/image SigfmImgInfo ownership =="
  "$OUT/bin/test_print_image_ownership" >"$OUT/logs/f5.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f5.verify)"; fail=1; }

  echo "== F6 disconnect-teardown receive state =="
  "$OUT/bin/test_receive_teardown" >"$OUT/logs/f6.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f6.verify)"; fail=1; }

  echo "== F1 fuzz: image ingest =="
  ASAN_OPTIONS=detect_leaks=0 "$OUT/bin/fuzz_image_ingest" "$OUT/corpus-image" \
      -runs=20000 -rss_limit_mb=4096 >"$OUT/logs/f1.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f1.verify)"; fail=1; }

  echo "== F2 fuzz: USB reassembly + parsers =="
  ASAN_OPTIONS=detect_leaks=0 "$OUT/bin/fuzz_goodix_usb" "$OUT/corpus-usb" \
      -runs=100000 -max_len=131072 -rss_limit_mb=4096 >"$OUT/logs/f2.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f2.verify)"; fail=1; }

  echo "== F3 fuzz: SIGFM print blob =="
  ASAN_OPTIONS=detect_leaks=0 "$OUT/bin/fuzz_sigfm_blob" "$OUT/corpus-sigfm" \
      -runs=50000 -max_len=1048576 -rss_limit_mb=4096 >"$OUT/logs/f3.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/f3.verify)"; fail=1; }

  echo "== doctest suite (sigfm-tests, ASan build) =="
  ASAN_OPTIONS=detect_leaks=1 "$BUILD/libfprint/sigfm/sigfm-tests" \
      >"$OUT/logs/doctest.verify" 2>&1 \
    && echo "  PASS" || { echo "  FAIL (see $OUT/logs/doctest.verify)"; fail=1; }

  if [ $fail -ne 0 ]; then
    echo "VERIFY: FAILURES present"
    return 1
  fi
  echo "VERIFY: all green"
}

case "${1:-}" in
  build) do_build ;;
  prove) do_build; do_prove ;;
  verify) do_build; do_verify ;;
  *)
    echo "usage: $0 build|prove|verify" >&2
    exit 2
    ;;
esac
