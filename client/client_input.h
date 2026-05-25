// SDL input polling for doom-client.
//
// Drains SDL events, translates to the wire-format input message and sends
// each one immediately to the engine via MSG_INPUT_EVENT.

#ifndef DOOMCLIENT_INPUT_H
#define DOOMCLIENT_INPUT_H

// Returns 0 on normal completion, -1 if a quit was requested (SDL_QUIT or
// fatal send error). Sends MSG_BYE on quit before returning.
int client_input_poll(int fd);

#endif
