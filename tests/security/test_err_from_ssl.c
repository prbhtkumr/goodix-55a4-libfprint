// Security regression test: TLS error conversion (err_from_ssl).
//
// The driver converts OpenSSL failures into GErrors at every TLS entry
// point (SSL_accept, SSL_read). The sensor controls when those fire. The
// original implementation:
//   * hand-malloc'd the GError, leaving .domain uninitialized (invalid
//     quark handed to GLib),
//   * strcpy'd strlen(msg)+1 bytes into a strlen(msg) buffer - a
//     one-byte heap overflow on every TLS error,
//   * crashed in strlen(NULL) when the OpenSSL error queue was empty
//     (the common WANT_READ / SYSCALL path).
//
// Runs both cases under ASan and validates the resulting GError.

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "drivers/goodixtls/goodixtls.c"

int
main (void)
{
  int fails = 0;

  /* Load libssl's reason strings so ERR_reason_error_string() can resolve
   * manually-raised codes. In production the driver creates an SSL_CTX,
   * which initialises libssl the same way. */
  OPENSSL_init_ssl (OPENSSL_INIT_LOAD_SSL_STRINGS, NULL);

  /* Case 1: empty error queue. */
  ERR_clear_error ();
  GError *e = err_from_ssl ();
  if (!e)
    {
      fprintf (stderr, "FAIL case1: NULL error\n");
      fails++;
    }
  else
    {
      printf ("case1: domain-registered=%d msg=%s\n",
              e->domain != 0, e->message ? e->message : "(null)");
      if (!e->message)
        fails++;
      g_error_free (e);
    }

  /* Case 2: a real SSL error on the queue. */
  ERR_raise (ERR_LIB_SSL, SSL_R_WRONG_VERSION_NUMBER);
  e = err_from_ssl ();
  if (!e)
    {
      fprintf (stderr, "FAIL case2: NULL error\n");
      fails++;
    }
  else
    {
      printf ("case2: msg=%s\n", e->message ? e->message : "(null)");
      if (!e->message || !strstr (e->message, "version"))
        {
          fprintf (stderr, "FAIL case2: reason string missing\n");
          fails++;
        }
      g_error_free (e);
    }

  if (fails)
    {
      printf ("F4 FAIL (%d)\n", fails);
      return 1;
    }
  printf ("F4 PASS\n");
  return 0;
}
