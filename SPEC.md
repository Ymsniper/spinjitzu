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

## 7b. Who runs what

- One thread, the tick thread, owns every engine object. Input arrives on
  other threads as timestamped events through a single-producer queue; the
  tick thread applies them, then steps the engine, then sends.
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
- `src/input`: device discovery, bind capture, raw `REL_X` reader.
- `src/out`: the virtual mouse the turns go out through.
- `src/ui`: the menu, a separate window (raylib + Dear ImGui).
- Profiles in `~/.config/spinjitzu/<profile>.cfg`, `key = value`.

## 10. Menu

A header always shows the profile, its `C` with a calibrated or stale
badge, an armed switch and the panic key. A footer
shows the last turn: degrees asked, counts sent, and how far off that is.

- **Calibrate**: steps 1 to 3 above, with a live counter; Test 360 and Test 90.
  Other tabs are greyed out until the profile is calibrated.
- **Turns**: one row per bind: key, degrees, direction, speed
  and jitter overrides. "+ Add turn".
- **Boomerang**: bind, direction, spin speed, finish.
- **Motion**: default speed, curve and jitter.
- **Output**: which mouse is read, the panic key.

## 11. Checks

Offline, before anyone plays:
- E1 to E4 over 10000 random turns and spins, with awkward `C` (prime, 211,
  1000003) and degrees (0.001, 7.5, 90, 359.999, -720).
- Boomerang with random hand movement, including on the release
  tick and during the finish.
- Calibration with overshoot, correction, either direction, vertical noise,
  and a refused near-zero result.
- Each check broken on purpose once, to show it can fail.
- The threads under the thread sanitizer.

One live check by the user, in a practice area: press the 360 bind 20 times;
the crosshair lands on the same pixel every time. Hold free
spin while moving the mouse and let go: the view faces the start.
