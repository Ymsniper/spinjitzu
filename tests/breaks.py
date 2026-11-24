#!/usr/bin/env python3
# Breaks the code on purpose, one way at a time, and checks that the checks
# then fail. A check that passes on broken code checks nothing.
#
#   python3 tests/breaks.py
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENGINE = "src/engine.hpp"
SESSION = "src/spike/session.hpp"
CORE = "src/core.hpp"
SETTINGS = "src/settings.hpp"
THEME = "src/app/theme.hpp"
BRIDGE = "src/bridge.hpp"
DEVICES = "src/devices.hpp"
TESTS = ["tests/engine_test.cpp", "tests/spike_test.cpp", "tests/core_test.cpp", "tests/bridge_test.cpp", "tests/theme_test.cpp", "tests/devices_test.cpp"]
EVDEV = subprocess.run(["pkg-config", "--cflags", "--libs", "libevdev"], capture_output=True, text=True).stdout.split()

BREAKS = [
    ("the counter folds whole turns away again",
     "        open_ = true;\n        return pending_;",
     "        open_ = true;\n        { const int64_t w = (sent_ + pending_) / c_; sent_ -= w * c_; asked_ -= (__int128)w * c_ * kFullTurnMilli; }\n        return pending_;"),
    ("rounding truncates",
     "return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);",
     "return a / b;"),
    ("each turn is rounded on its own",
     "pending_ = (int64_t)(roundDiv(asked_, kFullTurnMilli) - sent_);",
     "pending_ = (int64_t)roundDiv((__int128)milli * c_, kFullTurnMilli);"),
    ("a turn cut short is made up by the next",
     "asked_ -= (__int128)(pending_ - delivered) * kFullTurnMilli;",
     ""),
    ("the curve may run backwards",
     "share = std::clamp(std::max(share, share_), 0.0, 1.0);",
     "share = std::clamp(share, 0.0, 1.0);"),
    ("a turn left is not the mirror of one right",
     "due = std::llround(share * (double)total_);",
     "due = (int64_t)std::floor(share * (double)total_);"),
    ("a late tick sends its whole backlog at once",
     "const int64_t n = std::clamp<int64_t>(due - sent_, -cap_, cap_);",
     "const int64_t n = due - sent_;"),
    ("a turn stopped goes on sending",
     "int64_t stop() { done_ = true; return sent_; }",
     "int64_t stop() { return sent_; }"),
    ("release sends counts it does not report",
     "        const int64_t ahead = net_ * o_.dir;",
     "        { const double dt = (double)(now - last_) / kSecond; last_ = now; net_ += o_.dir * (int64_t)(o_.countsPerSec * dt); }\n        const int64_t ahead = net_ * o_.dir;"),
    ("the hand counts after the spin is over",
     "void hand(int64_t dx) { if (!done()) net_ += dx; }",
     "void hand(int64_t dx) { net_ += dx; }"),
    ("an overshoot keeps the old goal",
     "if (left < -c_ / 2) goal_ += c_ * o_.dir;",
     "if (false) goal_ += c_ * o_.dir;"),
    ("a finish never gives up",
     "if (now - fin0_ > (Ns)(kLongestFinish * kSecond)) { phase_ = Phase::GaveUp; return 0; }",
     ""),
    ("a late spin tick sends a burst",
     "std::clamp<Ns>(now - last_, 0, 20 * kTick)",
     "std::max<Ns>(now - last_, 0)"),
    ("a slow spin finishes at its own speed",
     "top_ = std::max({v_, floorSpeed(), r / (0.5 * kFinishWithin)});",
     "top_ = std::max(v_, floorSpeed());"),
    ("calibration takes any count",
     "if (c < kLeastCalibration) return std::nullopt;",
     ""),
    ("the hand's counts lose their remainder",
     "rem_ = total - out * den_;",
     "rem_ = 0;"),
    ("a half millidegree rounds toward zero",
     "return std::llround(m + (m >= 0 ? 1e-7 : -1e-7));",
     "return (int64_t)m;"),
    (SESSION, "the slow and the fast turn are never compared",
     "speedMatters_ = std::fabs((double)(kSlow_ - kFast_)) > tol;",
     "speedMatters_ = false;"),
    (SESSION, "the last hand 360 is used, not the median",
     "c_ = s[s.size() / 2];",
     "c_ = cs_.back();"),
    (SESSION, "a held key's repeats press it again",
     "if (value != 1 || was) return;",
     "if (value == 0) return;"),
    (SESSION, "a key held at start acts on its first report",
     "void heldAtStart(int code) { if (code >= 0 && code < 768) down_[code] = true; }",
     "void heldAtStart(int code) { (void)code; }"),
    (SESSION, "the correction is taken the wrong way round",
     "const auto cv = spinjitzu::corrected(c_, c_, kMean);",
     "const auto cv = spinjitzu::corrected(c_, c_, -kMean);"),
    (SESSION, "the five turns use the hand's figure",
     "Ledger l(cVirtual_);",
     "Ledger l(c_);"),
    (SESSION, "a key while the tool turns acts",
     "if (phase_ == Phase::Sending) { say(\"a key while the tool turns is ignored\"); return; }",
     ""),
    (SESSION, "movement during the turn is left out of the correction",
     "if (phase_ == Phase::Sending) { duringSend_ += dx; correction_ += dx; }",
     "if (phase_ == Phase::Sending) { duringSend_ += dx; }"),
    (SESSION, "a turn that lost events still gives a figure",
     "if (void_) { say(\"that turn lost counts and gives no figure: %s again\", n(kTurns)); return; }",
     ""),
    (SESSION, "F10 runs with acceleration in the path",
     "if (speedMatters_) { say(\"%s means nothing while acceleration is in the path: %s for flat, then %s again\", n(kFive), n(kFlat), n(kTurns)); return; }",
     ""),
    (SESSION, "the hand 360 stays on F8 whatever is bound",
     "if (code == keys_[kHand]) {",
     "if (code == kF8) {"),
    (CORE, "Esc binds itself instead of cancelling",
     "out_.captured = code == KEY_ESC ? 0 : code;",
     "out_.captured = code;"),
    (CORE, "turn keys work when not armed",
     "if (!armed_) { say(keyName(code) + \" pressed, but not armed: click the seal at the top right\"); return; }",
     ""),
    (CORE, "a press during a turn starts another",
     "if (phase_ != Phase::Idle) return;\n        if (code == s_.game.boom.key)",
     "if (code == s_.game.boom.key)"),
    (CORE, "waiting for a key does not hold the binds off",
     "if (capturing_ >= 0) {",
     "if (false) {"),
    (CORE, "a boomerang whose key device went away spins on",
     "if (phase_ == Phase::Spinning && !spin_.done()) { spin_.stop();",
     "if (false) { spin_.stop();"),
    (CORE, "the hand's movement is left out of the boomerang",
     "else if (phase_ == Phase::Spinning) spin_.hand(scaler_(dx));",
     ""),
    (CORE, "a new hand figure leaves the virtual mouse's alone",
     "            s_.game.cVirtual = s_.game.cHand;\n",
     ""),
    (SETTINGS, "numbers in the file are never read",
     "return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(v);",
     "return false;"),
    (SETTINGS, "jitter read from the file is not held to range",
     "else if (k == \"jitter\") { if (numIn(v, d)) s.game.jitter = jitterIn(d); }",
     "else if (k == \"jitter\") { if (numIn(v, d)) s.game.jitter = d; }"),
    (CORE, "left click can be bound",
     "if (code == BTN_LEFT) { say(\"left click cannot be bound: press another\"); return; }",
     ""),
    (CORE, "a turn key while not armed says nothing",
     "if (!armed_) { say(keyName(code) + \" pressed, but not armed: click the seal at the top right\"); return; }",
     "if (!armed_) return;"),
    (CORE, "the arm key turns as well",
     "say(armed_ ? \"armed (\" + keyName(code) + \")\" : \"not armed (\" + keyName(code) + \")\");\n            return;",
     "say(armed_ ? \"armed (\" + keyName(code) + \")\" : \"not armed (\" + keyName(code) + \")\");"),
    (CORE, "the arm key does not tell the window",
     "out_.armedNew = true;\n            out_.armed = armed_;",
     ""),
    (CORE, "settings put back an Armed the arm key changed",
     "        s_ = s;\n        if (newLedger)",
     "        s_ = s;\n        armed_ = s.armed;\n        if (newLedger)"),
    (BRIDGE, "an arm key press the window has not seen is dropped",
     "        if (!out.armedNew && armedNew) { c.armedNew = true; c.armed = armed; }\n",
     ""),
    (CORE, "the speed check runs at the test's speed",
     "o.degPerSec = speedTest_ ? 4 * 360.0 : 360.0;",
     "o.degPerSec = 360.0;"),
    (CORE, "the speed check corrects the calibration",
     "            if (speedTest_) {\n                // Only a report",
     "            if (false) {\n                // Only a report"),
    (CORE, "a speed check off the mark passes",
     "const bool landed = std::fabs((double)k) <= tolerance();",
     "const bool landed = true;"),
    (DEVICES, "KDE on X11 is sent to KWin",
     "    if (session == \"x11\") return Desktop::X11;\n    if (current.find(\"KDE\") != std::string::npos) return Desktop::KdeWayland;",
     "    if (current.find(\"KDE\") != std::string::npos) return Desktop::KdeWayland;\n    if (session == \"x11\") return Desktop::X11;"),
    (DEVICES, "Sway's name keeps the spaces",
     "    for (char& c : name) if (c == ' ') c = '_';\n",
     ""),
    (DEVICES, "the xinput command always sets three profiles",
     "(xinputValues > 2 ? \" 0\" : \"\")",
     "\" 0\""),
    (DEVICES, "Sway's profile is read off the first input listed",
     "size_t p = json.find(\"\\\"accel_profile\\\"\", at);",
     "size_t p = json.find(\"\\\"accel_profile\\\"\");"),
    (CORE, "another profile keeps the old game's hand 360s",
     "        panic();\n        hands_.clear();",
     "        panic();"),
    (CORE, "letters typed in Spinjitzu's window turn",
     "if (focused_) {",
     "if (false) {"),
    (SETTINGS, "leaving a profile drops what was changed in it",
     "    lib.games[lib.current] = lib.s.game;\n",
     ""),
    (SETTINGS, "names may repeat in another case",
     "folded(lib.names[i]) == folded(name)",
     "lib.names[i] == name"),
    (SETTINGS, "the last profile can be deleted",
     "if (lib.names.size() < 2 || i >= lib.names.size()) return false;",
     "if (i >= lib.names.size()) return false;"),
    (SETTINGS, "the file from before profiles is not kept",
     "        else writeFile(old, readFile(path));\n",
     ""),
    (BRIDGE, "settings sent after a profile switch drop the switch",
     "gameNew_ = gameNew_ || newGame;",
     "gameNew_ = newGame;"),
    (THEME, "the ink fades to a grey nobody can read",
     "constexpr Rgb kSumi{0x2A, 0x21, 0x19};",
     "constexpr Rgb kSumi{0x8A, 0x81, 0x79};"),
]
BREAKS = [b if len(b) == 4 else (ENGINE,) + b for b in BREAKS]


def main():
    caught, missed = 0, []
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copytree(os.path.join(ROOT, "src"), os.path.join(tmp, "src"))
        shutil.copytree(os.path.join(ROOT, "tests"), os.path.join(tmp, "tests"))
        for path, name, old, new in BREAKS:
            src = open(os.path.join(ROOT, path)).read()
            if src.count(old) != 1:
                print(f"STALE  {name}: the line it breaks is no longer there once")
                missed.append(name)
                continue
            open(os.path.join(tmp, path), "w").write(src.replace(old, new))
            fails, built = [], True
            for t in TESTS:
                exe = os.path.join(tmp, "t")
                b = subprocess.run(["g++", "-std=c++20", "-O1", "-pthread", "-o", exe, os.path.join(tmp, t)] + EVDEV,
                                   capture_output=True, text=True)
                if b.returncode != 0:
                    print(f"BUILD  {name}: the broken copy does not build\n{b.stderr[:400]}")
                    built = False
                    break
                r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
                if r.returncode != 0:
                    fails += [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
            open(os.path.join(tmp, path), "w").write(src)
            if built and fails:
                caught += 1
                print(f"caught {name}: {fails[0][6:]}")
            else:
                missed.append(name)
                print(f"MISSED {name}")
    print(f"{caught} of {len(BREAKS)} breaks caught")
    return 0 if not missed else 1


if __name__ == "__main__":
    sys.exit(main())
