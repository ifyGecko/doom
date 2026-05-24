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

sdl2_dep = dependency('sdl2')
m_dep    = cc.find_library('m', required : false)

doom_sources = files(
  'doomdef.c',
  'doomstat.c',
  'dstrings.c',
  'i_system.c',
  'i_sound.c',
  'i_video.c',
  'i_net.c',
  'tables.c',
  'f_finale.c',
  'f_wipe.c',
  'd_main.c',
  'd_net.c',
  'd_items.c',
  'g_game.c',
  'm_menu.c',
  'm_misc.c',
  'm_argv.c',
  'm_bbox.c',
  'm_fixed.c',
  'm_swap.c',
  'm_cheat.c',
  'm_random.c',
  'am_map.c',
  'p_ceilng.c',
  'p_doors.c',
  'p_enemy.c',
  'p_floor.c',
  'p_inter.c',
  'p_lights.c',
  'p_map.c',
  'p_maputl.c',
  'p_plats.c',
  'p_pspr.c',
  'p_setup.c',
  'p_sight.c',
  'p_spec.c',
  'p_switch.c',
  'p_mobj.c',
  'p_telept.c',
  'p_tick.c',
  'p_saveg.c',
  'p_user.c',
  'r_bsp.c',
  'r_data.c',
  'r_draw.c',
  'r_main.c',
  'r_plane.c',
  'r_segs.c',
  'r_sky.c',
  'r_things.c',
  'w_wad.c',
  'wi_stuff.c',
  'v_video.c',
  'st_lib.c',
  'st_stuff.c',
  'hu_stuff.c',
  'hu_lib.c',
  's_sound.c',
  'z_zone.c',
  'info.c',
  'sounds.c',
  'i_main.c',
)

executable('doom',
  doom_sources,
  dependencies : [sdl2_dep, m_dep],
  install : false,
)
