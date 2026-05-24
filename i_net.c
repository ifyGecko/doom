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
//      Network interface stub. All actual networking has been removed
//      from this port; this file only provides the minimal doomcom
//      setup that the core d_net.c expects so single-player works.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include "i_system.h"
#include "d_net.h"
#include "doomstat.h"

#include "i_net.h"


//
// I_InitNetwork
//
// d_net.c reads doomcom->id, ->numplayers, ->numnodes, ->consoleplayer
// to set itself up. For single-player we just allocate the struct and
// populate those fields.
//
void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    netgame              = false;
    doomcom->id          = DOOMCOM_ID;
    doomcom->numplayers  = 1;
    doomcom->numnodes    = 1;
    doomcom->deathmatch  = false;
    doomcom->consoleplayer = 0;
    doomcom->ticdup      = 1;
    doomcom->extratics   = 0;
}


//
// I_NetCmd
//
// Called by d_net.c via NetUpdate when it wants to send or receive a
// packet. With no actual network there is nothing to do.
//
void I_NetCmd(void)
{
    // No network: ignore. NetUpdate paths that lead here in
    // single-player will simply see no packets arrive (remotenode = -1).
}
