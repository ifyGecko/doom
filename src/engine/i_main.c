// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//
//-----------------------------------------------------------------------------




#include "doomdef.h"

#include "m_argv.h"
#include "d_main.h"

// Supervisor entry, defined in engine/i_supervisor.c. Owns the listening
// socket and fork()s a fresh child per accepted client; each child runs the
// existing I_NetBootstrap + D_DoomMain in its own address space. This main()
// itself never reaches game code.
extern int I_Supervise(int argc, char **argv);

int
main
( int		argc,
  char**	argv ) 
{ 
    myargc = argc; 
    myargv = argv; 

    return I_Supervise(argc, argv);
} 
