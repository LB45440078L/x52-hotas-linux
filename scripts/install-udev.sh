#!/usr/bin/env bash
#
# Installs the udev rule that grants the logged-in user access to the X52 Pro
# USB node, its hidraw endpoint and the evdev node.
#
# Must be run as root:
#     sudo ./scripts/install-udev.sh
#
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "error: this script must run as root (sudo $0)" >&2
    exit 1
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rule_src="${here}/../data/udev/70-x52pro.rules"
rule_dst="/etc/udev/rules.d/70-x52pro.rules"

if [[ ! -f "${rule_src}" ]]; then
    echo "error: cannot find ${rule_src}" >&2
    exit 1
fi

echo "==> installing ${rule_dst}"
install -Dm644 "${rule_src}" "${rule_dst}"

echo "==> reloading udev rules"
udevadm control --reload-rules

echo "==> re-triggering the X52 Pro"
udevadm trigger --subsystem-match=usb --attr-match=idVendor=06a3 --action=change || true
udevadm trigger --subsystem-match=usb --attr-match=idVendor=06a3 --action=add || true
udevadm trigger --subsystem-match=hidraw --action=change || true
udevadm trigger --subsystem-match=input --action=change || true

# Older rule sets do not use uaccess; fall back to a group that exists.
if ! getent group plugdev >/dev/null 2>&1; then
    echo "==> note: no plugdev group on this system, relying on uaccess ACLs"
fi

echo
echo "Done. If the device node is still not writable, unplug and replug the"
echo "X52 Pro (or reboot) so the new rule is applied."
