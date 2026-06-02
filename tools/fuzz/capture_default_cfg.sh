#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# Produce a default.cfg seed for the fuzz corpus.
#
# Output: tools/fuzz/defaults.cfg.bin
#
# Source of truth, in order of preference:
#   1. $HOME/.doomrc          (whatever the engine has written before)
#   2. A hand-rolled sample with the canonical keys from m_misc.c. This
#      guarantees AFL gets to see M_LoadDefaults' tokenizer hitting both
#      int defaults (mouse_sensitivity, key_*, etc.) and string defaults
#      (mousedev, mousetype, chatmacroN), which are different code paths
#      in m_misc.c.
# -----------------------------------------------------------------------------
set -eu

OUT="$(cd "$(dirname "$0")" && pwd)/defaults.cfg.bin"

if [[ -f "$HOME/.doomrc" ]]; then
  cp "$HOME/.doomrc" "$OUT"
  echo "captured $HOME/.doomrc -> $OUT ($(wc -c < "$OUT") bytes)"
  exit 0
fi

cat > "$OUT" <<'EOF'
mouse_sensitivity		5
show_messages		1
key_right		174
key_left		172
key_up		173
key_down		175
key_strafeleft		44
key_straferight		46
key_fire		157
key_use		32
key_strafe		184
key_speed		182
mousedev		"/dev/ttyS0"
mousetype		"microsoft"
use_mouse		1
mouseb_fire		0
mouseb_strafe		1
mouseb_forward		2
use_joystick		0
joyb_fire		0
joyb_strafe		1
joyb_use		3
joyb_speed		2
screenblocks		9
detaillevel		0
snd_channels		3
sfx_volume		8
music_volume		8
chatmacro0		"No"
chatmacro1		"I'm ready to kick butt!"
chatmacro2		"I'm OK."
chatmacro3		"No way!"
chatmacro4		"Help!"
chatmacro5		"You suck!"
chatmacro6		"Next time, scumbag..."
chatmacro7		"Come here!"
chatmacro8		"I'll take care of it."
chatmacro9		"Yes"
usegamma		0
EOF

echo "wrote sample default.cfg -> $OUT ($(wc -c < "$OUT") bytes)"
