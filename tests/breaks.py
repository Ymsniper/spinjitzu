#!/usr/bin/env python3
# Breaks src/engine.hpp on purpose, one way at a time, and checks that
# tests/engine_test.cpp then fails. A check that passes on broken code checks
# nothing.
#
#   python3 tests/breaks.py
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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
]


def main():
    src = open(os.path.join(ROOT, "src", "engine.hpp")).read()
    caught, missed = 0, []
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "src"))
        os.makedirs(os.path.join(tmp, "tests"))
        shutil.copy(os.path.join(ROOT, "tests", "engine_test.cpp"), os.path.join(tmp, "tests"))
        for name, old, new in BREAKS:
            if src.count(old) != 1:
                print(f"STALE  {name}: the line it breaks is no longer there once")
                missed.append(name)
                continue
            open(os.path.join(tmp, "src", "engine.hpp"), "w").write(src.replace(old, new))
            exe = os.path.join(tmp, "t")
            b = subprocess.run(["g++", "-std=c++20", "-O1", "-o", exe, os.path.join(tmp, "tests", "engine_test.cpp")],
                               capture_output=True, text=True)
            if b.returncode != 0:
                print(f"BUILD  {name}: the broken copy does not build\n{b.stderr[:400]}")
                missed.append(name)
                continue
            r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
            fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
            if r.returncode != 0 and fails:
                caught += 1
                print(f"caught {name}: {fails[0][6:]}")
            else:
                missed.append(name)
                print(f"MISSED {name}")
    print(f"{caught} of {len(BREAKS)} breaks caught")
    return 0 if not missed else 1


if __name__ == "__main__":
    sys.exit(main())
