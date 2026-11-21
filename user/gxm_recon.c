#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

#include <taihen.h>

#include <stdio.h>
#include <string.h>

#include "gxm_recon.h"

#if defined(__has_include)
#if __has_include("gxm_nids.h")
#include "gxm_nids.h"
#define GXM_RECON_AVAILABLE 1
#endif
#endif

#ifndef GXM_RECON_AVAILABLE

void GxmReconInit(void) {}
void GxmReconExit(void) {}

#else

#define RECON_FLAG "ux0:data/adrenaline_gxm_recon.flag"
#define RECON_LOG "ux0:data/adrenaline_gxm_recon.txt"
#define MAX_KEYS 512
#define N_HOOKS 5

static SceUID hook_ids[N_HOOKS];
static int n_hooks;
static tai_hook_ref_t create_render_target_ref, color_surface_init_ref, depth_surface_init_ref,
    set_viewport_ref, set_region_clip_ref;

static uint32_t keys[MAX_KEYS];
static int n_keys;
static volatile int lock;

static uint32_t mix(uint32_t h, uint32_t v) {
  return (h ^ v) * 16777619u + 0x9E3779B9u;
}

static uint32_t f2u(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof(u));
  return u;
}

// Appends one line the first time this (kind, key) is seen. Never blocks the game:
// if another thread is logging right now the event is simply dropped.
static void logOnce(uint32_t key, const char *line) {
  if (__sync_lock_test_and_set(&lock, 1))
    return;

  int i, seen = 0;
  for (i = 0; i < n_keys; i++) {
    if (keys[i] == key) {
      seen = 1;
      break;
    }
  }

  if (!seen && n_keys < MAX_KEYS) {
    keys[n_keys++] = key;
    SceUID fd = sceIoOpen(RECON_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
      sceIoWrite(fd, line, strlen(line));
      sceIoClose(fd);
    }
  }

  __sync_lock_release(&lock);
}

static int createRenderTargetPatched(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget) {
  if (params) {
    char line[200];
    snprintf(line, sizeof(line), "[render target] %ux%u scenesPerFrame=%u msaa=%u flags=0x%x\n",
             params->width, params->height, params->scenesPerFrame, params->multisampleMode, params->flags);
    uint32_t k = mix(mix(mix(1, params->width), params->height), params->multisampleMode);
    logOnce(k, line);
  }
  return TAI_CONTINUE(int, create_render_target_ref, params, renderTarget);
}

static int colorSurfaceInitPatched(SceGxmColorSurface *surface, SceGxmColorFormat colorFormat,
                                   SceGxmColorSurfaceType surfaceType, SceGxmColorSurfaceScaleMode scaleMode,
                                   SceGxmOutputRegisterSize outputRegisterSize, unsigned int width,
                                   unsigned int height, unsigned int strideInPixels, void *data) {
  char line[200];
  snprintf(line, sizeof(line), "[color surface] %ux%u stride=%u format=0x%x type=%u scale=%u outreg=%u data=%p\n",
           width, height, strideInPixels, (unsigned)colorFormat, (unsigned)surfaceType, (unsigned)scaleMode,
           (unsigned)outputRegisterSize, data);
  uint32_t k = mix(mix(mix(mix(2, width), height), strideInPixels), (uint32_t)colorFormat);
  logOnce(k, line);
  return TAI_CONTINUE(int, color_surface_init_ref, surface, colorFormat, surfaceType, scaleMode,
                      outputRegisterSize, width, height, strideInPixels, data);
}

static int depthSurfaceInitPatched(SceGxmDepthStencilSurface *surface, SceGxmDepthStencilFormat format,
                                   SceGxmDepthStencilSurfaceType surfaceType, unsigned int strideInSamples,
                                   void *depthData, void *stencilData) {
  char line[200];
  snprintf(line, sizeof(line), "[depth surface] stride=%u format=0x%x type=%u depth=%p stencil=%p\n",
           strideInSamples, (unsigned)format, (unsigned)surfaceType, depthData, stencilData);
  uint32_t k = mix(mix(3, strideInSamples), (uint32_t)format);
  logOnce(k, line);
  return TAI_CONTINUE(int, depth_surface_init_ref, surface, format, surfaceType, strideInSamples, depthData,
                      stencilData);
}

static void setViewportPatched(SceGxmContext *context, float xOffset, float xScale, float yOffset, float yScale,
                               float zOffset, float zScale) {
  char line[200];
  snprintf(line, sizeof(line), "[viewport] xOff=%.2f xScale=%.2f yOff=%.2f yScale=%.2f zOff=%.4f zScale=%.4f\n",
           (double)xOffset, (double)xScale, (double)yOffset, (double)yScale, (double)zOffset, (double)zScale);
  uint32_t k = mix(mix(mix(mix(4, f2u(xOffset)), f2u(xScale)), f2u(yOffset)), f2u(yScale));
  logOnce(k, line);
  TAI_CONTINUE(void, set_viewport_ref, context, xOffset, xScale, yOffset, yScale, zOffset, zScale);
}

static void setRegionClipPatched(SceGxmContext *context, SceGxmRegionClipMode mode, unsigned int xMin,
                                 unsigned int yMin, unsigned int xMax, unsigned int yMax) {
  char line[200];
  snprintf(line, sizeof(line), "[region clip] mode=%u x=%u..%u y=%u..%u\n", (unsigned)mode, xMin, xMax, yMin, yMax);
  uint32_t k = mix(mix(mix(mix(mix(5, (uint32_t)mode), xMin), yMin), xMax), yMax);
  logOnce(k, line);
  TAI_CONTINUE(void, set_region_clip_ref, context, mode, xMin, yMin, xMax, yMax);
}

static tai_hook_ref_t *hook_refs[N_HOOKS];

static void appendLine(const char *line) {
  SceUID fd = sceIoOpen(RECON_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, line, strlen(line));
    sceIoClose(fd);
  }
}

static void install(const char *name, tai_hook_ref_t *ref, uint32_t nid, const void *func) {
  SceUID id = taiHookFunctionImport(ref, "ScePspemu", GXM_LIB_NID, nid, (void *)func);
  char line[160];
  if (id >= 0 && n_hooks < N_HOOKS) {
    hook_ids[n_hooks] = id;
    hook_refs[n_hooks++] = ref;
    snprintf(line, sizeof(line), "[hook] %s: installed\n", name);
  } else {
    snprintf(line, sizeof(line), "[hook] %s: NOT installed, error 0x%08x (ScePspemu does not import it?)\n", name,
             (unsigned)id);
  }
  appendLine(line);
}

void GxmReconInit(void) {
  SceUID fd = sceIoOpen(RECON_FLAG, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  sceIoClose(fd);

  // Fresh log for every launch
  fd = sceIoOpen(RECON_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

  install("sceGxmCreateRenderTarget", &create_render_target_ref, GXM_NID_sceGxmCreateRenderTarget,
          createRenderTargetPatched);
  install("sceGxmColorSurfaceInit", &color_surface_init_ref, GXM_NID_sceGxmColorSurfaceInit,
          colorSurfaceInitPatched);
  install("sceGxmDepthStencilSurfaceInit", &depth_surface_init_ref, GXM_NID_sceGxmDepthStencilSurfaceInit,
          depthSurfaceInitPatched);
  install("sceGxmSetViewport", &set_viewport_ref, GXM_NID_sceGxmSetViewport, setViewportPatched);
  install("sceGxmSetRegionClip", &set_region_clip_ref, GXM_NID_sceGxmSetRegionClip, setRegionClipPatched);
}

void GxmReconExit(void) {
  while (n_hooks > 0) {
    n_hooks--;
    taiHookRelease(hook_ids[n_hooks], *hook_refs[n_hooks]);
  }
}

#endif
