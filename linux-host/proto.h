/* PSPdisp wire protocol: mirrors psp/source/shared.h. Little-endian. */
#ifndef PSPDISP_PROTO_H
#define PSPDISP_PROTO_H
#include <stdint.h>

#define COM_HEADER_MAGIC                 0xAFFE0600u
#define HOSTFS_MAGIC                     0x782F0812u   /* USB connect handshake */

#define COM_FLAGS_CONTAINS_IMAGE_DATA    0x00000001u
#define COM_FLAGS_CONTAINS_AUDIO_DATA    0x00000002u
#define COM_FLAGS_CONTAINS_SETTINGS_DATA 0x00000004u
#define COM_FLAGS_ENABLE_OSK             0x00000008u
#define COM_FLAGS_IMAGE_IS_JPEG          0x00000010u
#define COM_FLAGS_IMAGE_IS_PNG           0x00000020u
#define COM_FLAGS_IMAGE_IS_UNCOMPRESSED  0x00000040u
#define COM_FLAGS_IMAGE_IS_ROTATED_90    0x00000100u
#define COM_FLAGS_IMAGE_IS_ROTATED_180   0x00000200u
#define COM_FLAGS_IMAGE_IS_ROTATED_270   0x00000400u
#define COM_FLAGS_AUDIO_11025_HZ         0x00001000u
#define COM_FLAGS_AUDIO_22050_HZ         0x00002000u
#define COM_FLAGS_AUDIO_44100_HZ         0x00004000u
#define COM_FLAGS_AUDIO_CHUNK_2240       0x00010000u
#define COM_FLAGS_AUDIO_CHUNK_2688       0x00020000u
#define COM_FLAGS_IMAGE_CLEAR_SCREEN     0x00100000u
#define COM_FLAGS_FORCE_UPDATE           0x01000000u

#define PSP_W 480
#define PSP_H 272

#define NET_PORT 17584          /* WLAN/TCP base port (psp/source/wlan.h) */
#define NET_PASSWORD_LEN 32

/* PSP receive buffer (psp/source/com.h). The PSP reads image+audio+settings
   into this with no bounds check, so the host must never exceed it. */
#define COM_IMAGE_BUFFER_SIZE (400 * 1024)

typedef struct {
  uint32_t magic, flags, imageSize, settingsSize;
} __attribute__((packed)) FrameHeader;

typedef struct {
  uint32_t magic, flags, buttons;
  uint8_t  analogX, analogY;
} __attribute__((packed)) ResponseHeader;            /* 14 bytes */

/* USB mode: PSP appends 19*u32 settings after the header -> 90 bytes total. */
#define RESPONSE_FULL_SIZE (sizeof(ResponseHeader) + 19 * 4)

/* PSP button bitmasks (pspctrl.h) */
enum {
  PSP_SELECT = 0x000001, PSP_START = 0x000008,
  PSP_UP = 0x000010, PSP_RIGHT = 0x000020, PSP_DOWN = 0x000040, PSP_LEFT = 0x000080,
  PSP_LTRIG = 0x000100, PSP_RTRIG = 0x000200,
  PSP_TRIANGLE = 0x001000, PSP_CIRCLE = 0x002000, PSP_CROSS = 0x004000, PSP_SQUARE = 0x008000,
};

#endif
