# Goodix 27c6:55a4 Linux Driver Patch Set

This patch brings stable Linux support for the Goodix **27c6:55a4**
fingerprint sensor as shipped in Lenovo ThinkBook, ThinkPad and IdeaPad
laptops. The chip reports itself as `GF3208` or `GF3268` depending on whether
its configuration survived the last reboot. It is a libfprint patch, so
enrolment and verification go through the fprintd and PAM stack you already
have.

The patch applies on top of a community driver stack: the
[Hydrogell/goodix-27c6-55a4-fingerprint-linux](https://github.com/Hydrogell/goodix-27c6-55a4-fingerprint-linux)
build harness at `fe27b4812d60b4709d3dfb2b72f1e1f2704ede18`, which in turn
builds TheWeirdDev's libfprint fork (`55b4-experimental`) at `d1ca62a`, home
of the Goodix TLS driver family and the SIGFM matcher. On top of that base,
[`patches/libfprint-55a4-local-fixes.patch`](patches/libfprint-55a4-local-fixes.patch)
carries the fixes in this repository (11 files, +1189/-164). This is a
community project extending upstream libfprint Goodix TLS support, informed by
USB protocol analysis and hardware observation for Linux interoperability.

## Status

Works as a daily driver: enrol and verify run end-to-end through fprintd/PAM,
including sensor-side TLS with a provisioned key (see *Install & provision*
below). USB runtime autosuspend stays enabled, which is the default for this
device, and the wake-reset fix below keeps sessions working across
suspend/resume.

## What the patch changes (vs. the pinned fork)

- **USB read model**: at most one bulk-IN URB in flight, issued on demand
  while a command awaits data. Stacked or perpetually-pending IN reads make
  this firmware answer `GET_IMAGE` with `REQUEST_TLS_CONNECTION` (0xd0), so the
  driver now *obeys* that too: it re-establishes TLS and retries the image
  request.
- **USB runtime-PM wake-reset**: a suspend/resume cycle can leave the
  firmware's bulk-OUT endpoint wedged when the system hwdb marks this device
  autosuspend-capable (`ID_AUTOSUSPEND=1`). Control transfers still answer in
  ~1 ms, but every command write is NAKed until a USB port reset, so fprintd
  activation died in state 0 (`transfer timed out` -> `enroll-unknown-error`)
  after any idle period. On that first failed write the driver resets the USB
  device (interface still claimed) and retries the wake-up NOP exactly once,
  which also restores it for subsequent activations.
- **Learned FDT thresholds**: read per-zone finger-detect thresholds from the
  sensor's own `FDT_MODE` reply (threshold = measured base >> 1) instead of
  trusting hardcoded constants that never fire.
- **Calibration stability guard**: the background/baseline frame is captured
  once per activation and accepted only when two candidates agree, so a
  finger-contaminated baseline can no longer corrupt every subsequent image.
- **LED state cache**: three-valued and driven by command `0xae`, so a lost
  acknowledgement cannot leave it stuck on the wrong state.
- **SIGFM matcher fixes**: operator ordering corrected, angle-clamping to
  avoid NaN near parallel vectors, and absolute-tolerance rotation
  consistency.
- **Image-quality gate (opt-in)**: a ridge-contrast stddev check on the
  squashed frame that rejects flat (sweaty/dry) captures with a *center-finger*
  retry rather than feeding them to the matcher. Off by default until
  calibrated on your unit (`GOODIX55X4_MIN_CONTRAST`).
- **Reassembly overflow cap**: the USB packet reassembly buffer now refuses
  streams that could never be a valid Goodix frame.
- **Input hardening**: everything a faulty or hostile sensor, or a corrupted
  stored print file, can hand the driver is checked before use.
  - Short TLS image payloads are rejected outright instead of being decoded
    past their end.
  - `TLS_DATA` frames shorter than the header they strip are refused.
  - The SIGFM print-blob deserializer bounds-checks every read and rejects
    oversized `vector`/`cv::Mat` allocations, closing the path where a crafted
    print file crashed fprintd on verify.
  - TLS failures surface as well-formed `GError`s, never half-built structs.
  - Per-enrollment serialization buffers are freed, plugging a leak of roughly
    10 MB into fprintd per enrolment.

  All of it is covered by [`tests/security/README.md`](tests/security/README.md).
- **Async lifecycle robustness**: sessions finish cleanly now, leaving no
  fprintd waiter behind.
  - **Verify stops no longer SIGABRT**: each capture's matcher data has
    exactly one owner, so tearing down a print can no longer free the same
    buffer twice.
  - **TLS teardown is synchronised**: the serve thread is joined before the
    SSL object is freed, so a sensor that disappears mid-handshake cannot be
    read after free. A failed sleep stage closes the TLS session before
    deactivation completes, so the next activation cannot trip an internal
    assertion.
  - **Every completion path completes**: a failed scan or write surfaces to
    fprintd as a session error (verify no longer just hangs), and failed
    activation or sleep stages run their callbacks rather than leaving the
    device claimed in ACTIVATING/DEACTIVATING.
  - **Leaks are plugged**: a cancelled read's `GError` and the matcher's
    per-capture sub-print allocation are freed.
- **Tunables**: environment knobs for enrollment stages, match threshold,
  touch-diff and contrast gates (table below). The accuracy-validated values
  ship as baked defaults, leaving the env vars as per-unit overrides.

## Build

```sh
./scripts/setup-community.sh
```

The script clones the pinned upstream into a colon-free sibling work
directory (`../gx55a4-work`, override with `GX55A4_WORKROOT`), swaps this
repo's patch in place of the upstream driver patch, then runs its `setup.sh`
(which builds the libfprint fork from our patch and clones goodix-fp-dump).
Arch note: the script aliases
`opencv5.pc` as `opencv4.pc` locally because the fork's `sigfm/meson.build`
names the `opencv4` dependency.

Build the library from the resulting tree:

```sh
ninja -C ../gx55a4-work/goodix-27c6-55a4-fingerprint-linux/work/twd-libfprint/build
```

## Security test suite

`tests/security/` contains libFuzzer + ASan/UBSan harnesses for the
patch's security-sensitive paths (USB reassembly and receive parsers, TLS
image ingest, SIGFM print-blob deserialization, TLS error conversion).
They build against a clang sanitizer build of the tree:

```sh
CC=clang CXX=clang++ meson setup build-asan \
    ../gx55a4-work/goodix-27c6-55a4-fingerprint-linux/work/twd-libfprint \
    -Db_sanitize=address,undefined -Db_lundef=false -Ddoc=false -Dintrospection=false
ninja -C ../gx55a4-work/goodix-27c6-55a4-fingerprint-linux/work/twd-libfprint/build-asan
tests/security/run.sh verify
```

## Install & provision (one-time, requires root)

**Read the upstream install/provisioning scripts before running them.** They
write a fixed, publicly-known "whitebox" key blob into the sensor's NVM (slot
`0xbb010003`) so the sensor accepts the driver's TLS-PSK handshake. The key is
not device-specific and not derived from anyone's hardware: it has shipped in
[goodix-fp-dump](https://github.com/goodix-fp-dump) for years. NVM writes
are permanent, though, so double-check you are on the intended device. The C
driver never
writes keys itself: it only verifies and refuses to activate if the expected
key is absent.

After installing the built `libfprint-2.so` where your fprintd loads it and
pointing fprintd at it (e.g. an `Environment=LD_LIBRARY_PATH=...` drop-in under
`/etc/systemd/system/fprintd.service.d/`), enrol and verify normally:

```sh
fprintd-enroll
fprintd-verify
```

## Enrollment guidance (this sensor is small)

The 55a4 sensing area is small relative to a fingertip, so a template only
covers a narrow patch per stage: what fails to match is usually *finger
displacement*, not a too-strict threshold. Because SIGFM scoring scales
rapidly with matching minutiae, the match threshold acts almost like a step
function: genuine presses that land on a stored patch score in the
hundreds-to-hundreds-of-thousands, everything else scores 0-26.

Real-world test results (GF3208):

| configuration | False Accepts | True Matches |
|---|---|---|
| 16 stages, threshold 72 (old defaults) | - | ~2/8 |
| 16 stages, threshold 24 | 0/11 | 1/6 |
| **24 stages, threshold 24 (current defaults)** | **0/6** | **6/6** |

The fix for poor acceptance is **enrolment coverage**, not threshold
lowering: enrol across the 24 stages by shifting your finger angle and
position on each tap (up/down/left/right/centre, firm press, full lift
between stages).

## Tunables

Read once per fprintd process start, so restart fprintd after changing.

| env var | default | meaning |
|---|---|---|
| `GOODIX55X4_ENROLL_STAGES` | `24` (clamp 4..32) | enrollment stages (coverage) |
| `GOODIX55X4_BZ3_THRESHOLD` | `24` | SIGFM match threshold |
| `GOODIX55X4_TOUCH_DIFF` | `300` | mean abs diff from baseline to accept a frame as a touch |
| `GOODIX55X4_MIN_CONTRAST` | `0.0` (disabled) | min ridge-contrast stddev; the stddev is printed per capture (`G_MESSAGES_DEBUG=all`) to calibrate the floor |

## Known behaviour

- After the last operation of a session the sensor re-enumerates 1-7 s later
  (post-session sleep). Chained fprintd commands with zero gap can race this
  and fail with a libusb error; >=5 s between commands avoids it.
- The chip reports its family string state-dependently (`GF3208` after a fresh
  boot vs `GF3268` when configuration persisted); both firmware strings are
  accepted.
- No liveness/presentation-attack detection: single static frame, no swipe
  dynamics. Do not use as your sole authentication factor in hostile
  environments.

## FAQ

**Why am I asked for a fingerprint *and* a password (lock screen, login,
`sudo`), or why does password entry stall ~30 s before prompting?**

This is PAM configuration, not the driver. It happens when
`pam_fprintd.so` sits in the *same* PAM stack as `pam_unix`, typically an
`auth sufficient pam_fprintd.so` line added to the shared `system-auth`
file (a widely suggested tweak for "fingerprint login"). PAM authenticates
strictly serially within one stack: the fingerprint module runs first and
waits for a scan (defaults per `pam_fprintd(8)`: 30 s timeout, 3 tries)
before the password module is even reached. The same module's man
page states that simultaneous methods require *separate* PAM stacks.
Desktop environments already do exactly that for lock-screen
fingerprints: KDE's kscreenlocker, for instance, ships its own dedicated
`kde-fingerprint` PAM service that runs *beside* the password service, so
either method alone unlocks. The line inside the password stack is
therefore both redundant and serialising. Check with
`grep -Rh fprint /etc/pam.d/`; removing it restores touch-*or*-type at
the lock screen and immediate password prompts everywhere else. The
fingerprint PAM module is inert unless a PAM service references it, so a
system without such a line is never affected: installing this driver
changes nothing about your PAM setup.

## Repository layout

```
patches/libfprint-55a4-local-fixes.patch    the driver patch (the actual product)
scripts/setup-community.sh                  pinned, reproducible build wrapper
docs/HARDWARE-NOTES.md                      USB protocol, register and field notes
docs/descriptors-55a4.txt                   live USB descriptor dump of the sensor
tests/security/                             security fuzzing and regression suites
LICENSE                                     LGPL-2.1-or-later
NOTICE                                      third-party attributions
```

## Credits & provenance

See [`NOTICE`](NOTICE). The heavy lifting is upstream's: the libfprint Goodix
TLS driver family (Alexander Meiler, Matthieu Charette, Alireza S.N. and
contributors), the SIGFM matcher (Matthieu Charette, Natasha England-Elbro),
TheWeirdDev's fork carrying the 55b4/55a4 support, the Hydrogell build
harness, and goodix-fp-dump's public protocol research and provisioning tooling.

## License

LGPL-2.1-or-later; see [LICENSE](LICENSE). The patch modifies and is
distributed together with LGPL-2.1+ code, so applying it to the pinned fork
produces an LGPL-2.1+ library.
