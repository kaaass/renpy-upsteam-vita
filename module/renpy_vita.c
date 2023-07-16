#include "renpy_vita.h"

#include "psp2/kernel/processmgr.h"
#include "psp2/sysmodule.h"
#include "psp2/avplayer.h"
#include "psp2/kernel/sysmem.h"
#include "psp2/gxm.h"
#include "psp2/audioout.h"
#include <pygame_sdl2/pygame_sdl2.h>
#include <libswscale/swscale.h>

#define FRAMEBUFFER_ALIGNMENT 0x40000

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))
#endif

#define SCE_AVPLAYER_STATE_READY 2

int RPVITA_exit_process(int res) {
    sceKernelExitProcess(res);
}

/* Video Player */

static void convert_nv12_to_rgba(uint8_t *, uint8_t *, int, int);
static void *mem_alloc(void *p, uint32_t alignment, uint32_t size);
static void mem_free(void *p, void* pMemory);
static void *gpu_alloc(void* p, uint32_t alignment, uint32_t size);
static void gpu_free(void* p, void* pMemory);
static void event_callback(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData);

enum {
  PLAYER_INACTIVE,
  PLAYER_ACTIVE,
  PLAYER_STOP,
};

SceAvPlayerHandle movie_player;
int player_state = PLAYER_INACTIVE;
static SDL_mutex *frame_mutex;
static int audio_port = -1;

void movie_audio_init() {
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (audio_port < 0) {
        printf("Error opening audio port: %d\n", audio_port);
        // TODO: error handling
        return;
    }
    // TODO: close
}

void RPVITA_video_init() {
    import_pygame_sdl2();
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    // hold it until game end
    frame_mutex = SDL_CreateMutex();
    movie_audio_init();
}

#define MAX_WIDTH 960
#define MAX_HEIGHT 544
#define NEON_ALIGN 0x80
static uint8_t _video_buffer[MAX_WIDTH * MAX_HEIGHT * 4 + NEON_ALIGN];
#define video_buffer ((uint8_t *) ALIGN((uintptr_t) _video_buffer, NEON_ALIGN))
static SceAvPlayerFrameInfo video_frame_info;
static int received_frames = 0;
static uint64_t lastFrameTs = 0;
static uint8_t frame_buffer[MAX_WIDTH * MAX_HEIGHT * 4];

void RPVITA_periodic() {
}

int fetch_frame_thread() {
    // Fetch video frame
    while (player_state == PLAYER_ACTIVE) {
        if (sceAvPlayerIsActive(movie_player)) {
            if (sceAvPlayerGetVideoData(movie_player, &video_frame_info)) {
                int width = video_frame_info.details.video.width;
                int height = video_frame_info.details.video.height;

                if (lastFrameTs == 0 || video_frame_info.timeStamp != lastFrameTs) {
                    // Receive new frame

                    // Convert pixel format
                    convert_nv12_to_rgba(video_frame_info.pData, video_buffer, width, height);

                    SDL_LockMutex(frame_mutex);
                    memcpy(frame_buffer, video_buffer, width * height * 4);
                    received_frames += 1;
                    SDL_UnlockMutex(frame_mutex);
                }
            }
        } else {
            player_state = PLAYER_STOP;
        }
    }

    // If video stop, close av player
    if (player_state == PLAYER_STOP) {
        sceAvPlayerStop(movie_player);
        sceAvPlayerClose(movie_player);
        player_state = PLAYER_INACTIVE;
        received_frames = 0;
    }
    printf("Video thread exit\n");
    return 0;
}

PyObject *RPVITA_video_read_video() {
    SDL_Surface *surf = NULL;

    SDL_LockMutex(frame_mutex);
    if (received_frames > 0) {
        if (received_frames > 1) {
            printf("WARN: Dropped %d frames\n", received_frames - 1);
        }

        int width = video_frame_info.details.video.width;
        int height = video_frame_info.details.video.height;

        // Create SDL surface
        surf = SDL_CreateRGBSurfaceFrom(frame_buffer, width, height, 32, width * 4,
                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);

        received_frames = 0;
    }
    SDL_UnlockMutex(frame_mutex);

    // Make return
    if (surf) {
        return PySurface_New(surf);
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}


int movie_audio_thread() {
    SceAvPlayerFrameInfo frame;
    memset(&frame, 0, sizeof(SceAvPlayerFrameInfo));

    uint8_t *noSound = (uint8_t *) memalign(0x20, 4096 * 4);
    memset(noSound, 0, 4096 * 4);

    while (player_state == PLAYER_ACTIVE && sceAvPlayerIsActive(movie_player)) {
        if (sceAvPlayerGetAudioData(movie_player, &frame)) {
            sceAudioOutSetConfig(audio_port, 1024, frame.details.audio.sampleRate,
                    frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            sceAudioOutOutput(audio_port, frame.pData);
        } else {
            sceAudioOutOutput(audio_port, noSound);
        }
    }

    printf("Audio thread exit\n");
    return 0;
}

void RPVITA_video_start(const char *file) {
    SceAvPlayerInitData playerInit;
    memset(&playerInit, 0, sizeof(SceAvPlayerInitData));

    playerInit.memoryReplacement.allocate = mem_alloc;
    playerInit.memoryReplacement.deallocate = mem_free;
    playerInit.memoryReplacement.allocateTexture = gpu_alloc;
    playerInit.memoryReplacement.deallocateTexture = gpu_free;
    playerInit.eventReplacement.eventCallback = event_callback;

    playerInit.basePriority = 0xA0;
    playerInit.numOutputVideoFrameBuffers = 2;
    playerInit.autoStart = 0;

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

static void event_callback(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData) {
    printf("Player Event: %d\n", argEventId);

    if (argEventId == SCE_AVPLAYER_STATE_READY) {
        SceAvPlayerStreamInfo info;
        memset(&info, 0, sizeof(SceAvPlayerStreamInfo));

        int n = sceAvPlayerStreamCount(movie_player);
        printf("Stream count: %d\n", n);

        for (int i = 0; i < n; i++) {
            sceAvPlayerGetStreamInfo(movie_player, i, &info);

            if (info.type == SCE_AVPLAYER_AUDIO) {
                printf("Video: audio stream: freq=%d, channels=%d\n",
                        info.details.audio.sampleRate, info.details.audio.channelCount);

                // Enable
                printf("Enable audio stream\n");
                sceAvPlayerEnableStream(movie_player, i);
            } else if (info.type == SCE_AVPLAYER_VIDEO) {
                printf("Video: video stream: width=%d, height=%d, lang=%d\n",
                        info.details.video.width, info.details.video.height, info.details.video.languageCode);

                // Enable
                printf("Enable video stream\n");
                sceAvPlayerEnableStream(movie_player, i);
            }
        }

        printf("Video ts: %ld\n", sceAvPlayerCurrentTime(movie_player));
        printf("Start video\n");
        sceAvPlayerStart(movie_player);
        SDL_CreateThread(fetch_frame_thread, "Video fetch frame", NULL);
        SDL_CreateThread(movie_audio_thread, "Movie audio thread", NULL);
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
