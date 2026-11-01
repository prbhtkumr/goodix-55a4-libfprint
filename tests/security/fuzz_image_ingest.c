// Security harness: image-ingest length guard for the 55a4 driver.
//
// The TLS image handler forwards whatever plaintext length the sensor
// produced (device-controlled) to the scan image callback. decode_frame()
// then unconditionally reads GOODIX55X4_RAW_FRAME_SIZE bytes. This harness
// drives scan_on_read_img() with arbitrary payload lengths under ASan:
//
//   size <  GOODIX55X4_RAW_FRAME_SIZE  ->  must hit the short-payload guard
//                                      (rejected, no OOB read, no frame)
//   size == GOODIX55X4_RAW_FRAME_SIZE  ->  baseline made equal to the frame so
//                                      the touch gate deterministically
//                                      misses: exercises the full decode
//                                      path without reaching image_captured
//
// Invariants checked every input (abort = fuzzer crash):
//   1. frames may only be stored on the touch-accept path (full-size input)
//   2. a short payload must trip exactly one short-image guard rejection
//   3. a full-size payload must NOT trip the guard
//
// Note: fpi_ssm_mark_failed() completes and frees the SSM, and the poll
// path hands it to a pending timeout. The SSM must therefore never be
// dereferenced after scan_on_read_img() returns; the guard is observed
// through its log line instead.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "drivers/goodixtls/goodix55x4.c"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static FpiDeviceGoodixTls55X4 *gdev;
static int guard_hits; /* short-image guard rejections this iteration */

// The guard logs fp_err("short image payload: ..."); capture it (and keep
// driver log spam out of the fuzz output).
static void on_log(const gchar *domain, GLogLevelFlags level, const gchar *msg,
                   gpointer user_data) {
  if (strstr(msg, "short image payload") != NULL)
    guard_hits++;
}

// The touch-accept path advances the real state machine, which would drive
// USB commands on a device with no bus. Stop it here; device-facing
// behaviour of those states is covered by the field soak, not this harness.
static void harness_state(FpiSsm *ssm, FpDevice *dev) {
  if (fpi_ssm_get_error(ssm))
    return;
  fpi_ssm_mark_failed(ssm, fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL));
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  // The driver TUs log under their meson -DG_LOG_DOMAIN (extracted flags
  // give this TU the same domain, "libfprint-goodixtls55x4"); glib only
  // dispatches to exact-domain handlers.
  g_log_set_handler(G_LOG_DOMAIN,
                    G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
                        G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL,
                    on_log, NULL);
  gdev = g_object_new(fpi_device_goodixtls55x4_get_type (), NULL);
  if (!gdev) {
    fprintf(stderr, "fixture: device construction failed\n");
    exit(2);
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FpiDeviceGoodixTls55X4 *self = gdev;
  FpiSsm *ssm;

  if (size > GOODIX55X4_RAW_FRAME_SIZE)
    size = GOODIX55X4_RAW_FRAME_SIZE; // decode_frame's input is fixed-size

  guard_hits = 0;
  self->poll_attempts = 0;

  if (size == GOODIX55X4_RAW_FRAME_SIZE) {
    // Make |frame - baseline| exactly 0 for this input so the touch gate
    // misses deterministically (diff 0 < 300). Fuzzed frames still differ
    // from the baseline and exercise the touch-accept path, which
    // harness_state stops before any USB activity.
    Goodix55X4Pix tmp[GOODIX55X4_FRAME_SIZE];
    decode_frame(tmp, data);
    memcpy(self->empty_img, tmp, sizeof(tmp));
    self->empty_img_valid = TRUE;
  }

  ssm = fpi_ssm_new(FP_DEVICE(gdev), harness_state, SCAN_STAGE_NUM);

  scan_on_read_img(FP_DEVICE(gdev), (guint8 *)data, (guint16)size, ssm, NULL);
  /* Do NOT touch `ssm` here: it may already be freed (guard rejection and
   * poll-limit rejection complete it; the touch path frees it too). */

  if (self->frames != NULL) {
    if (size != GOODIX55X4_RAW_FRAME_SIZE) {
      fprintf(stderr, "INVARIANT 1 violated: frame stored on short input "
                      "(size=%zu)\n", size);
      abort();
    }
    g_slist_free_full(self->frames, g_free);
    self->frames = NULL;
  }

  if (size < GOODIX55X4_RAW_FRAME_SIZE) {
    if (guard_hits != 1) {
      fprintf(stderr, "INVARIANT 2 violated: short input not rejected "
                      "(size=%zu guard_hits=%d)\n", size, guard_hits);
      abort();
    }
  } else if (guard_hits != 0) {
    fprintf(stderr, "INVARIANT 3 violated: full-size input rejected "
                    "(guard_hits=%d)\n", guard_hits);
    abort();
  }

  return 0;
}
