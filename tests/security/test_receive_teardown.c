// F6: disconnect-teardown receive state.
//
// Regression test: disconnect teardown race. Completions arriving after
// dev_deinit must be dropped cleanly instead of touching freed reassembly
// buffers. Covers both layers: deinit clears data/length/inited before
// teardown, and the receive callback drops completions when !inited.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "drivers/goodixtls/goodix.c"     // receive path + deinit contract
#include "drivers/goodixtls/goodix55x4.c" // concrete device type

static int failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL: %s\n", msg);                                    \
      failures++;                                                            \
    }                                                                        \
  } while (0)

int main(void) {
  FpiDeviceGoodixTls55X4 *gdev =
      g_object_new(fpi_device_goodixtls55x4_get_type(), NULL);
  FpDevice *dev = FP_DEVICE(gdev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private((FpiDeviceGoodixTls *)gdev);

  // Mid-session state as the field crash had it: a partial pack buffered
  // (non-NULL data, stale nonzero length), a command waiting for its reply,
  // a read in flight, device marked inited.
  priv->transfer_cancel_tkn = g_cancellable_new();
  priv->data = g_malloc(64);
  memset(priv->data, 0xa5, 64);
  priv->length = 42;
  priv->inited = TRUE;
  priv->read_pending = TRUE;
  priv->ack = TRUE;

  // Close. On a fixture there is no USB device behind the FpDevice, so
  // g_usb_device_release_interface () fails with a non-fatal GLib critical
  // and no GError - expected and harmless here.
  GError *error = NULL;
  goodix_dev_deinit(dev, &error);
  g_clear_error(&error);

  // Contract: no receive state may survive close. (On the unfixed tree:
  // data is the freed-but-non-NULL pointer and length is still 42.)
  CHECK(priv->data == NULL, "deinit must NULL priv->data (dangling buffer)");
  CHECK(priv->length == 0, "deinit must reset priv->length");
  CHECK(priv->inited == FALSE, "deinit must clear priv->inited");
  CHECK(priv->read_pending == FALSE, "deinit must clear priv->read_pending");

  // The field gate escape: the cancelled token reads as un-cancelled again
  // (goodix_start_read_loop () g_cancellable_reset()s it), and a straggler
  // completion carries no error (bulk-IN data that arrived before the
  // disconnect). On the unfixed tree this walks into goodix_receive_pack ()
  // on the freed buffer; the field build aborted with "double free or
  // corruption (out)".
  g_cancellable_reset(priv->transfer_cancel_tkn);

  FpiUsbTransfer *xfer = fpi_usb_transfer_new(dev);
  guint8 *xbuf = g_malloc0(32);
  xfer->buffer = xbuf;
  xfer->length = 32;
  xfer->actual_length = 32;
  goodix_receive_data_cb(xfer, dev, NULL, NULL);
  fpi_usb_transfer_unref(xfer);
  g_free(xbuf);

  // The gate must have dropped it: no fresh reassembly buffer may exist.
  CHECK(priv->data == NULL, "straggler completion must be dropped pre-receive_pack");
  CHECK(priv->length == 0, "straggler completion must not accumulate");

  // Consequence layer: even direct entry into goodix_receive_pack () after
  // close must be memory-safe - with data NULL it starts a fresh, bounded
  // buffer (ASan watches every allocation here).
  guint8 chunk[32];
  memset(chunk, 0x5a, sizeof(chunk));
  goodix_receive_pack(dev, chunk, sizeof(chunk));
  CHECK(priv->length <= GOODIX_EP_IN_MAX_BUF_SIZE,
        "reassembly stays bounded after late entry");
  g_clear_pointer(&priv->data, g_free);
  priv->length = 0;

  g_object_unref(priv->transfer_cancel_tkn);
  priv->transfer_cancel_tkn = NULL;
  g_object_unref(gdev);

  if (failures) {
    fprintf(stderr, "F6: %d check(s) failed\n", failures);
    return 1;
  }
  printf("F6: all checks passed\n");
  return 0;
}
