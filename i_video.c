// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// DESCRIPTION:
//      DOOM graphics stuff, SDL2 port.
//
//      Replaces the original X11/MITSHM implementation with an SDL2
//      window + streaming RGBA texture. The Doom engine still writes
//      to an 8-bit indexed framebuffer (screens[0]); this module
//      converts that to 32-bit ARGB through a 256-entry palette
//      every frame before pushing to the GPU.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <SDL2/SDL.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"


static SDL_Window*   sdl_window   = NULL;
static SDL_Renderer* sdl_renderer = NULL;
static SDL_Texture*  sdl_texture  = NULL;

// 256-entry ARGB8888 palette built from Doom's gamma-corrected RGB triples.
static Uint32 palette[256];

// True once I_InitGraphics has run.
static int initialized = 0;

// Window scale factor. Doom's internal resolution is 320x200; we present
// a larger window for usability on modern displays. Use logical size so
// the texture maps cleanly regardless of window size.
static int window_multiply = 3;

// Mouse grab. Mirrors original X11 -grabmouse flag behavior.
static boolean grabMouse = false;


//
// Map an SDL keysym to the Doom KEY_* code expected by the engine.
//
static int xlatekey(SDL_Keycode sym)
{
    switch (sym)
    {
      case SDLK_LEFT:       return KEY_LEFTARROW;
      case SDLK_RIGHT:      return KEY_RIGHTARROW;
      case SDLK_DOWN:       return KEY_DOWNARROW;
      case SDLK_UP:         return KEY_UPARROW;
      case SDLK_ESCAPE:     return KEY_ESCAPE;
      case SDLK_RETURN:     return KEY_ENTER;
      case SDLK_TAB:        return KEY_TAB;
      case SDLK_F1:         return KEY_F1;
      case SDLK_F2:         return KEY_F2;
      case SDLK_F3:         return KEY_F3;
      case SDLK_F4:         return KEY_F4;
      case SDLK_F5:         return KEY_F5;
      case SDLK_F6:         return KEY_F6;
      case SDLK_F7:         return KEY_F7;
      case SDLK_F8:         return KEY_F8;
      case SDLK_F9:         return KEY_F9;
      case SDLK_F10:        return KEY_F10;
      case SDLK_F11:        return KEY_F11;
      case SDLK_F12:        return KEY_F12;

      case SDLK_BACKSPACE:
      case SDLK_DELETE:     return KEY_BACKSPACE;

      case SDLK_PAUSE:      return KEY_PAUSE;

      case SDLK_KP_EQUALS:
      case SDLK_EQUALS:     return KEY_EQUALS;

      case SDLK_KP_MINUS:
      case SDLK_MINUS:      return KEY_MINUS;

      case SDLK_LSHIFT:
      case SDLK_RSHIFT:     return KEY_RSHIFT;

      case SDLK_LCTRL:
      case SDLK_RCTRL:      return KEY_RCTRL;

      case SDLK_LALT:
      case SDLK_RALT:       return KEY_RALT;

      default:
        if (sym >= SDLK_SPACE && sym <= SDLK_z)
        {
            // SDL gives lowercase letters already; engine expects lowercase too.
            return (int) sym;
        }
        return 0;
    }
}


void I_ShutdownGraphics(void)
{
    if (sdl_texture)  { SDL_DestroyTexture(sdl_texture);   sdl_texture = NULL; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = NULL; }
    if (sdl_window)   { SDL_DestroyWindow(sdl_window);     sdl_window = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}


//
// I_StartFrame
//
void I_StartFrame (void)
{
    // Original was empty too.
}


static int lastmousex = 0;
static int lastmousey = 0;


//
// Build the Doom mouse button bitfield from current SDL button state and
// the button involved in this event (if any). Mirrors the original
// 1|2|4 (left|middle|right) packing.
//
static int translate_buttons(Uint32 state, Uint8 button, int pressed)
{
    int b = 0;
    if (state & SDL_BUTTON(SDL_BUTTON_LEFT))   b |= 1;
    if (state & SDL_BUTTON(SDL_BUTTON_MIDDLE)) b |= 2;
    if (state & SDL_BUTTON(SDL_BUTTON_RIGHT))  b |= 4;
    if (button == SDL_BUTTON_LEFT)   { if (pressed) b |= 1; else b &= ~1; }
    if (button == SDL_BUTTON_MIDDLE) { if (pressed) b |= 2; else b &= ~2; }
    if (button == SDL_BUTTON_RIGHT)  { if (pressed) b |= 4; else b &= ~4; }
    return b;
}


//
// Drain pending SDL events and translate to Doom events.
//
static void I_GetEvents(void)
{
    SDL_Event ev;
    event_t   event;

    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
          case SDL_KEYDOWN:
            event.type  = ev_keydown;
            event.data1 = xlatekey(ev.key.keysym.sym);
            if (event.data1) D_PostEvent(&event);
            break;

          case SDL_KEYUP:
            event.type  = ev_keyup;
            event.data1 = xlatekey(ev.key.keysym.sym);
            if (event.data1) D_PostEvent(&event);
            break;

          case SDL_MOUSEBUTTONDOWN:
            event.type  = ev_mouse;
            event.data1 = translate_buttons(SDL_GetMouseState(NULL, NULL),
                                            ev.button.button, 1);
            event.data2 = event.data3 = 0;
            D_PostEvent(&event);
            break;

          case SDL_MOUSEBUTTONUP:
            event.type  = ev_mouse;
            event.data1 = translate_buttons(SDL_GetMouseState(NULL, NULL),
                                            ev.button.button, 0);
            event.data2 = event.data3 = 0;
            D_PostEvent(&event);
            break;

          case SDL_MOUSEMOTION:
            event.type  = ev_mouse;
            event.data1 = translate_buttons(ev.motion.state, 0, 0);
            // Original X11 path scaled motion by 4; preserve that.
            event.data2 = (ev.motion.x - lastmousex) << 2;
            event.data3 = (lastmousey - ev.motion.y) << 2;
            lastmousex = ev.motion.x;
            lastmousey = ev.motion.y;
            if (event.data2 || event.data3)
                D_PostEvent(&event);
            break;

          case SDL_QUIT:
            I_Quit();
            break;

          default:
            break;
        }
    }
}


//
// I_StartTic
//
void I_StartTic (void)
{
    if (!initialized)
        return;
    I_GetEvents();
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // Original was empty.
}


//
// I_FinishUpdate
//
// Copy the 8-bit indexed Doom framebuffer (screens[0]) through the current
// palette into the streaming texture, then present it.
//
void I_FinishUpdate (void)
{
    static int lasttic;
    int        tics;
    int        i;

    // -devparm draws timing dots along the bottom of the screen.
    if (devparm)
    {
        i = I_GetTime();
        tics = i - lasttic;
        lasttic = i;
        if (tics > 20) tics = 20;

        for (i = 0; i < tics * 2; i += 2)
            screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0xff;
        for ( ; i < 20 * 2; i += 2)
            screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0x0;
    }

    void*  pixels;
    int    pitch;

    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) != 0)
        I_Error("SDL_LockTexture: %s", SDL_GetError());

    {
        const byte* src = screens[0];
        int y;
        for (y = 0; y < SCREENHEIGHT; y++)
        {
            Uint32* dst = (Uint32*)((Uint8*)pixels + y * pitch);
            int x;
            for (x = 0; x < SCREENWIDTH; x++)
                dst[x] = palette[src[x]];
            src += SCREENWIDTH;
        }
    }

    SDL_UnlockTexture(sdl_texture);

    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}


//
// I_SetPalette
//
// palette[] is 256 RGB triples (3 bytes each), gamma-corrected via
// gammatable like the original.
//
void I_SetPalette (byte* palette_in)
{
    int i, c;
    for (i = 0; i < 256; i++)
    {
        c = gammatable[usegamma][*palette_in++]; Uint8 r = (Uint8)c;
        c = gammatable[usegamma][*palette_in++]; Uint8 g = (Uint8)c;
        c = gammatable[usegamma][*palette_in++]; Uint8 b = (Uint8)c;
        palette[i] = (0xFFu << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
    }
}


void I_InitGraphics(void)
{
    if (initialized)
        return;

    signal(SIGINT, (void (*)(int)) I_Quit);

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        I_Error("SDL_Init(VIDEO) failed: %s", SDL_GetError());

    // Honor original -2/-3/-4 multiplier flags for window size.
    if (M_CheckParm("-2")) window_multiply = 2;
    if (M_CheckParm("-3")) window_multiply = 3;
    if (M_CheckParm("-4")) window_multiply = 4;

    grabMouse = !!M_CheckParm("-grabmouse");

    sdl_window = SDL_CreateWindow(
        "DOOM",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREENWIDTH  * window_multiply,
        SCREENHEIGHT * window_multiply,
        SDL_WINDOW_SHOWN);
    if (!sdl_window)
        I_Error("SDL_CreateWindow failed: %s", SDL_GetError());

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer)
    {
        // Fall back to software renderer on systems without GL.
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_renderer)
            I_Error("SDL_CreateRenderer failed: %s", SDL_GetError());
    }

    SDL_RenderSetLogicalSize(sdl_renderer, SCREENWIDTH, SCREENHEIGHT);

    sdl_texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREENWIDTH, SCREENHEIGHT);
    if (!sdl_texture)
        I_Error("SDL_CreateTexture failed: %s", SDL_GetError());

    if (grabMouse)
    {
        SDL_SetRelativeMouseMode(SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    }
    else
    {
        SDL_ShowCursor(SDL_DISABLE);
    }

    // V_Init() (called earlier in D_DoomMain) already allocated screens[0..3]
    // out of one shared buffer, so we don't touch it here.

    initialized = 1;
}
