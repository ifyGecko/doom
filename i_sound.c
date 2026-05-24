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
//      Sound interface stubs. Audio support was stripped from this port;
//      every entry point declared in i_sound.h is implemented as a
//      no-op so the rest of the engine can call them unmodified.
//
//-----------------------------------------------------------------------------

#include "i_sound.h"


void I_InitSound(void)        { }
void I_UpdateSound(void)      { }
void I_SubmitSound(void)      { }
void I_ShutdownSound(void)    { }

void I_SetChannels(void)      { }

int I_GetSfxLumpNum(sfxinfo_t* sfxinfo)
{
    (void)sfxinfo;
    return -1;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)id; (void)vol; (void)sep; (void)pitch; (void)priority;
    // Return a stable non-negative handle. The engine uses this as both
    // an identifier and a "is it playing" flag, so any consistent value
    // is fine since I_SoundIsPlaying always reports false.
    return 0;
}

void I_StopSound(int handle)
{
    (void)handle;
}

int I_SoundIsPlaying(int handle)
{
    (void)handle;
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle; (void)vol; (void)sep; (void)pitch;
}


//
//  MUSIC I/O
//
void I_InitMusic(void)              { }
void I_ShutdownMusic(void)          { }
void I_SetMusicVolume(int volume)   { (void)volume; }
void I_PauseSong(int handle)        { (void)handle; }
void I_ResumeSong(int handle)       { (void)handle; }

int I_RegisterSong(void* data)
{
    (void)data;
    return 0;
}

void I_PlaySong(int handle, int looping)
{
    (void)handle; (void)looping;
}

void I_StopSong(int handle)         { (void)handle; }
void I_UnRegisterSong(int handle)   { (void)handle; }
