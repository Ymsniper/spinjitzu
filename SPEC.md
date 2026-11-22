# Spinjitzu: specification

Turn the player in a game by an exact number of degrees, from any key or
mouse button. A free and open source take on the "perfect 360" button of
some gaming mice, for Linux.

## 1. Words

- **count**: one unit of horizontal mouse movement, as the kernel reports it
  (`REL_X`). Right is positive.
- **C**: counts per 360, for one game at one in-game sensitivity. Always a
  positive integer.
- **turn**: one press of a turn bind; moves the view by a set number of degrees.
- **boomerang**: spins for as long as its bind is held, then finishes facing
  the way the view faced when the bind was pressed.

## 2. Calibration

1. The user presses the calibrate key (a global bind, default F8), so the
   game keeps focus.
2. They turn a full 360 by hand, using anything on screen as the reference.
3. They press the calibrate key again.

`C` is the absolute value of the net `REL_X` between the two presses.
Overshooting and correcting is fine: wherever the user stops is taken as
exactly 360. `REL_Y` is ignored. A result under 200 counts is refused (the
key was pressed twice by mistake, or the wrong mouse was read).

A calibration belongs to a profile (a game and its sensitivity). The hand
calibration is taken on the real mouse.

**Correction.** A hand 360 a few counts off becomes drift: 20 presses carry
20 times the error. So the virtual mouse's figure is corrected: the tool
turns its current `C` through it, the user corrects by hand to the
reference and presses the key. With `k` the hand's correction in hand
counts, signed in the turn's direction (positive: the tool fell short), the
tool's `C` covered `360 * (1 - k / C_hand)` degrees, so its new value is
`C_virtual = round(C_virtual * C_hand / (C_hand - k))`. It can be run again
until a turn lands where it started. The menu refuses a turn with no
calibration.

**Speed check.** Once a test lands and says checked, the test key turns the
same `C` four times as fast. If the game turns the view by the counts alone,
it lands on the mark again; if it lands off, the game sees acceleration on the
virtual mouse, and a calibration taken at one speed is wrong at every other. The
correction is only reported (by how many counts), never applied: `C` stays as
it was. This works on any desktop and in any game, where reading the
desktop's settings cannot tell whether the game reads raw or accelerated
motion.

Converting the hand's counts to the virtual mouse's counts uses
`C_virtual / C_hand`, carrying the remainder.

## 3. Exactness

These hold for every speed, jitter and degree setting, and are checked
offline.

- **E1, a turn adds up.** The counts sent for one turn sum to that turn's
  target exactly.
- **E2, turns do not drift.** Each turn's degrees are rounded to 0.001 (typed
  0.0005 is 0.001, typed -0.0005 is -0.001). After any sequence of turns
  whose rounded degrees total `D` (signed), the counts sent total
  `round(D * C / 360)`, halves rounded away from zero, so a turn left is the
  mirror of the same turn right. Four 90s are exactly one `C`. This holds
  between turns that ran to the end; see section 8 for one cut short.
- **E3, speed and jitter change timing only.** Never the total.
- **E4, a boomerang comes home.** On the tick it ends, the net horizontal
  movement since the bind was pressed, the tool's own plus the hand's,
  is a whole multiple of `C`. What the hand moves after that is
  the user's own. The only exception is a finish that gives up (section 6).

## 4. Movement

- The tool sends on a steady 1 ms tick (absolute-deadline sleeps).
- Time is whole nanoseconds on the monotonic clock, everywhere.
- **Speed** is the average turn rate in degrees per second, slider plus typed
  box, 30 to 20000. A turn lasts `|degrees| / speed`, at least one tick and
  at most 5 s: a turn that would take longer is sped up to fit.
- A tick sends no more than four ticks' worth at the turn's peak rate. A tick
  that came late (the system stalled) catches up over the next few ticks
  instead of jumping, and the turn still ends on its total.
- **Curve**: smooth (minimum jerk: slow start, fast middle, slow end, the way
  a hand moves) or constant. Each tick sends the rounded cumulative curve
  minus what was already sent, so the sum is exact by construction.
- **Jitter**: an amount in percent (0 to 30, slider plus typed box) and a
  no-jitter box. It bends the curve with smooth seeded noise that is zero at
  both ends and never runs the turn backwards. Only the timing of the counts
  changes (E3).

## 5. The hand

Nothing keeps the hand from the game. While a turn runs the hand and the
tool add up, so a turn is exact while the hand keeps still. During a
boomerang the hand's counts are added to the net (section 6).

## 6. Boomerang

- Holding the bind spins at the spin speed in the chosen direction, reaching
  that speed over a short ramp.
- The hand's motion reaches the game while spinning and finishing, and its
  counts are added to the net.
- On release the goal is fixed: the next whole multiple of `C` at or ahead of
  the net in the spin direction. A net already on a multiple ends at once.
  A net behind the start (the hand pulled back further than the spin went)
  goes forward to the start. The finish slows evenly to stop exactly on the
  goal, starting from the speed it had, or faster where that is needed to be
  home within 2 s. Option: "shortest", the nearest multiple instead.
- During the finish the remaining distance is recomputed every tick
  from the net, hand included. If the hand carries the view more than half a
  turn past the goal, or pulls it back more than half a turn further than it
  had left at release, the goal moves by a whole turn to the nearer one. A finish still running after 3 s gives up where it
  is and says so.

## 7. Binds and timing

- Any key or button on any input device, captured with "press the key".
  Devices are read directly (`/dev/input/event*`), never grabbed: the key
  also reaches the game, so bind keys the game does not use.
- A press while a turn runs is ignored. Key repeat is ignored. Two binds at
  once: the first one wins.
- Left click cannot be bound: it fires in every game and clicks the window.
  Esc cancels a capture. One key binds one thing; the menu refuses a clash.
- **Armed**: turn and boomerang keys work only while armed. An arm key
  (unbound at first) switches it from inside the game. Not armed, a turn key
  says why in the log.

## 7a. Desktops

- The window runs on any desktop (raylib over GLFW, X11, XWayland on
  Wayland). What differs is the pointer acceleration the desktop puts on the
  tool's own mouse.
- KDE on Wayland: the tool sets its mouse flat through KWin when it starts.
- X11 sessions (KDE on X11 too), Sway, Hyprland: the profile is read where it
  can be (`xinput`, `swaymsg`) and the command that makes the tool's mouse
  flat is shown, to copy and run. A desktop is set by the tool only once its
  command is confirmed there.
- GNOME has one setting for every mouse: it is read and its command shown
  with that warning, never run by the tool.
- Others: the tool says it does not know the desktop.
- Whatever the desktop, the speed check (section 2) says whether the game
  sees acceleration at all.

## 7b. Who runs what

- One worker thread owns the devices and every engine object: it reads the
  devices, steps the engine on a 1 ms timer while something turns, and
  sends. The window is another thread; it hands over settings and commands
  and reads what the worker says, through one lock held only to copy.
- Armed goes to the worker as its own order, never inside settings: settings
  sent before the window heard of an arm key press would put the old state
  back. The worker tells every change of Armed back, and an untold change is
  kept until the window looks.
- Input is checked where it enters: degrees beyond 1000 turns or not a
  number are refused, speed and jitter are held to their ranges, a `C` under
  the calibration floor is refused.

## 8. Safety

- A turn lasts at most 5 s (section 4). A boomerang runs while its bind is
  held and through its finish (at most 3 s), and stops if the bind's device
  disappears.
- **Panic key** (default Pause): stops everything.
- **A turn cut short** (panic, a lost device, a failed write) keeps what it
  sent and drops the rest: the drift-free count takes back the part not sent,
  so the next turn does not make it up. The footer says "stopped: N of M
  counts".

## 9. Pieces

- `src/engine.hpp`: everything in sections 2 to 6 that has no device in it:
  calibration, targets, curves, jitter, boomerang. Tested offline.
- `src/core.hpp`: the app with no window and no device: binds, calibration
  and the speed check, turns, boomerang, Armed and the arm key, panic.
- `src/settings.hpp`: the settings file. `src/bridge.hpp`: the one lock
  between the window and the worker.
- `src/devices.hpp`: device discovery, the raw reader, the `uinput` mouse,
  key names, the desktop and its acceleration.
- `src/app`: the window (raylib + Dear ImGui) and the worker. `theme.hpp`:
  the colours, free of any library so a check can hold them to contrast.
  `scroll.hpp`: the fonts, the logo, the paper, the frame, the brush, the
  seal, the ensō and the opening.
- `assets/`: Shojumaru (OFL, shipped unchanged, reserved name), a Noto Serif
  CJK JP subset (OFL), the Sensei, each font with its licence beside it. They
  are carried inside the binary.
- Settings in `~/.config/spinjitzu/spinjitzu.cfg`, `key = value`.

## 10. Menu

The window is a handscroll (makimono) of ink on washi: a wooden roller at the
left, an indigo brocade cover at the right, brush strokes for rules. Colours
are six named tokens (paper, ink, cinnabar, gold, indigo, wood); every pair
that carries text meets WCAG contrast, ink 7:1, the rest 4.5:1, on the
darkest the paper may get under text. Every control has an ink border.
Shojumaru sets the title and the tabs; the Noto subset sets the rest and
スピン術. Cinnabar is kept for the seal, errors and the open tab's stroke.

- **Opening**: the scroll lies tied shut at the right, in its brocade with
  the gold Sensei and a スピン術 title slip, then unrolls right to left, the
  way a handscroll is read, until the roller is left (about 1.1 s). Any key or
  click skips it and reaches no control; a desktop set to no animation (KDE's
  speed at 0, GNOME's switch off) never sees it.
- **Header**: the Sensei in ink, the title, `C` by hand and virtual with
  checked or not, what the tool is doing. At the right, Armed as a seal:
  cinnabar with スピン術 cut out in white when armed (it stamps on), an empty
  frame in full ink when not (never faint), the words beside it, and the arm
  key's bind. The window's title says armed or not, for the taskbar.
- **Calibrate**: the steps, the hand 360 and test keys, the speed check's
  result, a live counter, the log.
- **Turns**: one row per bind: key, direction, degrees, its own speed and
  jitter or the defaults, and an ensō: a brush circle from the top, as far as
  the turn goes and round its way, "2x" inside for two turns. "+ Add turn".
- **Boomerang**: bind, direction, spin speed, finish.
- **Motion**: default speed, curve and jitter.
- **Output**: which mouse is read, the desktop and its
  acceleration (section 7a), the panic key.
- **Footer**: the last turn, its degrees and counts, "stopped: N of M" when
  cut short, and an ensō drawing the turn as it goes out; spinning during a
  boomerang.
- `spinjitzu --shot out.png <tab> [seconds] [armed]` draws the window off
  screen into a picture, at a set time of the opening, without opening any
  device or writing the settings.

## 11. Checks

Offline, before anyone plays:
- E1 to E4 over 10000 random turns and spins, with awkward `C` (prime, 211,
  1000003) and degrees (0.001, 7.5, 90, 359.999, -720).
- Boomerang with random hand movement, including on the release
  tick and during the finish.
- Calibration with overshoot, correction, either direction, vertical noise,
  and a refused near-zero result.
- The speed check: it runs four times as fast, reports landed or off by how
  much, and leaves the settings exactly as they were.
- The arm key: it switches, never turns, binds while a capture waits, and no
  press is lost or undone between the threads.
- Every colour pair that carries text, held to its contrast.
- Every character in the source's strings is in the fonts (`tests/glyphs.py`).
- The desktop: which one it is from the environment, what `xinput` and
  `swaymsg` say, and each desktop's command.
- Each check broken on purpose once, to show it can fail (`tests/breaks.py`).
- The threads under the thread sanitizer.

One live check by the user, in a practice area: press the 360 bind 20 times;
the crosshair lands on the same pixel every time. Hold free
spin while moving the mouse and let go: the view faces the start.
