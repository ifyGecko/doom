project('doom', 'c',
  version : '1.10',
  default_options : [
    'c_std=gnu89',
    'warning_level=1',
    'optimization=2',
    'debug=true',
  ],
)

cc = meson.get_compiler('c')

# -fno-strict-aliasing: the renderer plays heavy aliasing games with the
#  framebuffer (cast through unsigned/double/etc.) that the C standard does
#  not strictly allow. Keeping the original semantics requires disabling
#  alias-based optimization.
# -fcommon: original sources declare some globals in headers without extern;
#  modern gcc rejects that without this flag.
# The -Wno-error=... entries demote legacy K&R-era constructs (implicit int,
#  implicit function declarations, signed/pointer type mismatches) from
#  hard errors back to warnings, since the original Doom sources rely on
#  them.
add_project_arguments(
  '-Wno-unused',
  '-Wno-format-security',
  '-fno-strict-aliasing',
  '-fcommon',
  '-Wno-error=implicit-int',
  '-Wno-error=implicit-function-declaration',
  '-Wno-error=int-conversion',
  '-Wno-error=incompatible-pointer-types',
  '-DNORMALUNIX',
  '-DLINUX',
  language : 'c',
)

sdl2_dep    = dependency('sdl2')
ncurses_dep = dependency('ncurses')
m_dep       = cc.find_library('m', required : false)

# ---- include directories ---------------------------------------------------
#
# src/common  : wire protocol + framing, shared by both binaries.
# src/engine  : headless game engine (original DOOM sources + net transport).
# src/client  : SDL2 front-end (window, input capture, frame display).
# src/config  : standalone ncurses configurator for ~/.doomrc.
#
# The client also reads src/engine headers for KEY_* constants in doomdef.h,
# which are part of the wire-level input vocabulary. The config tool keeps
# its own copy of the KEY_* numeric constants instead, so it does not pull
# in the engine headers (whose symbols collide with ncurses' KEY_* macros).
#
common_inc = include_directories('src/common')
engine_inc = include_directories('src/engine')
client_inc = include_directories('src/client')
config_inc = include_directories('src/config')

# ---- common ----------------------------------------------------------------
#
# Length-prefixed framing layer + wire format definitions.
#
common_sources = files(
  'src/common/framing.c',
)

# ---- doom-engine -----------------------------------------------------------
#
# Headless game engine. Follows the chocolate-doom philosophy of preserving
# the original 1990s simulation faithfully: physics, AI, rendering math,
# collision, level logic, and the original DOOM bugs are NOT to be altered.
# What may be modified is the surrounding infrastructure - the SDL video
# layer was replaced wholesale by a net transport (i_video_net.c in place
# of i_video.c), the audio playback layer was stripped to ease porting,
# and observability such as the init logger (i_initlog.c) instruments any
# source file it needs to, as long as the change does not affect gameplay.
#
engine_sources = files(
  # core / globals
  'src/engine/doomdef.c',
  'src/engine/doomstat.c',
  'src/engine/dstrings.c',
  'src/engine/info.c',
  'src/engine/tables.c',

  # system interface (entry + net transport replacing SDL)
  'src/engine/i_main.c',
  'src/engine/i_supervisor.c',
  'src/engine/i_system.c',
  'src/engine/i_video_net.c',
  'src/engine/i_initlog.c',

  # top-level doom driver
  'src/engine/d_main.c',
  'src/engine/d_items.c',

  # game / menu / finale
  'src/engine/g_game.c',
  'src/engine/m_menu.c',
  'src/engine/f_finale.c',
  'src/engine/f_wipe.c',

  # misc / math utilities
  'src/engine/m_argv.c',
  'src/engine/m_bbox.c',
  'src/engine/m_cheat.c',
  'src/engine/m_fixed.c',
  'src/engine/m_misc.c',
  'src/engine/m_random.c',
  'src/engine/m_swap.c',

  # play simulation
  'src/engine/p_ceilng.c',
  'src/engine/p_doors.c',
  'src/engine/p_enemy.c',
  'src/engine/p_floor.c',
  'src/engine/p_inter.c',
  'src/engine/p_lights.c',
  'src/engine/p_map.c',
  'src/engine/p_maputl.c',
  'src/engine/p_mobj.c',
  'src/engine/p_plats.c',
  'src/engine/p_pspr.c',
  'src/engine/p_setup.c',
  'src/engine/p_sight.c',
  'src/engine/p_spec.c',
  'src/engine/p_switch.c',
  'src/engine/p_telept.c',
  'src/engine/p_tick.c',
  'src/engine/p_user.c',

  # renderer
  'src/engine/r_bsp.c',
  'src/engine/r_data.c',
  'src/engine/r_draw.c',
  'src/engine/r_main.c',
  'src/engine/r_plane.c',
  'src/engine/r_segs.c',
  'src/engine/r_sky.c',
  'src/engine/r_things.c',

  # automap / HUD / status bar / intermission
  'src/engine/am_map.c',
  'src/engine/hu_lib.c',
  'src/engine/hu_stuff.c',
  'src/engine/st_lib.c',
  'src/engine/st_stuff.c',
  'src/engine/wi_stuff.c',

  # video / wad / memory
  'src/engine/v_video.c',
  'src/engine/w_wad.c',
  'src/engine/z_zone.c',
)

executable('doom-engine',
  engine_sources + common_sources,
  include_directories : [engine_inc, common_inc],
  dependencies        : [m_dep],
  install             : false,
)

# ---- doom-client -----------------------------------------------------------
#
# Thin SDL2 front-end. Opens a window, captures input, displays received
# frames. No DOOM game logic.
#
client_sources = files(
  'src/client/client_main.c',
  'src/client/client_video.c',
  'src/client/client_input.c',
)

executable('doom-client',
  client_sources + common_sources,
  include_directories : [client_inc, common_inc, engine_inc],
  dependencies        : [sdl2_dep],
  install             : false,
)

# ---- doom-config -----------------------------------------------------------
#
# ncurses TUI for editing a doomrc. Standalone; shares no compiled code with
# the engine or client. The on-disk format it produces matches the parser
# in src/engine/m_misc.c (M_LoadDefaults / M_SaveDefaults).
#
config_sources = files(
  'src/config/config_main.c',
  'src/config/config_data.c',
  'src/config/config_keys.c',
  'src/config/config_ui.c',
)

executable('doom-config',
  config_sources,
  include_directories : [config_inc],
  dependencies        : [ncurses_dep],
  install             : false,
)
