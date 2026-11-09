# War Thunder simulator profile for the X52 Pro

`x52pro_jets_sim.blk` is a controls export tuned for jets in simulator mode on
the physical stick (axis offsets 11-21, the "Logitech X52 Professional" entry).

Import it in War Thunder: **Controls -> the folder icon (Load) -> pick the
file**. Export a backup of your current setup first.

## What it changes

| Setting | Value | Why |
| --- | --- | --- |
| ailerons innerDeadzone | 0.08 | swallows the stick's return bounce, which reads as a hard counter-roll on jets |
| ailerons kMul | 0.9 | slightly less than full roll authority, keeps it controllable |
| elevator innerDeadzone | 0.04 | same bounce, less critical on pitch |
| rudder innerDeadzone | 0.06 | the twist axis is the noisiest one |
| mouseAxisId | -1 on all three | the mouse can never contribute a flight axis |
| ID_WHEEL_BRAKE | joyButton 42 | the lower trigger brakes both wheels at once |

`joyButton 42` is physical button 14: War Thunder numbers the sticks in order,
and this device starts at button offset 28.

## Why the deadzone

Releasing the stick from full deflection makes it overshoot past centre. With a
2% deadzone that overshoot reaches the aircraft as a genuine opposite aileron
input. On a jet, a few percent for a few frames is a visible snap-back; on a
prop it is lost in the roll rate. 8% removes it without costing real authority.

If you want a softer response around the centre as well, set
**Controls -> Aircraft -> Ailerons -> Nonlinearity** to 1.3-1.5 in the game UI
rather than in the file.
