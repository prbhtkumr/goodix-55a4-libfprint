# Security test harnesses (55a4 patch set)

Fuzz and regression harnesses that exercise the security-sensitive paths
of the patched driver: USB frame reassembly and parsing, TLS image
ingest, SIGFM print-blob deserialization, TLS error conversion,
print/image `SigfmImgInfo` ownership, and disconnect-teardown receive
state. Every target builds with ASan + UBSan (clang, libFuzzer) against
the same sources the production library is built from.

## Harnesses

| target | covers | invariant |
| ------ | ------ | --------- |
| `fuzz_image_ingest.c` (F1) | `scan_on_read_img()` / `on_scan_empty_img()` - the sensor controls the TLS plaintext length | payloads shorter than `GOODIX55X4_RAW_FRAME_SIZE` must be rejected by the short-image guard (no OOB read in `decode_frame`); full-size payloads must not trip the guard |
| `fuzz_goodix_usb.c` (F2) | two-chunk reassembly cap, dispatch (incl. the `TLS_DATA` 9-byte header strip), 7 receive parsers, `goodix_decode_protocol()` length arithmetic | reassembled payload never exceeds `GOODIX_MAX_RECV_BUF_SIZE`; no parser reads out of bounds; `payload_len` invariants hold |
| `fuzz_sigfm_blob.cpp` (F3) | `sigfm_deserialize_binary()` - every stored print is deserialized on verify | malformed blobs return `NULL` (no exception crosses the C ABI, no OOB read, no attacker-sized allocation); valid blobs survive a reserialize round trip |
| `test_err_from_ssl.c` (F4) | `err_from_ssl()` - every TLS failure | empty and populated OpenSSL error queues both produce a valid GLib error (registered domain, g_malloc'd message) |
| `test_print_image_ownership.cpp` (F5) | `fpi_print_add_from_image()` <-> `fp_image_finalize` `SigfmImgInfo` ownership - every print/image teardown order | exactly one owner frees the info: an image added to a print hands it over (ownership steal), images never added free it themselves; three teardown orders (production, reversed, never-attached) run leak- and double-free-free |
| `test_receive_teardown.c` (F6) | `goodix_dev_deinit()` / `goodix_receive_data_cb()` - close vs straggler bulk-IN completions | after close no receive state survives (buffer NULL, length 0, inited/read_pending cleared); a late completion is dropped before reassembly; direct late entry into `goodix_receive_pack()` stays bounded |

## Running

Requires a clang sanitizer build of the tree (the script extracts the
exact compile flags from its `compile_commands.json`):

```sh
CC=clang CXX=clang++ meson setup build-asan <tree> \
    -Db_sanitize=address,undefined -Db_lundef=false -Ddoc=false \
    -Dintrospection=false
ninja -C build-asan

tests/security/run.sh verify   # full green run (bounded fuzzing + doctest)
tests/security/run.sh prove     # demonstrate the fixed defects on an unfixed tree
tests/security/run.sh build     # harnesses only
```

Environment: `GX55A4_TREE` (work tree), `GX55A4_WORKROOT` (workroot
sibling dir, same convention as `scripts/setup-community.sh`; defaults
to `../gx55a4-work` next to the repository), `GX55A4_PKGCONFIG_COMPAT`
(dir with the `opencv4.pc` alias on Arch), `SEC_OUT` (scratch dir;
default `${TMPDIR:-/tmp}/goodix-secbuild`).

Fuzz runs use `ASAN_OPTIONS=detect_leaks=0` (libFuzzer's persistent
fixture allocates on purpose); the deterministic targets (F4-F6) and
the doctest suite run with leak detection on.

## Fixed defects (regression-covered here)

* Short TLS image payload reached `decode_frame()`'s fixed-size
  read (heap OOB read, device-triggerable).
* `err_from_ssl()` crashed on an empty OpenSSL queue
  (`strlen(NULL)`), overflowed the message buffer by one byte, and left
  the `GError` domain uninitialized.
* `stream::read(dist)` had no bounds check: a crafted print
  blob drove out-of-bounds reads, `std::vector::reserve()` /
  `cv::Mat::create()` with attacker-chosen sizes (crash in fprintd on
  every verify with a corrupted print file).
* `fp_print_serialize()` leaked every serialized SIGFM sub-print
  buffer (~430 KB per stage per enrollment) and misused
  `g_clear_object()` on a `GPtrArray`.
* unaligned `guint16` load in `goodix_receive_reset()`
  (UBSan-visible).
* `GOODIX_FLAGS_TLS_DATA` stripped a 9-byte header without a
  length guard: `payload + 9` pointer overflow and a wrapped
  `payload_len - 9` handed to the TLS layer on short frames.

Run `run.sh prove` on an unfixed tree to reproduce each defect as a
sanitizer crash; `run.sh verify` is the green gate that must hold after
every patch change.
