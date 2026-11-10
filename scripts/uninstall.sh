#!/usr/bin/env bash
# Remove what scripts/install.sh installed. Leaves ~/.config/x52pro alone.
set -euo pipefail
prefix="${PREFIX:-$HOME/.local}"
rm -f "$prefix/bin/x52d" "$prefix/bin/x52ctl" "$prefix/bin/x52panel" \
      "$prefix/bin/x52probe"
rm -f "$prefix/share/applications/x52panel.desktop"
rm -f "$prefix/share/icons/hicolor/scalable/apps/x52panel.svg"
systemctl --user disable --now x52d.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/x52d.service"
systemctl --user daemon-reload 2>/dev/null || true
echo "removed; settings and profiles in ~/.config/x52pro were kept"
