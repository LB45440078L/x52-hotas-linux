#!/usr/bin/env python3
"""Shift a War Thunder .blk from a multi-device layout to a single X52.

When the virtual pad was present the X52 sat at axesOffset 11 / buttonsOffset 28.
With it gone the X52 is the only joystick, so it starts at 0/0: every bind has to
move by the same delta. Binds that pointed at the removed device are dropped
rather than silently re-pointed at a wrong axis.
"""
import re, sys

SHIFT_AXES, SHIFT_BUTTONS, KEEP = 11, 28, "Logitech X52 Professional"

def main(src, dst):
    t = open(src, 'rb').read().decode('utf-8', 'surrogateescape')
    crlf = '\r\n' in t
    t = t.replace('\r\n', '\n')

    # --- rebuild the device table with the X52 alone ---------------------
    m = re.search(r'  deviceMapping\{\n(?:.*?\n)*?  \}\n', t)
    keep = re.search(r'    joystick\{\n(?:(?!    \}).)*?name:t="' + KEEP + r'[^\n]*\n(?:.*?\n)*?    \}\n', m.group(0), re.S)
    blk = keep.group(0)
    blk = re.sub(r'axesOffset:i=\d+', 'axesOffset:i=0', blk)
    blk = re.sub(r'buttonsOffset:i=\d+', 'buttonsOffset:i=0', blk)
    blk = re.sub(r'axesCount:i=\d+', 'axesCount:i=11', blk)
    blk = re.sub(r'buttonsCount:i=\d+', 'buttonsCount:i=39', blk)
    t = t[:m.start()] + '  deviceMapping{\n' + blk + '  }\n' + t[m.end():]

    dropped = []

    # --- shift axes -------------------------------------------------------
    X52_AXES = range(SHIFT_AXES, SHIFT_AXES + 11)          # 11..21
    X52_BTNS = range(SHIFT_BUTTONS, SHIFT_BUTTONS + 39)    # 28..66

    def axis(mo):
        v = int(mo.group(1))
        if v in X52_AXES:
            return 'axisId:i=%d' % (v - SHIFT_AXES)
        dropped.append('axisId %d' % v)
        return 'axisId:i=-1'
    t = re.sub(r'axisId:i=(\d+)', axis, t)

    # --- shift buttons ----------------------------------------------------
    def button(mo):
        v = int(mo.group(1))
        if v in X52_BTNS:
            return 'joyButton:i=%d' % (v - SHIFT_BUTTONS)
        dropped.append('joyButton %d' % v)
        return 'joyButton:i=-1'
    t = re.sub(r'joyButton:i=(\d+)', button, t)

    # drop binds that landed on the removed device
    t = re.sub(r'^      (?:joyButton|axisId):i=-1\n', '', t, flags=re.M)
    # remove blocks that became empty
    t = re.sub(r'    [A-Za-z0-9_]+\{\n    \}\n\n?', '', t)
    t = re.sub(r'\n\n\n+', '\n\n', t)

    if crlf: t = t.replace('\n', '\r\n')
    open(dst, 'wb').write(t.encode('utf-8', 'surrogateescape'))
    print("wrote", dst)
    for d in sorted(set(dropped)): print("  dropped:", d)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
