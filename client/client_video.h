// SDL2 rendering surface used by doom-client.
//
// Mirrors what the original i_video.c did: keep an 8-bit indexed frame, map
// through a 256-entry ARGB palette, blit to a streaming SDL texture.

#ifndef DOOMCLIENT_VIDEO_H
#define DOOMCLIENT_VIDEO_H

#include <stdint.h>

int  client_video_init(int width, int height, int multiply, int grab_mouse);
void client_video_shutdown(void);

// palette_bgra points to 256 * 4 bytes laid out B,G,R,A (matches wire format).
void client_video_set_palette(const uint8_t *palette_bgra);

// pixels points to width * height bytes of indexed framebuffer.
void client_video_present(const uint8_t *pixels);

#endif
