/* F5: SigfmImgInfo ownership transfer between FpPrint and FpImage.
 *
 * Regression test: fpi_print_add_from_image() must take ownership of
 * image->sigfm_info (the fix nulls the image's pointer). Without it,
 * fp_print_finalize() and fp_image_finalize() both free the same heap
 * block and fprintd aborts on every completed match. Three teardown
 * orders are covered below.
 */

/* Include order matters: fp-device.h/fp-print.h form an include cycle that
 * only resolves when the chain enters through fpi-print.h (or fp-device.h)
 * first - fp-device.h:225 needs FpFingerStatusFlags, which fp-print.h
 * defines after its own include of fp-device.h. */
#include "fp-image.h"
#include "fpi-image.h"
/* fpi-print.h uses C's 'template' as a parameter name (fpi_print_sigfm_match);
 * rename it so the header parses as C++. The parameter name has no effect on
 * the ABI. */
#define template template_
#include "fpi-print.h"
#undef template
#include "fp-print.h"
/* sigfm.hpp only forward-declares SigfmImgInfo; the definition (keypoints
 * vector + descriptor Mat) and OpenCV live here. */
#include "sigfm/img-info.hpp"

#include <cstdio>

static FpPrint *
make_sigfm_print (void)
{
  FpPrint *p = (FpPrint *) g_object_new (FP_TYPE_PRINT,
                                         "driver", "goodixtls55x4",
                                         "device-id", "f5-test",
                                         NULL);
  if (!p)
    return NULL;
  fpi_print_set_type (p, FPI_PRINT_SIGFM);
  return p;
}

static FpImage *
make_image_with_info (void)
{
  FpImage *img = fp_image_new (108, 88);
  img->sigfm_info = new SigfmImgInfo ();
  /* Non-empty backing buffer: the production crash was a vector<KeyPoint>
   * buffer freed twice, not just the wrapper object. */
  img->sigfm_info->keypoints.push_back (cv::KeyPoint (1.f, 2.f, 3.f));
  return img;
}

static int
attach (FpPrint *pr, FpImage *img)
{
  GError *err = NULL;
  if (!fpi_print_add_from_image (pr, img, &err))
    {
      std::fprintf (stderr, "add_from_image failed: %s\n",
                    err ? err->message : "no error?");
      g_clear_error (&err);
      return 2;
    }
  if (img->sigfm_info != NULL)
    {
      std::fprintf (stderr,
                    "FAIL: image still owns sigfm_info after add - the print's "
                    "prints array holds the same pointer, both will free it\n");
      return 1;
    }
  return 0;
}

int
main (void)
{
  int rc;

  /* Case 1: production order - image ref dropped first; the print holds
   * the last ref, so fp_print_finalize tears the image down (no-op) and
   * then frees the prints array (exactly one owner). */
  {
    FpImage *img = make_image_with_info ();
    FpPrint *pr = make_sigfm_print ();
    if (!pr)
      {
        std::fprintf (stderr, "cannot create FpPrint\n");
        return 2;
      }
    if ((rc = attach (pr, img)) != 0)
      return rc;
    g_object_unref (img);
    g_object_unref (pr);
  }

  /* Case 2: reversed order - print finalizes first (frees the stolen
   * info), image later finalizes against a NULL field. */
  {
    FpImage *img = make_image_with_info ();
    FpPrint *pr = make_sigfm_print ();
    if (!pr)
      return 2;
    if ((rc = attach (pr, img)) != 0)
      return rc;
    g_object_unref (pr);
    g_object_unref (img);
  }

  /* Case 3: never attached - fp_image_finalize must still free its own
   * info (the leak fix this must not regress). */
  {
    FpImage *img = make_image_with_info ();
    g_object_unref (img);
  }

  std::printf ("F5 ownership: all teardown orders free each SigfmImgInfo "
               "exactly once\n");
  return 0;
}
