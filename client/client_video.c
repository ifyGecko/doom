// SDL2 rendering for doom-client.
//
// Most of this is lifted unchanged from the engine's old i_video.c. The only
// real differences:
//   - palette comes pre-gamma-corrected from the engine (in BGRA), so we
//     skip gammatable[] entirely.
//   - we don't try to drive a game loop; the caller hands us frames it
//     received from the server.

#include "client_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

static SDL_Window   *sdl_window   = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture  *sdl_texture  = NULL;

static int frame_w = 0, frame_h = 0;
static Uint32 palette[256];

int client_video_init(int width, int height, int multiply, int grab_mouse)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return -1;
    }

    if (multiply < 1) multiply = 1;

    sdl_window = SDL_CreateWindow(
        "DOOM (remote)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width * multiply, height * multiply,
        SDL_WINDOW_SHOWN);
    if (!sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer) {
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_renderer) {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return -1;
        }
    }
    SDL_RenderSetLogicalSize(sdl_renderer, width, height);

    sdl_texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!sdl_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    if (grab_mouse) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
    SDL_ShowCursor(SDL_DISABLE);

    frame_w = width;
    frame_h = height;

    // Identity palette so a stray frame before MSG_PALETTE doesn't show garbage.
    {
        int i;
        for (i = 0; i < 256; i++) palette[i] = 0xFF000000u;
    }
    return 0;
}

void client_video_shutdown(void)
{
    if (sdl_texture)  { SDL_DestroyTexture(sdl_texture);   sdl_texture = NULL; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = NULL; }
    if (sdl_window)   { SDL_DestroyWindow(sdl_window);     sdl_window = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void client_video_set_palette(const uint8_t *p)
{
    int i;
    for (i = 0; i < 256; i++) {
        Uint8 b = p[i*4 + 0];
        Uint8 g = p[i*4 + 1];
        Uint8 r = p[i*4 + 2];
        // Alpha byte is ignored (we always present opaque).
        palette[i] = (0xFFu << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
    }
}

void client_video_present(const uint8_t *src)
{
    void *pixels;
    int   pitch;
    int   y, x;

    if (!sdl_texture) return;

    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) != 0) {
        fprintf(stderr, "SDL_LockTexture: %s\n", SDL_GetError());
        return;
    }
    for (y = 0; y < frame_h; y++) {
        Uint32 *dst = (Uint32 *)((Uint8 *)pixels + y * pitch);
        for (x = 0; x < frame_w; x++)
            dst[x] = palette[src[x]];
        src += frame_w;
    }
    SDL_UnlockTexture(sdl_texture);

    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}
