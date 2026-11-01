// Security harness: SIGFM print-blob deserialization.
//
// Enrolled templates are handed back to the driver by fprintd on every
// verify (fp_print_deserialize -> sigfm_deserialize_binary). A crafted or
// corrupted print file must not cause out-of-bounds reads: every length
// field inside the blob is attacker-influenced, so the stream reader has
// to bounds-check before consuming.
//
// Invariants:
//   * deserialize returns either a fully-consistent SigfmImgInfo or NULL
//     (all parse failures are caught, no exception crosses the C ABI)
//   * a successful deserialize followed by re-serialize must not crash
//     and must produce a buffer that can be free()d

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sigfm.hpp"
#include "img-info.hpp" // complete SigfmImgInfo for the empty seed

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > (size_t)INT32_MAX)
    return 0;

  SigfmImgInfo *info = sigfm_deserialize_binary(data, (int)size);
  if (info) {
    int outlen = 0;
    unsigned char *buf = sigfm_serialize_binary(info, &outlen);
    free(buf); // copy_buffer() allocates with malloc()
    sigfm_free_info(info);
  }
  return 0;
}

static void write_seed(const char *dir, const char *name,
                       const unsigned char *b, int n) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "seed: cannot open %s\n", path);
    exit(2);
  }
  fwrite(b, 1, (size_t)n, f);
  fclose(f);
}

// --make-seed <dir>: write two well-formed blobs (empty template, and one
// extracted from a synthetic image) to use as the fuzz corpus starting
// point, then exit. Called by run.sh.
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) {
  if (*argc >= 3 && strcmp((*argv)[1], "--make-seed") == 0) {
    const char *dir = (*argv)[2];

    // Seed A: an empty (but valid) template.
    SigfmImgInfo empty;
    int n = 0;
    unsigned char *b = sigfm_serialize_binary(&empty, &n);
    write_seed(dir, "seed-empty.bin", b, n);
    free(b);

    // Seed B: a template extracted from a deterministic synthetic image
    // (SIFT finds corners in this pattern), so vectors/Mat carry content.
    const int w = 108, h = 88;
    std::vector<SfmPix> img((size_t)w * h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        img[(size_t)y * w + x] = (SfmPix)(((x * 13 + y * 7) % 64) * 4);

    SigfmImgInfo *info = sigfm_extract(img.data(), w, h);
    if (info) {
      n = 0;
      b = sigfm_serialize_binary(info, &n);
      write_seed(dir, "seed-extracted.bin", b, n);
      free(b);
      sigfm_free_info(info);
    }

    exit(0);
  }
  return 0;
}
