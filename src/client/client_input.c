// SDL -> DOOM input translation and forwarding.
//
// Translates SDL keysym / mouse events into DOOM's KEY_* / button bitfield
// representation, then ships them to the engine as MSG_INPUT_EVENT.

#include "client_input.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "doomdef.h"             // KEY_* constants
#include "wire.h"
#include "framing.h"

static int lastmousex = 0;
static int lastmousey = 0;

static int xlatekey(SDL_Keycode sym)
{
    switch (sym) {
      case SDLK_LEFT:    return KEY_LEFTARROW;
      case SDLK_RIGHT:   return KEY_RIGHTARROW;
      case SDLK_DOWN:    return KEY_DOWNARROW;
      case SDLK_UP:      return KEY_UPARROW;
      case SDLK_ESCAPE:  return KEY_ESCAPE;
      case SDLK_RETURN:  return KEY_ENTER;
      case SDLK_TAB:     return KEY_TAB;
      case SDLK_F1:      return KEY_F1;
      case SDLK_F2:      return KEY_F2;
      case SDLK_F3:      return KEY_F3;
      case SDLK_F4:      return KEY_F4;
      case SDLK_F5:      return KEY_F5;
      case SDLK_F6:      return KEY_F6;
      case SDLK_F7:      return KEY_F7;
      case SDLK_F8:      return KEY_F8;
      case SDLK_F9:      return KEY_F9;
      case SDLK_F10:     return KEY_F10;
      case SDLK_F11:     return KEY_F11;
      case SDLK_F12:     return KEY_F12;

      case SDLK_BACKSPACE:
      case SDLK_DELETE:  return KEY_BACKSPACE;

      case SDLK_PAUSE:   return KEY_PAUSE;

      case SDLK_KP_EQUALS:
      case SDLK_EQUALS:  return KEY_EQUALS;

      case SDLK_KP_MINUS:
      case SDLK_MINUS:   return KEY_MINUS;

      case SDLK_LSHIFT:
      case SDLK_RSHIFT:  return KEY_RSHIFT;

      case SDLK_LCTRL:
      case SDLK_RCTRL:   return KEY_RCTRL;

      case SDLK_LALT:
      case SDLK_RALT:    return KEY_RALT;

      default:
        if (sym >= SDLK_SPACE && sym <= SDLK_z) return (int)sym;
        return 0;
    }
}

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

static int send_event(int fd, uint8_t ev_type, int32_t d1, int32_t d2, int32_t d3)
{
    uint8_t  buf[13];
    uint32_t u;

    buf[0] = ev_type;

    u = (uint32_t)d1;
    buf[1] = (uint8_t)u; buf[2] = (uint8_t)(u >> 8);
    buf[3] = (uint8_t)(u >> 16); buf[4] = (uint8_t)(u >> 24);

    u = (uint32_t)d2;
    buf[5] = (uint8_t)u; buf[6] = (uint8_t)(u >> 8);
    buf[7] = (uint8_t)(u >> 16); buf[8] = (uint8_t)(u >> 24);

    u = (uint32_t)d3;
    buf[9]  = (uint8_t)u;  buf[10] = (uint8_t)(u >> 8);
    buf[11] = (uint8_t)(u >> 16); buf[12] = (uint8_t)(u >> 24);

    return net_send(fd, MSG_INPUT_EVENT, buf, sizeof buf);
}

int client_input_poll(int fd)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
          case SDL_KEYDOWN: {
              int k = xlatekey(ev.key.keysym.sym);
              if (k && send_event(fd, DOOMNET_EV_KEYDOWN, k, 0, 0) < 0) return -1;
              break;
          }
          case SDL_KEYUP: {
              int k = xlatekey(ev.key.keysym.sym);
              if (k && send_event(fd, DOOMNET_EV_KEYUP, k, 0, 0) < 0) return -1;
              break;
          }
          case SDL_MOUSEBUTTONDOWN: {
              int b = translate_buttons(SDL_GetMouseState(NULL, NULL),
                                        ev.button.button, 1);
              if (send_event(fd, DOOMNET_EV_MOUSE, b, 0, 0) < 0) return -1;
              break;
          }
          case SDL_MOUSEBUTTONUP: {
              int b = translate_buttons(SDL_GetMouseState(NULL, NULL),
                                        ev.button.button, 0);
              if (send_event(fd, DOOMNET_EV_MOUSE, b, 0, 0) < 0) return -1;
              break;
          }
          case SDL_MOUSEMOTION: {
              int b   = translate_buttons(ev.motion.state, 0, 0);
              int dx  = (ev.motion.x - lastmousex) << 2;
              int dy  = (lastmousey - ev.motion.y) << 2;
              lastmousex = ev.motion.x;
              lastmousey = ev.motion.y;
              if (dx || dy) {
                  if (send_event(fd, DOOMNET_EV_MOUSE, b, dx, dy) < 0) return -1;
              }
              break;
          }
          case SDL_QUIT:
              net_send(fd, MSG_BYE, NULL, 0);
              return -1;
          default:
              break;
        }
    }
    return 0;
}
