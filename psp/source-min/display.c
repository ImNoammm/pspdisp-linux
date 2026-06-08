/*
  pspdisp-min — GU display.
*/
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include "display.h"
#include "shared.h"

#define BUF_WIDTH  512
#define SCR_WIDTH  480
#define SCR_HEIGHT 272
#define PIXEL_SIZE 4
#define FRAME_OFF  (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

static unsigned int __attribute__((aligned(16))) l_list[262144];
static ScePspFVector3 l_center = { 240.0f, 136.0f, 0.0f };

/* VRAM offset of the current draw buffer. Starts at 0 (matches sceGuDrawBuffer
   in displayInit) and flips on every swap, in lockstep with sceGuSwapBuffers. */
static int l_drawOff = 0;

typedef struct
{
  float u, v;
  unsigned int color;
  float x, y, z;
} texturedVertex;

void displayInit(void)
{
  sceGuInit();
  sceGuStart(GU_DIRECT, l_list);
  sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
  sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)(BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE), BUF_WIDTH);
  sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
  sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
  sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
  sceGuDepthRange(0xc350, 0x2710);
  sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuDepthFunc(GU_GEQUAL);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuFrontFace(GU_CW);
  sceGuShadeModel(GU_SMOOTH);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  sceGuFinish();
  sceGuSync(0, 0);
  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);
}

void displayClear(unsigned int color)
{
  sceGuStart(GU_DIRECT, l_list);
  sceGuClearColor(color);
  sceGuClearDepth(0);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
  sceGuFinish();
  sceGuSync(0, 0);
}

void displaySwap(void)
{
  sceGuSwapBuffers();
  l_drawOff ^= FRAME_OFF;
}

unsigned int displayDrawAddr(void)
{
  /* PSP VRAM is a fixed 2 MB at physical 0x04000000 on every model; 0x44000000
     is its uncached alias, so CPU text writes are seen immediately. */
  return 0x44000000u + l_drawOff;
}

static void setupOrtho(void)
{
  sceGumMatrixMode(GU_PROJECTION);
  sceGumLoadIdentity();
  sceGumOrtho(0, 480, 272, 0, -1, 1);
  sceGumMatrixMode(GU_VIEW);
  sceGumLoadIdentity();
  sceGumMatrixMode(GU_MODEL);
  sceGumLoadIdentity();
}

/* The PSP texture sampler maxes out at 512 wide, so the 480-wide frame is blit
   in 32px vertical slices (the classic PSP "advanceBlit"). */
static void slicedBlit(float startX, float startY, float imageWidth)
{
  sceGuTexScale(1.0f / 512.0f, 1.0f / 512.0f);

  for (float xPos = 0.0f; xPos < imageWidth; xPos += 32.0f)
  {
    texturedVertex *v = (texturedVertex *)sceGuGetMemory(4 * sizeof(texturedVertex));
    v[0].color = v[1].color = v[2].color = v[3].color = 0xFFFFFFFF;
    v[0].u = xPos;          v[0].v = 0;      v[0].x = startX + xPos;          v[0].y = startY;          v[0].z = 0;
    v[1].u = xPos + 32.0f;  v[1].v = 0;      v[1].x = startX + xPos + 32.0f;  v[1].y = startY;          v[1].z = 0;
    v[2].u = xPos + 32.0f;  v[2].v = 512.0f; v[2].x = startX + xPos + 32.0f;  v[2].y = startY + 512.0f; v[2].z = 0;
    v[3].u = xPos;          v[3].v = 512.0f; v[3].x = startX + xPos;          v[3].y = startY + 512.0f; v[3].z = 0;
    sceGumDrawArray(GU_TRIANGLE_FAN,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    4, 0, v);
  }

  sceGuTexScale(1.0f, 1.0f);
}

void displayDrawFrame(unsigned int *pixels, unsigned int rotation)
{
  float imageWidth = 480.0f, imageHeight = 272.0f, rotate = 0.0f;

  switch (rotation)
  {
    case COM_FLAGS_IMAGE_IS_ROTATED_90_DEG:  rotate = GU_PI * 0.5f; imageWidth = 272.0f; imageHeight = 480.0f; break;
    case COM_FLAGS_IMAGE_IS_ROTATED_180_DEG: rotate = GU_PI;        break;
    case COM_FLAGS_IMAGE_IS_ROTATED_270_DEG: rotate = GU_PI * 1.5f; imageWidth = 272.0f; imageHeight = 480.0f; break;
    default: break;
  }

  sceGuStart(GU_DIRECT, l_list);

  setupOrtho();
  sceGumTranslate(&l_center);
  sceGumRotateZ(rotate);

  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexImage(0, 512, 512, imageWidth, (void *)pixels);

  slicedBlit(-0.5f * imageWidth, -0.5f * imageHeight, imageWidth);

  sceGuFinish();
  sceGuSync(0, 0);
}
