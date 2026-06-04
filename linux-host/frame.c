/* Frame encode: capture (any size) -> scale to PSP 480x272 -> baseline JPEG,
   with optional 90/180/270 rotation. Plus a cheap content hash. */
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
#include "pspdisp.h"

/* Scale one output pixel from the capture. The host does NOT rotate pixels:
   it only scales the source into the wire image (dims swapped to 272x480 for
   90/270, matching the Windows app's StretchBlt). The PSP performs the actual
   geometric rotation per the rotation flag (psp graphicDrawFrame). Rotating
   here too would double-rotate. */
static void sample(capture_backend *cap, int outCol, int outRow, int outW, int outH,
                   uint8_t *rgb)
{
  int sx = (int)((int64_t)outCol * cap->width  / outW);
  int sy = (int)((int64_t)outRow * cap->height / outH);
  if (sx >= cap->width)  sx = cap->width  - 1;
  if (sy >= cap->height) sy = cap->height - 1;
  cap->pixel(sx, sy, &rgb[0], &rgb[1], &rgb[2]);
}

unsigned char *frame_encode(capture_backend *cap, unsigned long *out_size, uint32_t *out_flags)
{
  bool swap = (g_opt.rotation == 90 || g_opt.rotation == 270);
  int outW = swap ? PSP_H : PSP_W;
  int outH = swap ? PSP_W : PSP_H;

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  unsigned char *buf = NULL; unsigned long size = 0;
  jpeg_mem_dest(&cinfo, &buf, &size);
  cinfo.image_width = outW; cinfo.image_height = outH;
  cinfo.input_components = 3; cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, g_opt.quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  uint8_t *row = malloc((size_t)outW * 3);
  while (cinfo.next_scanline < cinfo.image_height) {
    for (int c = 0; c < outW; c++)
      sample(cap, c, cinfo.next_scanline, outW, outH, &row[c * 3]);
    JSAMPROW r = row;
    jpeg_write_scanlines(&cinfo, &r, 1);
  }
  free(row);
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  uint32_t flags = COM_FLAGS_CONTAINS_IMAGE_DATA | COM_FLAGS_IMAGE_IS_JPEG;
  if (g_opt.rotation == 90)  flags |= COM_FLAGS_IMAGE_IS_ROTATED_90;
  if (g_opt.rotation == 180) flags |= COM_FLAGS_IMAGE_IS_ROTATED_180;
  if (g_opt.rotation == 270) flags |= COM_FLAGS_IMAGE_IS_ROTATED_270;
  *out_flags = flags;
  *out_size = size;
  return buf;
}

/* FNV-1a over a sparse grid of the capture, for skip-unchanged detection. */
uint64_t frame_hash(capture_backend *cap)
{
  uint64_t h = 1469598103934665603ULL;
  int step_x = cap->width  / 64; if (step_x < 1) step_x = 1;
  int step_y = cap->height / 64; if (step_y < 1) step_y = 1;
  for (int y = 0; y < cap->height; y += step_y)
    for (int x = 0; x < cap->width; x += step_x) {
      uint8_t r, g, b;
      cap->pixel(x, y, &r, &g, &b);
      h = (h ^ r) * 1099511628211ULL;
      h = (h ^ g) * 1099511628211ULL;
      h = (h ^ b) * 1099511628211ULL;
    }
  return h;
}
