#!/usr/bin/env bash
#
# Builds and installs the X52 Pro driver for the current user (no root needed
# except for the udev rule).
#
#   ./scripts/install.sh            install into ~/.local
#   PREFIX=/usr/local ./scripts/install.sh
#   ./scripts/install.sh --no-udev  skip the permission setup
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
prefix="${PREFIX:-${HOME}/.local}"
build_dir="${root}/build"
with_udev=1

for arg in "$@"; do
    case "${arg}" in
        --no-udev) with_udev=0 ;;
        --help|-h)
            sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: ${arg}" >&2; exit 2 ;;
    esac
done

echo "==> building"
cmake -S "${root}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${build_dir}"

echo "==> installing binaries to ${prefix}/bin"
install -d "${prefix}/bin"
install -m755 "${build_dir}/x52d" "${prefix}/bin/x52d"
install -m755 "${build_dir}/x52ctl" "${prefix}/bin/x52ctl"
install -m755 "${build_dir}/x52probe" "${prefix}/bin/x52probe"
if [[ -x "${build_dir}/x52panel" ]]; then
    install -m755 "${build_dir}/x52panel" "${prefix}/bin/x52panel"
fi

echo "==> installing data"
install -d "${prefix}/share/applications" "${prefix}/share/icons/hicolor/scalable/apps"
install -m644 "${root}/data/applications/org.x52pro.Panel.desktop" \
    "${prefix}/share/applications/org.x52pro.Panel.desktop"
install -m644 "${root}/data/icons/org.x52pro.Panel.svg" \
    "${prefix}/share/icons/hicolor/scalable/apps/org.x52pro.Panel.svg"
sed -i "s|^Exec=x52panel|Exec=${prefix}/bin/x52panel|" \
    "${prefix}/share/applications/org.x52pro.Panel.desktop"

# Update the desktop database when the tool is available.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${prefix}/share/applications" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q "${prefix}/share/icons/hicolor" 2>/dev/null || true
fi

echo "==> installing default profiles"
profiles_dir="${XDG_CONFIG_HOME:-${HOME}/.config}/x52pro/profiles"
install -d "${profiles_dir}"
for p in "${root}"/data/profiles/*.json; do
    target="${profiles_dir}/$(basename "${p}")"
    if [[ -e "${target}" ]]; then
        echo "    keeping existing $(basename "${p}")"
    else
        install -m644 "${p}" "${target}"
        echo "    installed $(basename "${p}")"
    fi
done

echo "==> installing systemd user unit"
unit_dir="${XDG_CONFIG_HOME:-${HOME}/.config}/systemd/user"
install -d "${unit_dir}"
sed "s|ExecStart=%h/.local/bin/x52d|ExecStart=${prefix}/bin/x52d|" \
    "${root}/data/systemd/x52d.service" > "${unit_dir}/x52d.service"
if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload || true
    systemctl --user enable --now x52d.service 2>/dev/null || \
        echo "    (enable manually: systemctl --user enable --now x52d)"
fi

if [[ ${with_udev} -eq 1 ]]; then
    echo "==> checking device permissions"
    if "${prefix}/bin/x52probe" info >/dev/null 2>&1; then
        echo "    already allowed to access the device"
    else
        echo
        echo "    The X52 Pro needs a udev rule so your user can talk to it."
        echo "    Run:  sudo ${here}/install-udev.sh"
        echo "    then unplug and replug the controller."
        echo
    fi
fi

echo
echo "Installed:"
echo "  x52d      driver daemon (systemd --user)"
echo "  x52ctl    command line control"
echo "  x52panel  GTK4 control panel"
echo "  x52probe  hardware diagnostics"
