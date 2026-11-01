// Security harness: USB-layer parsing for the Goodix TLS driver.
//
// Everything behind the bulk-IN endpoint is device-controlled. Modes
// (selected by data[0] % 4):
//
//   0  reassembly: feed the input as two successive chunks through
//      goodix_receive_pack(); exercises header parsing, accumulation and
//      the overflow cap. Invariant after every input:
//      priv->length <= GOODIX_EP_IN_MAX_BUF_SIZE (the S-01 property that
//      also keeps the guint32 length arithmetic past any wrap).
//
//   1  dispatch-deep: goodix_receive_protocol() with priv->cmd forced to
//      the frame's command byte, so a crafted frame reaches
//      goodix_receive_done() -> reset_state -> callback. (The 0xd0
//      re-image branch is deliberately unreachable here: it requires
//      priv->cmd == GET_IMAGE while the frame command is 0xd0.)
//
//   2  parser dispatch: direct calls into each reply parser
//      (firmware version / none / default / success / reset / PSK read /
//      PSK write) with the fuzzed payload and a benign callback - the
//      length guards of every parser are exercised.
//
//   3  goodix_decode_protocol() invariant: when it reports success,
//      payload_len must be <= data_len.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "drivers/goodixtls/goodix.c"     // receive path + private state
#include "drivers/goodixtls/goodix55x4.c" // concrete device type

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static FpiDeviceGoodixTls55X4 *gdev;

/* Benign callbacks for each reply-parser signature. */
static void stub_cmd(FpDevice *d, guint8 *b, guint16 l, gpointer u,
                     GError *e) {
  (void)d; (void)b; (void)l; (void)u; (void)e;
}
static void stub_fw(FpDevice *d, gchar *fw, gpointer u, GError *e) {
  (void)d; (void)fw; (void)u; (void)e;
}
static void stub_none(FpDevice *d, gpointer u, GError *e) {
  (void)d; (void)u; (void)e;
}
static void stub_default(FpDevice *d, guint8 *b, guint16 l, gpointer u,
                         GError *e) {
  (void)d; (void)b; (void)l; (void)u; (void)e;
}
static void stub_success(FpDevice *d, gboolean ok, gpointer u, GError *e) {
  (void)d; (void)ok; (void)u; (void)e;
}
static void stub_reset(FpDevice *d, gboolean ok, guint16 num, gpointer u,
                       GError *e) {
  (void)d; (void)ok; (void)num; (void)u; (void)e;
}
static void stub_psk_read(FpDevice *d, gboolean ok, guint32 flags,
                          guint8 *psk, guint16 len, gpointer u, GError *e) {
  (void)d; (void)ok; (void)flags; (void)psk; (void)len; (void)u; (void)e;
}

static GoodixCallbackInfo *cb_info_new(GoodixCmdCallback cb) {
  GoodixCallbackInfo *ci = g_new0(GoodixCallbackInfo, 1);
  ci->callback = (GCallback)cb;
  ci->user_data = NULL;
  return ci;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  gdev = g_object_new(fpi_device_goodixtls55x4_get_type (), NULL);
  if (!gdev) {
    fprintf(stderr, "fixture: device construction failed\n");
    exit(2);
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FpDevice *dev = FP_DEVICE(gdev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private((FpiDeviceGoodixTls *)gdev);
  const uint8_t *p;
  size_t n;
  uint8_t mode;

  /* Reset per-input device state. */
  g_clear_pointer(&priv->data, g_free);
  priv->length = 0;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->cmd = 0;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->reimage_tries = 0;

  if (size < 1)
    return 0;
  mode = data[0] % 4;
  p = data + 1;
  n = size - 1;

  switch (mode) {
  case 0: { /* reassembly, two chunks */
    if (n >= 2) {
      size_t c1 = n / 2;
      goodix_receive_pack(dev, (guint8 *)p, (guint32)c1);
      goodix_receive_pack(dev, (guint8 *)p + c1, (guint32)(n - c1));
    } else if (n == 1) {
      goodix_receive_pack(dev, (guint8 *)p, 1);
    }
    break;
  }
  case 1: { /* dispatch-deep */
    if (n >= 1) {
      priv->cmd = p[0];
      priv->reply = TRUE;
      priv->ack = TRUE;
      priv->callback = (GoodixCmdCallback)stub_cmd;
      priv->user_data = NULL;
      goodix_receive_protocol(dev, (guint8 *)p, (guint32)n);
    }
    break;
  }
  case 2: { /* direct reply parsers */
    guint sel;
    const guint8 *pay;
    guint16 plen;
    if (n < 1)
      break;
    sel = p[0];
    pay = p + 1;
    plen = (n - 1) > G_MAXUINT16 ? G_MAXUINT16 : (guint16)(n - 1);
    switch (sel % 7) {
    case 0:
      goodix_receive_firmware_version(dev, (guint8 *)pay, plen,
                                      cb_info_new((GoodixCmdCallback)stub_fw), NULL);
      break;
    case 1:
      goodix_receive_none(dev, (guint8 *)pay, plen,
                          cb_info_new((GoodixCmdCallback)stub_none), NULL);
      break;
    case 2:
      goodix_receive_default(dev, (guint8 *)pay, plen,
                             cb_info_new((GoodixCmdCallback)stub_default), NULL);
      break;
    case 3:
      goodix_receive_success(dev, (guint8 *)pay, plen,
                             cb_info_new((GoodixCmdCallback)stub_success), NULL);
      break;
    case 4:
      goodix_receive_reset(dev, (guint8 *)pay, plen,
                           cb_info_new((GoodixCmdCallback)stub_reset), NULL);
      break;
    case 5:
      goodix_receive_preset_psk_read(dev, (guint8 *)pay, plen,
                                     cb_info_new((GoodixCmdCallback)stub_psk_read),
                                     NULL);
      break;
    case 6:
      goodix_receive_preset_psk_write(dev, (guint8 *)pay, plen,
                                      cb_info_new((GoodixCmdCallback)stub_success),
                                      NULL);
      break;
    }
    break;
  }
  case 3: { /* decode_protocol invariant */
    if (n >= 4) {
      guint8 cmd;
      guint8 *payload = NULL;
      guint16 payload_len;
      gboolean vc, vn;
      if (goodix_decode_protocol((guint8 *)p, (guint32)n, &cmd, &payload,
                                 &payload_len, &vc, &vn)) {
        if (payload_len > n) {
          fprintf(stderr,
                  "INVARIANT 3 violated: payload_len %u > data_len %zu\n",
                  payload_len, n);
          abort();
        }
        g_free(payload);
      }
    }
    break;
  }
  }

  /* S-01 property: the reassembly buffer never exceeds the largest legal
     pack, no matter how chunks are split. */
  if (priv->length > GOODIX_EP_IN_MAX_BUF_SIZE) {
    fprintf(stderr, "INVARIANT 0 violated: reassembly length %u > cap\n",
            priv->length);
    abort();
  }

  g_clear_pointer(&priv->data, g_free);
  priv->length = 0;
  return 0;
}
