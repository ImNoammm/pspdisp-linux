/* PC audio capture -> PSP PCM frames via PulseAudio simple API.
   UNTESTED: framing follows psp/source/audio.c (22050 Hz, chunk 2688), but
   A/V sync was not validated on hardware. Off by default (-a to enable).

   Pick a monitor source so you capture playback, not the mic, e.g.:
     PSPDISP_AUDIO_SOURCE=$(pactl get-default-sink).monitor pspdisp -a   */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include "pspdisp.h"

/* Match the Windows app exactly (MainLoop.pas): 22050 Hz is paired with the
   2240 chunk, so the PSP reads chunk*2 = 4480 bytes of S16 stereo per frame. */
#define RATE   22050
#define CHUNK  2240
#define FRAME_BYTES (CHUNK * 2)          /* bytes PSP reads per video frame */

static pa_simple *pa;

bool audio_init(void)
{
  pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = 2 };
  const char *dev = getenv("PSPDISP_AUDIO_SOURCE");   /* NULL = default source */
  pa_buffer_attr ba = { .maxlength = (uint32_t)-1, .fragsize = FRAME_BYTES };
  int err = 0;
  pa = pa_simple_new(NULL, "PSPdisp", PA_STREAM_RECORD, dev, "screen",
                     &ss, NULL, &ba, &err);
  if (!pa) { fprintf(stderr, "audio: pa_simple_new failed: %s\n", pa_strerror(err)); return false; }
  VLOG("audio: capturing %s @ %dHz\n", dev ? dev : "(default source)", RATE);
  return true;
}

int audio_read_frame(uint8_t *dst, int max, uint32_t *audio_flags)
{
  if (!pa || max < FRAME_BYTES) return 0;
  int err = 0;
  if (pa_simple_read(pa, dst, FRAME_BYTES, &err) < 0) {
    VLOG("audio: read error: %s\n", pa_strerror(err));
    return 0;
  }
  *audio_flags = COM_FLAGS_CONTAINS_AUDIO_DATA | COM_FLAGS_AUDIO_22050_HZ | COM_FLAGS_AUDIO_CHUNK_2240;
  return FRAME_BYTES;
}

void audio_term(void)
{
  if (pa) { pa_simple_free(pa); pa = NULL; }
}
