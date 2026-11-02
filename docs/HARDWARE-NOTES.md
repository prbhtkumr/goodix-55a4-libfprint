# Goodix 27c6:55a4 Linux driver hardware notes

Scope: the *current* implementation state - libfprint fork pinned at
`d1ca62a` (TheWeirdDev/libfprint) inside the community repo pinned at
`fe27b48`, plus the cumulative local patch
`patches/libfprint-55a4-local-fixes.patch` (built and validated on live
hardware 2026-10-20/24). Source references are `path:line` against the
patched fork's `libfprint/` tree unless noted.

Example runtime deployment: library at `/opt/libfprint-goodix/lib64`,
fprintd drop-in `/etc/systemd/system/fprintd.service.d/10-goodix55a4.conf`:

```
Environment=LD_LIBRARY_PATH=/opt/libfprint-goodix/lib64
Environment=G_MESSAGES_DEBUG=all
# Optional: the 24/24 defaults are now baked into the driver;
# these env lines are per-unit overrides, not requirements.
#Environment=GOODIX55X4_BZ3_THRESHOLD=24
#Environment=GOODIX55X4_ENROLL_STAGES=24
```

---

## 1. libfprint architecture & base class

### 1.1 Class hierarchy

The driver is an **`FpImageDevice`** subclass (not bare `FpDevice`):

```
FpiDeviceGoodixTls55X4            goodix55x4.c:114 (concrete, final)
  +- FpiDeviceGoodixTls           goodix.c:73   (abstract, private-data owner)
       +- FpImageDevice           (libfprint core)
            +- FpDevice
```

```c
// goodix.c:73
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpiDeviceGoodixTls, fpi_device_goodixtls,
                                     FP_TYPE_IMAGE_DEVICE);
// goodix55x4.c:168
G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4, FPI,
                     DEVICE_GOODIXTLS55X4, FpiDeviceGoodixTls);
```

Device class wiring (`fpi_device_goodixtls55x4_class_init`): `dev_class->type =
FP_DEVICE_TYPE_USB`, `scan_type = FP_SCAN_TYPE_PRESS`,
`algorithm = FPI_DEVICE_ALGO_SIGFM`, image geometry 108x88, id table
`{27c6:55b4 driver_data=0}`, `{27c6:55a4 driver_data=GOODIX_55X4_FLAG_LEARN_FDT}`
(`goodix55x4.h:75-79`).

### 1.2 Async GObject lifecycle hooks

All five hooks are installed in `fpi_device_goodixtls55x4_class_init`
and every one completes through libfprint's
`*_complete()` async contract:

| hook | function | behavior |
|---|---|---|
| `img_open` | `dev_init` (1282) | `goodix_dev_init()` resets base private state, allocates `transfer_cancel_tkn`, `g_usb_device_claim_interface(0)`; force-sends LED-off (`goodix55x4_clear_led`); `fpi_image_device_open_complete()`. |
| `activate` | `dev_activate` (1323) | starts the **activate SSM** (`activate_run_state`, sec 2.3); on success `activate_complete` chains `goodix_tls()` (TLS handshake, sec 2.4) and only then `fpi_image_device_activate_complete()`. |
| `change_state` | `dev_change_state` (1330) | handles `FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON` -> `scan_start()` (scan SSM, sec 3.3); all other states ignored (finger-off is driven from inside the scan SSM). |
| `deactivate` | `dev_deactivate` (1344) | `fpi_device_add_timeout(250 ms)` -> **sleep SSM** (0x60 -> 0x92/`0x6c` -> TLS shutdown + `fpi_image_device_deactivate_complete`). The 250 ms delay lets the current scan settle first. |
| `img_close` | `dev_deinit` (1301) | `goodix_reset_state()`, force LED-off (synchronous write before release), `goodix_dev_deinit()` -> cancels cancellable, `goodix_shutdown_tls()`, `g_usb_device_release_interface()`; `fpi_image_device_close_complete()`. |

State machines are GLib `FpiSsm`s: `ACTIVATE_*` (8 states, `goodix55x4.c:189`),
`SCAN_*` (9, :457), `SCAN_EMPTY_*` (3 sub-SSM, :910), `SLEEP_*` (3, :479),
`TLS_HANDSHAKE_STAGE_*` (5, `goodix.c:1539`). Every SSM failure propagates via
`fpi_ssm_mark_failed()` -> `*_complete(error)`.

### 1.3 Cancellation & async USB (GCancellable / GTask / FpiUsbTransfer)

- **One `GCancellable` per open**, `priv->transfer_cancel_tkn`
  (`goodix.c:57`): created in `goodix_dev_init` (:1411), cancelled in
  `goodix_cancel_receive`/`goodix_dev_deinit` (:1438, :1455), reset in
  `goodix_start_read_loop` (:566-567). It is handed to every async IN
  transfer: `fpi_usb_transfer_submit(transfer, 0, priv->transfer_cancel_tkn,
  goodix_receive_data_cb, NULL)` (:596-597).
- **Bulk IN is async**: `FpiUsbTransfer` is libfprint's GIO-async wrapper over
  GUsbDevice - internally `g_usb_device_bulk_transfer_async` with a
  GAsyncReadyCallback (`fpi-usb-transfer.c:423,313`). Completion callback:
  `goodix_receive_data_cb` (`goodix.c:498`).
- **Bulk OUT is synchronous**: `goodix_send_data` uses
  `fpi_usb_transfer_submit_sync(..., GOODIX_TIMEOUT=1000 ms, ...)`
  (`goodix_send_data`, timeout def `goodix.h:25`).
- **GTask** is used by libfprint core for the CPU-side pipeline, receiving the
  device cancellable: `fp_image_extract_sigfm_info(image, cancellable, ...)`
  -> `g_task_new()` + `g_task_run_in_thread(fp_image_sigfm_extract_thread_func)`
  (`fp_image_extract_sigfm_info`, worker thread
  `fp_image_sigfm_extract_thread_func` calls `sigfm_extract()` = OpenCV SIFT).
  `fpi_image_device_image_captured` passes
  `fpi_device_get_cancellable()` on that path (`fpi-image-device.c:526-527`);
  cancellation surfaces as `G_IO_ERROR_CANCELLED` handled at
  `fpi-image-device.c:254`.
- **Timeouts**: `goodix_send_protocol` arms `priv->timeout`
  (`fpi_device_add_timeout` inside `goodix_send_protocol`) firing
  `goodix_receive_timeout_cb` -> `G_IO_ERROR_TIMED_OUT` for command 0x%02x;
  FDT commands are sent with `timeout_ms = 0` (none - they legitimately block
  on finger/lift, `goodix.c:912,836,861`).

---

## 2. USB transport & endpoint configuration

### 2.1 Endpoints (from `docs/descriptors-55a4.txt`, verified live)

| role | address | type | wMaxPacket | notes |
|---|---|---|---|---|
| control | EP 0 | control | 64 B (`bMaxPacketSize0`) | standard USB, not used by protocol code |
| bulk OUT (commands) | **0x01** | bulk | 512 B (HS) | `GOODIX_55X4_EP_OUT = 0x1 \| FPI_USB_ENDPOINT_OUT` |
| bulk IN (replies/data) | **0x82** | bulk | 512 B (HS) | `GOODIX_55X4_EP_IN = 0x82 \| FPI_USB_ENDPOINT_IN` |
| interrupt | - | - | - | **none**: `bNumEndpoints = 2`; there is no polling endpoint |

Device: `bcdUSB 2.00`, class 239/2/1 (Misc + IAD), `bmAttributes 0xa0`
(bus-powered + remote wakeup), 100 mA; interface 0 vendor-specific (0xff),
claimed by `g_usb_device_claim_interface(..., class->interface /* 0 */, ...)`
(`goodix.c:1413`). Host-side constants: `GOODIX_EP_IN_MAX_BUF_SIZE 0x10000`,
`GOODIX_EP_OUT_MAX_BUF_SIZE 0x40` (`goodix_proto.h:22-23`).

### 2.2 Packet framing

Two nested layers, both little-endian, both packed (`goodix_proto.h/.c`):

```
OUT/IN frame (GoodixPack)            cmd packet inside flags=0xa0 frame (GoodixProtocol)
+------+--------+------+----------+  +--------+--------+------+----------------+----+
| flags| length | chk  | payload  |  | cmd    | length | ...  | payload        | chk|
| u8   | le16   | u8   | variable |  | u8     | le16   |      | variable       | u8 |
+------+--------+------+----------+  +--------+--------+------+----------------+----+
   ^      ^       ^                    length = payload_len + 1 (includes chk byte)
   |      |       additive sum of the 3 header bytes            chk = 0xAA - sum(bytes)
   |      payload length in bytes                                or 0x88 (GOODIX_NULL_CHECKSUM)
```

- **Outer checksum**: `goodix_calc_checksum` = plain byte sum (`goodix_proto.c:26-35`);
  encode writes `sum(flags,len)` at byte 3; decode compares equality
  (`goodix_proto.c:107-108`).
- **Inner checksum**: `0xaa - sum(cmd,len,payload)` or the fixed
  `GOODIX_NULL_CHECKSUM = 0x88` when `calc_checksum = FALSE` (NOP uses this)
  (`goodix_proto.c:80-85,133-137`).
- **Padding**: encodes pad the outer frame to a multiple of 64 B; the sender
  transmits **fixed 64-byte zero-padded bulk packets** - device parses the
  length field and ignores the zero tail (`goodix.c:613-642`, mirrors the
  pyusb reference).
- **Flags byte**: `0xa0` `GOODIX_FLAGS_MSG_PROTOCOL`, `0xb0` `GOODIX_FLAGS_TLS`,
  `0xb2` `GOODIX_FLAGS_TLS_DATA` (`goodix_proto.h:27-29`). For `0xb2`, the
  first 9 payload bytes are stripped before the TLS layer
  (`WeirdMCUPack`, 9 B; `goodix.c:456-461`).
- **There are no sequence counters.** Ordering is enforced host-side only:
  `goodix_send_protocol` refuses to start a second command while
  `ack || reply || timeout` is set (`goodix.c:677-684`), replies must carry
  the same `cmd` as `priv->cmd` or they are dropped with
  `"Invalid protocol command: 0x%02x"` (inside `goodix_receive_protocol`), and
  ACKs are matched against `priv->cmd` too (inside `goodix_receive_ack`).
- **ACK frame** (`GoodixAck`, `goodix_proto.h:80-86`): `{u8 cmd; always_true:1;
  has_no_config:1}`. `has_no_config` prints `"MCU has no config"` - the bit
  that correlates with the GF3208-vs-GF3268 firmware-string flip (observed on
  live hardware across power cycles).

**Read model (critical, a local protocol fix)**: at most **one** bulk-IN URB in flight
(`read_pending`, `goodix.c:585-588`), issued *on demand* - right after a
command write (`goodix_send_protocol` tail, :709-712) or for TLS reads
(`goodix_read_tls`, :1485-1487) - and re-armed only while `ack || reply`
remain set (`goodix_receive_data_cb`, :524-538). A perpetually-pending IN read
on 0x82 makes the 55a4 firmware answer `GET_IMAGE` with `0xd0`
(`REQUEST_TLS_CONNECTION`) instead of a frame; the "obey 0xd0" recovery path
re-handshakes and retries <= 2x (`goodix.c:369-398`).

**Reassembly**: `goodix_receive_pack` accumulates IN transfers into
`priv->data` until the outer length field decodes
(sec 6.2b). One raw frame is 14 256 B -> ~28 x 512 B USB packets of payload
(plus pack/TLS overhead); libusb transfers may coalesce these (up to 64 KB
requested), so reassembly is driven by the **length field**, not by packet
count.

### 2.3 Initialization sequence on wake (`ACTIVATE_*` SSM)

| # | state | wire command (opcode) | payload | expected completion |
|---|---|---|---|---|
| 1 | `ACTIVATE_READ_AND_NOP` | `goodix_start_read_loop` + `NOP 0x00` | 4x0x00, checksum=`0x88` (null), reply=FALSE, completed locally to flush stale command buffer | `activate_read_and_nop_done` (see the wake-recovery note below; other states use `check_none`) |
| 2 | `ACTIVATE_ENABLE_CHIP` | `ENABLE_CHIP 0x96` | `{enable=1}`, reply=FALSE (ACK only) | `check_none` |
| 3 | `ACTIVATE_NOP` | `NOP 0x00` | as above | `check_none` |
| 4 | `ACTIVATE_CHECK_FW_VER` | `FIRMWARE_VERSION 0xa8` | 1 B, reply=TRUE | reply = NUL-terminated string; accepted iff prefix `GF32` **and** contains `_RTSEC_APP_100` (observed `GF3208/GF3268_RTSEC_APP_10063`; loose regex = goodix-fp-dump's `VALID_FIRMWARE`, `check_firmware_version`) |
| 5 | `ACTIVATE_CHECK_PSK` | `PRESET_PSK_READ 0xe4` | `{flags le32=0xbb020007, length le32=0}` (8 B, byte-exact pyusb variant), reply=TRUE | reply: status + `{flags,length,offset}` + 32 B key; **flags == `0xbb020007`**, **len == 32**, key **== `goodix_55x4_psk_0`** (`81 b8 ff 49 ...`, `goodix55x4.h:33`); failure => activation aborts (`check_preset_psk_read`). The C driver never writes this key - see sec 5 (`provision_psk.py`, slot `0xbb010003`, one-time). |
| 6 | `ACTIVATE_RESET` | `RESET 0xa2` | `{reset_sensor=1, soft_reset_mcu=1, sleep_time=20}` | reply: status byte + LE u16 reset number **== 1024** (`GOODIX_55X4_RESET_NUMBER`, `check_reset`) |
| 7 | `ACTIVATE_SET_MCU_IDLE` | `MCU_SWITCH_TO_IDLE_MODE 0x70` | `{sleep_time=20}`, reply=FALSE (ACK only) | `check_idle` |
| 8 | `ACTIVATE_SET_MCU_CONFIG` | `UPLOAD_CONFIG_MCU 0x90` | `goodix_55x4_config` - 256-byte vendor blob, tail `...0x9a,0x69` | success reply via `goodix_receive_success` (2 B, nonzero = success) |
| 9 | TLS (not an SSM state) | `goodix_tls()` sec 2.4 | - | `fpi_image_device_activate_complete` |

**Wake-recovery on state `ACTIVATE_READ_AND_NOP` (environmental).** The
system hwdb marks `usb:v27C6p55A4*` autosuspend-capable
(`ID_AUTOSUSPEND=1`), and after a runtime suspend/resume cycle the 55a4
firmware can come back with EP0 alive (~1 ms control replies) while
bulk-OUT stays wedged - every OUT to `0x01` NAKs until a USB port reset
(measured on hardware with raw usbfs probes). The state-0 `NOP` write
then fails with `transfer timed out` and fprintd reports
`enroll-unknown-error`. `activate_read_and_nop_done` recovers in-band: on
a write error it defers to a `g_idle_add` source (a synchronous retry
would race `goodix_send_nop()`'s trailing `goodix_receive_done()`), calls
`g_usb_device_reset()` with the interface still claimed (the same port
reset enumeration performs; no NVM/config involvement), and retries the
NOP exactly once - `nop_wake_tried`, cleared per activation in
`dev_activate`. A failed reset fails the SSM with the original error.
After recovery the device also survives the *next* suspend/resume cycle,
so the retry is not needed on every activation.

### 2.4 TLS layer (`goodixtls.c`, `goodix.c:1469-1772`)

- `goodix_tls_server_init`: OpenSSL `SSL_CTX` pinned to **TLS 1.2 only**,
  cipher list `"PSK:@SECLEVEL=0"`, PSK server callback returning **32 zero
  bytes** (`tls_server_psk_server_callback`, `goodixtls.c:55-77`) - i.e. the
  "all-zeros" handshake key; the sensor only agrees once the public whitebox
  blob sits in NVM slot `0xbb010003` (sec 5). Transport is an `AF_UNIX`
  `socketpair`; `SSL_accept` runs on a `pthread` (`goodixtls.c:158-194`).
- Handshake drive (`tls_handshake_run`, `goodix.c:1571-1632`): send server
  Hello (flags `0xb0`), proxy the next three device flights
  (`goodix_read_tls` -> 0xb0), after server ChangeCipherSpec+Finished sleep
  **15 ms** (fw quiet time; without it every later `GET_IMAGE` answers `0xd0`),
  then send **`TLS_SUCCESSFULLY_ESTABLISHED 0xd4`** (`tls_handshake_done`).
- The whole handshake is *triggered* by the device's `0xd0`
  `REQUEST_TLS_CONNECTION` (`goodix_send_request_tls_connection`,
  `on_goodix_request_tls_connection`, `goodix.c:1641-1672`).
- Image path: `goodix_tls_read_image` -> `GET_IMAGE 0x20` -> reply arrives as
  `0xb2` TLS_DATA -> strip 9 B -> feed ciphertext to the TLS hop -> `SSL_read`
  decrypts -> plaintext raw frame to the capture callback
  (`goodix_tls_ready_image_handler`, `goodix.c:1724-1757`).

### 2.5 Steady-state command set (opcodes, `goodix_proto.h:31-55`)

`0x00` NOP | `0x20` GET_IMAGE | `0x32` FDT_DOWN | `0x34` FDT_UP | `0x36`
FDT_MODE | `0x60` SLEEP | `0x92` SLEEP_REALTEK (payload `0x6c`) | `0x70`
IDLE | `0x80/0x82` write/read sensor register (present, currently unused -
commented `0x022c <- {0x05,0x03}` hint at `goodix55x4.c:1158-1159`) | `0x90`
UPLOAD_CONFIG | `0x96` ENABLE_CHIP | `0xa2` RESET | `0xa8` FW_VERSION |
`0xae` QUERY_MCU_STATE = **LED** (`00 01 32` = on, `01 00 32` = off, three-byte
payloads, reply=TRUE, `goodix55x4.c:489-555`) | `0xb0` ACK | `0xd0`
REQUEST_TLS_CONNECTION | `0xd4` TLS_ESTABLISHED | `0xe0/0xe4` PSK write/read.

---

## 3. Sensor capture & calibration pipeline

### 3.1 Frame geometry & bit depth

| quantity | value | source |
|---|---|---|
| delivered image | **108 x 88 px** (`GOODIX55X4_WIDTH x HEIGHT`) | `goodix55x4.c:46-47` |
| pixels per frame | 9 504 | - |
| raw frame on wire | **14 256 B** = `(88 x 108)/4 x 6` (`GOODIX55X4_RAW_FRAME_SIZE`) | `goodix55x4.c:52-53` |
| packing | **4 pixels per 6 bytes**, 12-bit grayscale (`Goodix55X4Pix` = `unsigned short`, histogram masked `& 0xfff`) | `decode_frame` :589-608, :652 |
| scan width (uncropped) | 108 (so crop = none in x; the <=1-px dead border is handled downstream) | `goodix55x4.c:48` |
| delivered to libfprint | **8-bit** 0..255 (`FpImage`), 12->8-bit done by `squash_frame_linear` | :637-687 |
| frames per touch | `GOODIX55X4_CAP_FRAMES = 1` (averaging path exists but is inert) | :61, :793-816 |

`decode_frame` nibble unpacking (exact):

```c
for (int i = 0; i < GOODIX55X4_RAW_FRAME_SIZE; i += 6) {
  const guint8 *chunk = raw_frame + i;
  *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
  *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
  *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
  *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
}
```

### 3.2 Multi-packet assembly

1. USB: 512 B bulk-IN transfers arrive on 0x82; `goodix_receive_data_cb`
   appends each into `priv->data` (`goodix_receive_pack`, sec 6.2b) until the
   outer `GoodixPack.length` is satisfied - ~28 x 512 B packets of raw-frame
   payload, but the protocol is length-driven, not packet-count-driven.
2. Flags `0xb2` -> strip 9 B header -> TLS `SSL_read` decrypts the record
   (`goodix_tls_ready_image_handler`) -> 14 256 B plaintext raw frame handed to
   `scan_on_read_img` / `on_scan_empty_img`.
3. Downstream there is *no* striping across frames: one GET_IMAGE = one frame
   (the movement-estimation assembler runs but with a single frame it is a
   pass-through, sec 6.3).

### 3.3 Scan & calibration SSM (`SCAN_*`, `goodix55x4.c:457-477,1108-1212`)

```
CALIBRATE - (only if !empty_img_valid) -> SCAN_EMPTY sub-SSM:
              UPLOAD_CONFIG(0x90, in TLS) -> FDT_MODE(0x36, neutral 0x8000 probe)
              -> GET_IMG(0x20) x2 (stability-guarded baseline)
LED_ON(0xae) -> FDT_MODE(0x36) -> FDT_DOWN(0x32) -> GET_IMG(0x20)
             -> FDT_MODE(0x36) -> FDT_UP_NO_REPLY(0x34, ack-only)
             -> FDT_UP(0x34, blocks until lift) -> FDT_DONE (finger off report)
```

**Baseline / DC subtraction - yes, host-side and per-activation:**

- `empty_img[9504]` baseline captured once per activation and *validated for
  stability*: two candidate frames must differ by mean |delta| <= `12.0`
  (`GOODIX55X4_CAL_STABLE_DIFF`), retried <= 16 x 150 ms; otherwise the last
  candidate is taken with a warning (`on_scan_empty_img`, :941-987). Vendor
  behavior mirrored: never re-capture the baseline while a finger may be down
  (`empty_img_valid`, :126-133); FDT_UP blocking keeps lift-off clean.
- Per-pixel subtraction is **absolute difference**: `postprocess_frame`
  computes `|frame - empty_img|` and warns if the sum is 0 ("finger on scanner
  during calibration?") (:695-715).
- Contrast normalization (`squash_frame_linear`, :637-687): 4096-bin histogram
  over the **interior** (1-px border ring excluded), stretch from the **2nd to
  98th percentile**, out-of-band pixels clamp - replaces naive min/max which
  the comment documents as causing SIFT keypoint counts to swing 62..235.
- **Sensor-side gain/offset registers**: not written directly by the driver.
  Gain/offset/DC programming lives inside the uploaded 256-byte vendor config
  blob `goodix_55x4_config` (`goodix55x4.h:39-64`, identical to
  `DEVICE_CONFIG` in goodix-fp-dump `driver_55x4.py:30`), uploaded with
  `0x90` at activate *and* before each baseline. Register-write primitives
  (`0x80`/`0x82`) exist but are unused (commented `0x022c` hint).
- **FDT zone thresholds are learned per unit**: `FDT_MODE` replies carry the
  per-zone base (4 B header + 12 x le16); threshold = `base >> 1` - identical
  to the value the vendor's Windows driver sends on the same hardware, 12
  zones (`GOODIX55X4_FDT_ZONES`), sent back in FDT payloads
  (`build_fdt_payload`: `kind, 0x01, 12 x (0x80, threshold)` = 26 B;
  neutral baseline probe table is 24 B; static fallback tables
  `fdt_switch_state_{mode,mode2,down,up}_55X4` are 26 B - 55b4 heritage, used
  only if learning is off; `55a4` sets `GOODIX_55X4_FLAG_LEARN_FDT`).
- **Host-side finger detect (backstop)**: every scanned frame is diffed
  against the baseline; mean |delta| must reach `touch_diff` (default **300**,
  env `GOODIX55X4_TOUCH_DIFF`, field-calibrated idle ~4-5 / contact ~950) or
  the frame is re-polled every 100 ms up to 100 attempts
  (`scan_on_read_img`, :767-791; constants :75-85).

---

## 4. Verification, enrollment & scoring

### 4.1 Enrollment stages

There is no `fp_image_device_set_nr_enroll_stages()` API - it is a class
field assignment in `class_init` (`goodix55x4.c:1369-1377`):

```c
  const char *stages_env = g_getenv ("GOODIX55X4_ENROLL_STAGES");
  int stages = (stages_env && *stages_env) ? atoi (stages_env) : 24;
  dev_class->nr_enroll_stages = CLAMP (stages, 4, 32);
```

Default **24** (baked 2026-10-24 from the field battery; was 16
upstream), clamped 4..32; `GOODIX55X4_ENROLL_STAGES` is now only a per-unit
override. (fprintd's D-Bus property
reported `enroll stages changed to 25` - its display accounting sits one
above the class value; observed, mechanism in fprintd, not here). Each stage
captures one image -> one SIFT
sub-print (`fpi_image_device_minutiae_detected`: `fpi_print_add_from_image`
then `fpi_print_add_print(enroll_print, print); priv->enroll_stage += 1`,
completion at `enroll_stage == nr_enroll_stages`,
`fpi-image-device.c:295-314`). The template therefore holds **N sub-prints,
one per stage** - verified live (16 and 24 score-line counts).

### 4.2 Matcher: SIGFM (native libfprint, not Bozorth3, not in-driver)

- Dispatch: `img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM` ->
  `fpi-image-device.c:331/360` calls `fpi_print_sigfm_match(...)`. The NBIS
  Bozorth3 path (`fpi_print_bz3_match`, default threshold
  `BOZORTH3_DEFAULT_THRESHOLD = 40`, `fp-image-device.c:26`) exists but is
  only selected for `FPI_PRINT_NBIS` - **not used by this driver**.
- SIGFM is compiled into libfprint itself: `libfprint/sigfm/sigfm.cpp`
  (c) 2022 Charette / England-Elbro, LGPL-2.1+, OpenCV SIFT:
  - **extract** `sigfm_extract`: `cv::SIFT::detectAndCompute` over a full-image
    mask -> `SigfmImgInfo{keypoints, descriptors}` (:127-140).
  - **score** `sigfm_match_score(frame, enrolled)` (:142-241):
    1. `BFMatcher::knnMatch(k=2)` + Lowe ratio `d1 < 0.75*d2` -> unique
       keypoint pairs; `nb_matched < min_match(5)` -> 0.
    2. Pair-of-pairs with <=5 % length mismatch -> rotation hypothesis (with
       clamped `asin/acos` to avoid NaN near parallel vectors).
    3. `angles.size() < 5` -> 0.
    4. **Score = count of mutually rotation-consistent angle-hypothesis pairs**
       (`|delta sin| <= 0.10 rad && |delta cos| <= 0.10 rad`, absolute tolerance -
       the fixed variant). Fully-consistent m keypoints -> ~`C(C(m,2), 2)` -
       quartic growth (m=5->~45, m=6->~105 ...), hence the near-step behavior at
       any threshold in 24..72. Exceptions -> -1 (match error).
- Templates are serialized into `FpPrint` as binary `SigfmImgInfo` blobs
  (`fp-print.c:730,897-923`).

### 4.3 The values 24 / 24 and score bounds (exact origins)

**Threshold 24** - `class_init`, `goodix55x4.c` (baked default since
2026-10-24):

```c
  // Match threshold, env-tunable per unit (GOODIX55X4_BZ3_THRESHOLD). SIGFM's
  // score is quartic in matched-minutiae count, so this is effectively a
  // step: m>=6 scores ~369+, m<=5 lands at 0-26 on this GF3208 unit. 24 is
  // the field-validated pair for the 24-stage default above: impostors
  // 0/17 with max score 3 vs genuine matches 369-141697 ...
  const char *bz3_env = g_getenv ("GOODIX55X4_BZ3_THRESHOLD");
  img_dev_class->bz3_threshold = (bz3_env && *bz3_env) ? atoi (bz3_env) : 24;
```

Default is **24** (previously upstream's `24 * 3 = 72`, overridden at runtime
by the drop-in env var; the validated value is now the compiled default and
the env var a per-unit override). Plumbing: `FpImageDeviceClass.bz3_threshold` ->
`priv->bz3_threshold` (falls back to `BOZORTH3_DEFAULT_THRESHOLD = 40` if <= 0,
`fp-image-device.c:191-193`) -> passed into the match call
(`fpi-image-device.c:331-333`).

**Stages 24** - baked default (sec 4.1); `GOODIX55X4_ENROLL_STAGES` overrides.

**Decision + logging** - `fpi-print.c:253-277` (exact):

```c
FpiMatchResult fpi_print_sigfm_match(FpPrint *template, FpPrint *print,
                                     gint bz3_threshold, GError **error) {
  if (template->type != FPI_PRINT_SIGFM) { /* NOT_SUPPORTED error */ }

  SigfmImgInfo *against = g_ptr_array_index(print->prints, 0);
  for (int i = 0; i != template->prints->len; ++i) {
    SigfmImgInfo *pinfo = g_ptr_array_index(template->prints, i);
    int score = sigfm_match_score(pinfo, against);
    if (score < 0) { /* DATA_INVALID error */ }
    fp_dbg("sigfm score %d/%d", score, bz3_threshold);
    if (score >= bz3_threshold)
      return FPI_MATCH_SUCCESS;      /* early return on first hit */
  }
  return FPI_MATCH_FAIL;
}
```

So `sigfm score 369/24` means: sub-print *i* produced **369 mutually
rotation-consistent angle pairs >= threshold 24** => `FPI_MATCH_SUCCESS`.
The `/N` in the log is the live threshold (24 - the baked default; before
that, 24 via the drop-in env var, 72 from the old compiled default).

**Field data (GF3208 unit, 2026-10-20):**

| configuration | False Accepts | True Matches | observed scores |
|---|---|---|---|
| 16 stages, threshold 72 | - | ~2/8 | hits 86-112, misses 0-26 |
| 16 stages, threshold 24 | 0/11 | 1/6 | impostor max **3**; hit 76; misses 0-3 |
| **24 stages, threshold 24** | **0/6** | **6/6** | impostor max **0**; hits **369 - 141 697** (sub-prints #3-#8) |

### 4.4 Image-quality checks before `fpi_image_device_image_captured`

A **ridge-contrast gate** exists since the 2026-10-24 hardening pass, though it
ships **disabled** (`GOODIX55X4_MIN_CONTRAST` default 0.0) pending one
calibration battery. Pipeline order:

1. finger-detect diff >= `touch_diff` (300, env-tunable) - else re-poll/fail
   (`scan_on_read_img`);
2. calibration-baseline stability (<= 12.0 mean delta, else retry/force-take);
3. `postprocess_frame` sum-zero warning (frame == baseline) - logged, return
   value not enforced at the call site;
4. percentile contrast normalization (sec 3.3) - improves SIFT but is not a gate;
5. **contrast stddev gate** (`frame_contrast_score`, interior ring of the
   squashed 8-bit frame): if `GOODIX55X4_MIN_CONTRAST > 0` and the stddev is
   below it -> `FP_DEVICE_RETRY_CENTER_FINGER` via the scan-SSM
   `mark_failed` channel (same teardown path as the touch backstop). The
   stddev is printed for every capture (`Image contrast stddev: ...`) to
   calibrate the floor; 0.0 (default) skips the gate;
6. `fpi_do_movement_estimation` + `fpi_assemble_frames` over the frame list
   (single frame in practice).

Until the gate is enabled, dry/sparse/sweaty images are not intercepted as
*quality* failures - they manifest as low scores; enabling it
converts that into an explicit CENTER_FINGER retry.

The channel is end-to-end since the robustness fixes: the
`write_sensor_complete`/`scan_complete` error paths now call
`fpi_image_device_session_error()` (they used to log-and-drop the error
and leak the GError), so a gate hit - or any scan failure - reaches
fprintd as a real `FP_DEVICE_RETRY_CENTER_FINGER`/session error instead
of hanging the verify session.

### 4.5 Verification pipeline summary

```
press -> FDT_DOWN blocks until touch -> GET_IMAGE(0x20) -> 0xb2/TLS decrypt
  -> 14256B raw -> |frame - empty_img| -> percentile squash -> FpImage (108x88x8)
  -> fpi_image_device_image_captured -> GTask: SIFT extract (probe SigfmImgInfo)
  -> fpi_print_sigfm_match: for each of N enrolled sub-prints:
        BFMatcher ratio test -> geometry pairs -> angle-pair score
        >= 24 ? SUCCESS (early return) : next sub-print ... -> FAIL
  -> fpi_device_verify_report -> fprintd -> PAM
```

Notable properties: threshold/stages are read **once per fprintd
process start** (class_init) - restart required after env changes; the match
loop is order-dependent only in latency, not outcome; there is no liveness /
presentation-attack detection (single static frame, no swipe dynamics); FAR
margin at the deployed threshold was 0/17 impostor presses with max score 3
vs genuine >= 369; the post-session sleep (`0x60`/`0x6c`) triggers a
disconnect+re-enumerate 1-7 s after deactivation, which races zero-gap
chained fprintd operations (`libusb -4`) - mitigated by >=5 s gaps between
consecutive fprintd commands.

---

## 5. Borrowed sister-driver code & FOSS artifacts

| artifact | origin | what was taken | license |
|---|---|---|---|
| `goodix.c/h`, `goodixtls.c/h`, `goodix_proto.c/h`, `goodix55x4.c/h` | libfprint `drivers/goodixtls` (c) 2021 Meiler, Charette, Alireza, via TheWeirdDev/libfprint `55b4-experimental` @ `d1ca62a` | base transport, SSMs, TLS hop, protocol codecs, 55b4/55a4 skeleton | **LGPL-2.1+** (file headers) |
| `goodix511.c/h` | upstream libfprint Goodix 511 driver (same headers) | sibling driver kept in tree; `goodix_511_psk_0` referenced by the TLS PSK length check (`goodixtls.c:61`) | LGPL-2.1+ |
| `libfprint/sigfm/` (`sigfm.cpp/hpp`, `binary.hpp`, `img-info.hpp`) | (c) 2022 Charette & England-Elbro (TheWeirdDev fork) | SIGFM matcher/serializer (sec 4.2) | LGPL-2.1+ |
| host-side finger-poll (`frame_diff_score`, `retry_finger_poll`, `poll_attempts`) | sidevesh, TheWeirdDev/libfprint **PR #4** - credited in-source (`goodix55x4.c:63-74`) | touch-detect backstop; threshold + env override changed by us | LGPL-2.1+ |
| `squash_frame_linear` | "Borrowed from the elan driver", then rewritten (interior percentile trim) | 12->8-bit contrast normalization | LGPL-2.1+ |
| `DEVICE_CONFIG` == `goodix_55x4_config` | **goodix-fp-dump** `driver_55x4.py:30` (`DEVICE_CONFIG`) | 256-byte MCU/sensor register config blob | **MIT** (goodix-fp-dump `LICENSE`) |
| whitebox PSK provisioning (`provision_psk.py`, `write_psk(0xbb010003, PSK_WHITE_BOX)`) | goodix-fp-dump `driver_55x4.py:63-70` + community `scripts/` | one-time NVM key write (C driver only *checks*, never writes) | MIT |
| research helpers (`probe_psk.py`, `dump_snapshot.py` ...) | goodix-fp-dump / community repo | read-only device probes | MIT / repo scripts |
| community protocol/technical docs | Hydrogell/goodix-27c6-55a4-fingerprint-linux | protocol facts, thresholds, knob descriptions - **linked upstream, not vendored** (no license stated upstream) | upstream terms |
| libfprint core (`fp-*`, `fpi-*`, `fpi-usb-transfer`, `fpi-assembling`, NBIS/Bozorth3 plumbing) | libfprint upstream | base classes, GTask/GUsbDevice async, assembler | LGPL-2.1+ |
| OpenCV (SIFT, BFMatcher) | opencv project (host package; `opencv4.pc` alias for Arch's `opencv5.pc`) | feature extract/match | Apache-2.0 |
| OpenSSL TLS-PSK server | openssl | localhost TLS terminating the sensor's session | Apache-2.0 |
| this patch's local fixes (`read_pending`/on-demand IN, 0xd0 reimage, LED 3-state cache, calibration guard, FDT threshold learning, percentile squash, SIGFM correctness fixes, quality gate, reassembly cap, input hardening, env/baked tunables, USB runtime-PM wake-reset, async-lifecycle robustness: TLS serve-thread join, scan/activate/sleep error-path completion incl. TLS teardown on sleep failure, cancelled-transfer handling, capture-data single-ownership transfer + cleanup, allocator unification, disconnect-teardown receive-state guard: teardown clears the reassembly buffer and flips the device-inited flag before releasing anything, and the receive callback drops straggler completions on a closed device) | original work on top of the pinned fork, recorded in `patches/libfprint-55a4-local-fixes.patch` (11 files, +1189/-164 on top of `d1ca62a`) | - | LGPL-2.1+ (same as the code it patches) |

---

## 6. Core code snippets (verbatim)

### 6.a Device private structs

There is no struct named `Goodix55a4Device`; the concrete device state is
`struct _FpiDeviceGoodixTls55X4` (`goodix55x4.c:114-166`):

```c
struct _FpiDeviceGoodixTls55X4 {
  FpiDeviceGoodixTls parent;

  guint8 *otp;

  GSList *frames;
  guint poll_attempts;

  // Per-zone finger-detect thresholds measured from this sensor (see above).
  guint8 fdt_thresholds[GOODIX55X4_FDT_ZONES];
  gboolean fdt_thresholds_valid;

  // The background frame is valid for the whole activation. ...
  gboolean empty_img_valid;

  // Validating a baseline candidate before accepting it. ...
  Goodix55X4Pix cal_prev[GOODIX55X4_FRAME_SIZE];
  gboolean cal_have_prev;
  guint cal_attempts;

  // Cached LED state, so the switch is only sent on an actual change: ...
  Goodix55X4LedState led_state;
  gboolean led_want_on;

  // Effective touch threshold; see GOODIX55X4_TOUCH_DIFF.
  double touch_diff;

  Goodix55X4Pix empty_img[GOODIX55X4_FRAME_SIZE];
};
```

Transport state lives in the abstract base's instance private,
`FpiDeviceGoodixTlsPrivate` (`goodix.c:36-71`, incl. the local protocol-fix fields):

```c
typedef struct
{
  GoodixTlsServer    *tls_hop;

  GSource            *timeout;

  guint8              cmd;

  gboolean            ack;
  gboolean            reply;

  GoodixCmdCallback   callback;
  gpointer            user_data;

  guint8             *data;
  guint32             length;

  GoodixCallbackInfo *tls_ready_callback;

  GCancellable       *transfer_cancel_tkn;
  gboolean            inited;
  /* TRUE while a bulk-IN URB is in flight on the data endpoint. Guarantees at
     most ONE pending IN read, and only while a command awaits data - the exact
     pattern of the working pyusb reference and the Windows driver. Stacked or
     pre-command IN URBs make the 55a4 fw answer GET_IMAGE with 0xd0. */
  gboolean            read_pending;

  /* "Obey 0xd0": when the fw answers GET_IMAGE with REQUEST_TLS_CONNECTION,
     comply - re-establish TLS and retry the image request. ... */
  GoodixCmdCallback   reimage_callback;
  gpointer            reimage_user_data;
  guint               reimage_tries;
} FpiDeviceGoodixTlsPrivate;
```

### 6.b USB packet reassembly / capture callback

Reassembly across bulk-IN transfers, `goodix_receive_pack`:

```c
void
goodix_receive_pack (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 flags;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum; // TODO implement checksum.

  priv->data = g_realloc (priv->data, priv->length + length);
  memcpy (priv->data + priv->length, data, length);
  priv->length += length;

  if (!goodix_decode_pack (priv->data, priv->length, &flags, &payload,
                           &payload_len, &valid_checksum))
    {
      // Packet is not full, we still need data.
      fp_dbg ("not full packet");
      return;
    }

  switch (flags)
    {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      fp_dbg ("Got protocol msg");
      goodix_receive_protocol (dev, payload, payload_len);
      break;

    case GOODIX_FLAGS_TLS:
      fp_dbg ("Got TLS msg");
      goodix_receive_done (dev, payload, payload_len, NULL);
      // TLS message sending it to TLS server.
      break;

    case GOODIX_FLAGS_TLS_DATA:
        fp_dbg("Got TLS data msg");
        priv->reimage_tries = 0;
        // GOODIX 55x4: Remove first 9 to get valid TLS content
        goodix_receive_done(dev, payload+9, payload_len-9, NULL);
        break;

    default:
      fp_warn ("Unknown flags: 0x%02x", flags);
      break;
    }

  g_clear_pointer (&priv->data, g_free);
  priv->length = 0;
}
```

IN-side arming, `goodix_receive_data` + completion (`goodix.c:576-598,498-539`):

```c
void
goodix_receive_data (FpDevice *dev)
{
  ...
  if (priv->read_pending)
    // One IN URB at a time; the completion callback re-issues as needed.
    return;
  priv->read_pending = TRUE;

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (transfer, class->ep_in,
                              GOODIX_EP_IN_MAX_BUF_SIZE);
  fpi_usb_transfer_submit (transfer, 0, priv->transfer_cancel_tkn,
                           goodix_receive_data_cb, NULL);
}

void
goodix_receive_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer user_data, GError *error)
{
  ...
  priv->read_pending = FALSE;
  if (g_cancellable_is_cancelled (priv->transfer_cancel_tkn)) { ... return; }
  if (error) {
      // Retry only while a command is still waiting for data. ...
      if (priv->ack || priv->reply)
        goodix_receive_data (dev);
      return;
    }
  goodix_receive_pack (dev, transfer->buffer, transfer->actual_length);
  /* On-demand reads ...: only keep an IN transfer pending while a command
     still expects data. A perpetually-pending bulk-IN read on EP 0x82 makes
     the 55a4 firmware answer GET_IMAGE with REQUEST_TLS_CONNECTION (0xd0) ... */
  if (priv->ack || priv->reply)
    goodix_receive_data (dev);
}
```

Capture callback, `scan_on_read_img` (`goodix55x4.c:754-850`):

```c
static void scan_on_read_img(FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err) {
  g_print("SCAN_ON_READ_IMG\n");
  if (err) { fpi_ssm_mark_failed(ssm, err); return; }

  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);

  Goodix55X4Pix candidate[GOODIX55X4_FRAME_SIZE];
  decode_frame(candidate, data);

  double diff = frame_diff_score(candidate, self->empty_img);
  if (G_UNLIKELY(self->touch_diff <= 0.0)) {
    const gchar *env = g_getenv("GOODIX55X4_TOUCH_DIFF");
    self->touch_diff = env ? g_ascii_strtod(env, NULL) : 0.0;
    if (self->touch_diff <= 0.0)
      self->touch_diff = GOODIX55X4_FINGER_DETECT_DIFF_THRESHOLD;
    else
      fp_info("touch threshold overridden to %.0f", self->touch_diff);
  }
  g_print("Finger-detect poll: diff vs background = %.1f\n", diff);

  if (diff < self->touch_diff) {
    if (++self->poll_attempts >= GOODIX55X4_FINGER_DETECT_MAX_ATTEMPTS) {
      fpi_ssm_mark_failed(ssm, fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL));
      return;
    }
    fpi_device_add_timeout(dev, GOODIX55X4_FINGER_DETECT_POLL_DELAY_MS,
                           retry_finger_poll, ssm, NULL);
    return;
  }

  self->poll_attempts = 0;
  save_frame(self, data);

  /* Multi-frame: keep collecting held frames until we have CAP_FRAMES, then
     average them pixel-wise. ... CAP_FRAMES is 1, so this path never runs ... */
  if (g_slist_length(self->frames) < GOODIX55X4_CAP_FRAMES) {
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_MODE);
    return;
  }
  /* ... frame averaging block follows; handover in sec 6.c ... */
}
```

### 6.c Final frame handover to libfprint

Tail of `scan_on_read_img` (`goodix55x4.c:804-849`):

```c
  guint32 *acc = g_malloc0(GOODIX55X4_FRAME_SIZE * sizeof(guint32));
  int nframes = 0;
  for (GSList *l = self->frames; l; l = l->next) {
    Goodix55X4Pix *f = l->data;
    for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i)
      acc[i] += f[i];
    nframes++;
  }
  Goodix55X4Pix *avg = g_malloc(GOODIX55X4_FRAME_SIZE * sizeof(Goodix55X4Pix));
  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i)
    avg[i] = nframes ? acc[i] / nframes : 0;
  g_free(acc);
  g_print("Averaged %d frames\n", nframes);

  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);
  struct fpi_frame *aframe =
      g_malloc(GOODIX55X4_FRAME_SIZE + sizeof(struct fpi_frame));
  postprocess_frame(avg, self->empty_img);      // |frame - baseline|
  squash_frame_linear(avg, aframe->data);       // 12-bit -> 8-bit, interior p2..p98
  g_free(avg);

  GSList *frames = g_slist_append(NULL, aframe);
  struct fpi_frame_asmbl_ctx assembly_ctx;
  assembly_ctx.frame_width = GOODIX55X4_WIDTH;
  assembly_ctx.frame_height = GOODIX55X4_HEIGHT;
  assembly_ctx.image_width = GOODIX55X4_WIDTH;
  assembly_ctx.get_pixel = get_pix;
  fpi_do_movement_estimation(&assembly_ctx, frames);
  FpImage *img = fpi_assemble_frames(&assembly_ctx, frames);

  g_slist_free_full(frames, g_free);
  g_slist_free_full(self->frames, g_free);
  self->frames = NULL;

  g_print("Signal IMG Capture\n");
  fpi_image_device_image_captured(img_dev, img);   // <- handover

  // TRIED AND REVERTED: reporting the finger gone here ... measured 4/6 true
  // accept against 6/6 for waiting properly ... not worth ~1/3 of the accuracy.
  fpi_ssm_next_state(ssm);                          // -> FDT_MODE -> FDT_UP (lift wait)
}
```

---

*Keep in sync with `patches/libfprint-55a4-local-fixes.patch` when the patch
changes.*
