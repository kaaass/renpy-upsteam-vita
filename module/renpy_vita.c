#include "renpy_vita.h"

#include "psp2/kernel/processmgr.h"
#include "psp2/sysmodule.h"
#include "psp2/avplayer.h"
#include "psp2/kernel/sysmem.h"
#include "psp2/gxm.h"
#include <pygame_sdl2/pygame_sdl2.h>
#include <libswscale/swscale.h>

#define FRAMEBUFFER_ALIGNMENT 0x40000

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))
#endif

int RPVITA_exit_process(int res) {
    sceKernelExitProcess(res);
}

/* Video Player */

static void convert_nv12_to_rgba(uint8_t *, uint8_t *, int, int);
static void *mem_alloc(void *p, uint32_t alignment, uint32_t size);
static void mem_free(void *p, void* pMemory);
static void *gpu_alloc(void* p, uint32_t alignment, uint32_t size);
static void gpu_free(void* p, void* pMemory);

enum {
  PLAYER_INACTIVE,
  PLAYER_ACTIVE,
  PLAYER_STOP,
};

SceAvPlayerHandle movie_player;
int player_state = PLAYER_INACTIVE;

void RPVITA_video_init() {
    import_pygame_sdl2();
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
}

#define MAX_WIDTH 960
#define MAX_HEIGHT 544
#define NEON_ALIGN 0x80
static uint8_t _video_buffer[MAX_WIDTH * MAX_HEIGHT * 4 + NEON_ALIGN];
#define video_buffer ((uint8_t *) ALIGN((uintptr_t) _video_buffer, NEON_ALIGN))

PyObject *RPVITA_video_read_video() {
    SDL_Surface *surf = NULL;

    // Process video frame
    if (player_state == PLAYER_ACTIVE) {
        if (sceAvPlayerIsActive(movie_player)) {
            SceAvPlayerFrameInfo frame;
            if (sceAvPlayerGetVideoData(movie_player, &frame)) {
                int width = frame.details.video.width;
                int height = frame.details.video.height;

                // Convert pixel format
                convert_nv12_to_rgba(frame.pData, video_buffer, width, height);

                // Create SDL surface
                surf = SDL_CreateRGBSurfaceFrom(video_buffer, width, height, 32, width * 4,
                        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
            }
        } else {
            player_state = PLAYER_STOP;
        }
    }

    // If video stop, close av player
    if (player_state == PLAYER_STOP) {
        sceAvPlayerStop(movie_player);
        sceAvPlayerClose(movie_player);
        // TODO stop audio
        player_state = PLAYER_INACTIVE;
    }

    // Make return
    if (surf) {
        return PySurface_New(surf);
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

void RPVITA_video_start(const char *file) {
    SceAvPlayerInitData playerInit;
    memset(&playerInit, 0, sizeof(SceAvPlayerInitData));

    playerInit.memoryReplacement.allocate = mem_alloc;
    playerInit.memoryReplacement.deallocate = mem_free;
    playerInit.memoryReplacement.allocateTexture = gpu_alloc;
    playerInit.memoryReplacement.deallocateTexture = gpu_free;

    playerInit.basePriority = 0xA0;
    playerInit.numOutputVideoFrameBuffers = 2;
    playerInit.autoStart = 1;

    movie_player = sceAvPlayerInit(&playerInit);

    sceAvPlayerAddSource(movie_player, file);

    player_state = PLAYER_ACTIVE;
}

void RPVITA_video_stop() {
    player_state = PLAYER_STOP;
}

int RPVITA_video_get_playing() {
    return player_state == PLAYER_ACTIVE && sceAvPlayerIsActive(movie_player);
}

void vita_audio_callback(void *p, Uint8 *stream, int length) {
    SceAvPlayerFrameInfo frame;
    memset(&frame, 0, sizeof(SceAvPlayerFrameInfo));

    memset(stream, 0, length);

    if (player_state != PLAYER_ACTIVE || !sceAvPlayerIsActive(movie_player)) {
        return;
    }

    if (sceAvPlayerGetAudioData(movie_player, &frame)) {
        memcpy(stream, frame.pData, frame.details.audio.size);
    }
}

/* Utils function */

struct SwsCache {
    struct SwsContext *sws_ctx;
    int width;
    int height;
} sws_cache = { NULL, 0, 0 };

static void convert_nv12_to_rgba(uint8_t *nv12_data, uint8_t *rgba_data, int width, int height) {
    struct SwsContext *sws_ctx = NULL;

    // Get sws context from cache
    if (sws_cache.sws_ctx && sws_cache.width == width && sws_cache.height == height) {
        sws_ctx = sws_cache.sws_ctx;
    } else {
        // Create new sws context and cache it
        if (sws_cache.sws_ctx) {
            sws_freeContext(sws_cache.sws_ctx);
        }
        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12, width, height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            // TODO error handling
            return;
        }
        sws_cache.sws_ctx = sws_ctx;
        sws_cache.width = width;
        sws_cache.height = height;
    }

    uint8_t *src_slices[3] = { nv12_data,
                               nv12_data + width * height };
    int src_strides[3] = { width, width };

    uint8_t *dst_slices[1] = { rgba_data };
    int dst_strides[1] = { width * 4 };

    sws_scale(sws_ctx, src_slices, src_strides, 0, height, dst_slices, dst_strides);
}

static void *mem_alloc(void *p, uint32_t alignment, uint32_t size) {
	return memalign(alignment, size);
}

static void mem_free(void *p, void* pMemory) {
	free(pMemory);
}

static void *gpu_alloc(void *p, uint32_t alignment, uint32_t size) {
    void *res = NULL;

    if (alignment < FRAMEBUFFER_ALIGNMENT)
        alignment = FRAMEBUFFER_ALIGNMENT;

    size = ALIGN(size, alignment);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(SceKernelAllocMemBlockOpt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    SceUID memblock = sceKernelAllocMemBlock("Video Memblock", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    sceKernelGetMemBlockBase(memblock, &res);
    sceGxmMapMemory(res, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
    return res;
}

static void gpu_free(void *p, void *ptr) {
    SceUID memblock = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(memblock);
}
