#!/usr/bin/env bash
# Installs the udev rule that stops the keyboard's System Control interface from
# being advertised to games as a gamepad. Needs root, so it is run by hand.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install -m 0644 "$here/packaging/udev/99-hide-phantom-joystick.rules" /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=input --action=change
echo "installed; devices now reporting ID_INPUT_JOYSTICK:"
for e in /dev/input/event*; do
    v=$(udevadm info --query=property --name="$e" 2>/dev/null | grep '^ID_INPUT_JOYSTICK=' || true)
    [ "$v" = "ID_INPUT_JOYSTICK=1" ] && printf '  %s  %s\n' "$e" "$(cat /sys/class/input/$(basename $e)/device/name 2>/dev/null)"
done
