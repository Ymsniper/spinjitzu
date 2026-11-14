// Checks for src/spike/session.hpp against a made-up game: one that turns by
// the hand's counts and by the virtual mouse's counts times a ratio, with or
// without acceleration, and a made-up hand that is a little off when it turns
// a 360 and puts the crosshair back on the mark after every turn of the tool.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o spike_test tests/spike_test.cpp && ./spike_test
#include <cstdio>
#include <string>
#include "../src/spike/session.hpp"

using namespace spinjitzu;
using namespace spinjitzu::spike;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}

// The game: its view in hand counts. The hand's true 360 is `truth` counts.
// The virtual mouse's counts count `ratio` times, and with acceleration a
// count sent with others in the same tick counts more.
struct Game {
    int64_t truth = 16000;
    double ratio = 1.0;
    double accel = 0.0;   // extra gain per count in a tick
    double view = 0.0;
    void fromHand(int64_t d) { view += (double)d; }
    void fromTool(int64_t n) { view += (double)n * ratio * (1.0 + accel * std::fabs((double)n)); }
    // The hand's move back onto the nearest mark.
    int64_t backOnMark() const {
        const double mark = std::round(view / (double)truth) * (double)truth;
        return (int64_t)std::llround(mark - view);
    }
};

struct Rig {
    Session s;
    Game g;
    Ns now = 0;
    int64_t sent = 0;
    std::vector<std::string> log;
    void drain() { for (auto& l : s.take()) log.push_back(l); }
    void press(int k) { now += 20 * kTick; s.key(k, 1, now); now += 20 * kTick; s.key(k, 0, now); drain(); }
    void hand(int64_t d) { now += kTick; g.fromHand(d); s.hand(d, now); }
    // The hand turns a 360 that is `off` counts more than the truth.
    void hand360(int64_t off) {
        press(kF8);
        const int64_t want = g.truth + off;
        for (int64_t done = 0; done < want;) { const int64_t d = std::min<int64_t>(37, want - done); hand(d); done += d; }
        press(kF8);
    }
    // Ticks until the tool stops turning, with a stall now and then.
    void runSend() {
        int i = 0;
        while (s.wantsTicks() && i < 100000) {
            now += kTick + (i % 997 == 500 ? 60 * kTick : 0);
            const int64_t n = s.tick(now);
            g.fromTool(n);
            sent += n;
            i++;
        }
        drain();
    }
    // Back onto the mark by hand, then F8.
    void correct() {
        int64_t d = g.backOnMark();
        while (d) { const int64_t step = std::clamp<int64_t>(d, -29, 29); hand(step); d -= step; }
        press(kF8);
    }
    bool said(const std::string& part) const { for (auto& l : log) if (l.find(part) != std::string::npos) return true; return false; }
    void fullCheck() {
        press(kF9); runSend(); correct(); runSend(); correct();
        press(kF10); runSend(); correct();
    }
};

int main() {
    {
        Rig r;
        r.hand360(0);
        r.fullCheck();
        chk(r.s.c() == 16000 && !r.s.speedMatters() && r.s.cVirtual() == 16000 && r.s.fiveDone() && r.s.fiveOff() == 0,
            "a virtual mouse that turns the game as the hand does: same at both speeds, same figure, five turns on the mark");
        chk(r.said("Acceleration does not touch") && r.said("It turns the game as the hand does") && r.said("landed on the mark"),
            "and the log says so in words");
    }
    {
        Rig r;
        r.g.ratio = 1.02;
        r.hand360(0);
        r.fullCheck();
        const double got = (double)r.s.cVirtual() / 16000.0;
        chk(!r.s.speedMatters() && std::fabs(got - 1.0 / 1.02) < 0.0005, "a virtual mouse 2% too far: its figure comes out 1 / 1.02 of the hand's");
        chk(std::llabs(r.s.fiveOff()) <= 48 && r.said("different amount") && r.said("landed on the mark"),
            "and five turns at the corrected figure land on the mark");
    }
    {
        Rig r;
        r.g.accel = 0.002;
        r.hand360(0);
        r.fullCheck();
        chk(r.s.speedMatters() && r.said("Acceleration is in the virtual mouse's path"),
            "acceleration on the virtual mouse: the slow and the fast turn land apart, and it says so");
    }
    {
        Rig r;
        r.hand360(25);    // the hand overshot its own 360 by 25 counts and called it a turn
        r.fullCheck();
        chk(!r.s.speedMatters() && r.said("It turns the game as the hand does"),
            "a hand 360 25 counts long is the hand's error, within tolerance, not a mismatch");
    }
    {
        Rig r;
        r.hand360(-30); r.hand360(4); r.hand360(10);
        chk(r.s.c() == 16004 && r.said("hand 360 #3"), "several hand 360s: the median is used, not the last");
    }
    {
        Rig r;
        r.press(kF9);
        chk(r.said("F9 needs a hand 360 first") && !r.s.wantsTicks(), "F9 before any hand 360 is refused");
        r.press(kF10);
        chk(r.said("F10 needs F9 done first"), "F10 before F9 is refused");
        r.press(kF8);
        r.hand(150);
        r.press(kF8);
        chk(r.said("too small to be a 360") && r.s.c() == 0, "a hand turn of 150 counts is refused");
    }
    {
        Rig r;
        r.hand360(0);
        r.press(kF9);
        r.now += kTick; r.s.tick(r.now);
        r.press(kF10);
        r.drain();
        chk(r.said("a key while the tool turns is ignored"), "a key pressed while the tool turns is ignored");
        r.runSend();
        chk(r.sent == 16000, "the slow turn sends exactly C, through a stall");
    }
    {
        Rig r;
        r.hand360(0);
        r.s.key(kF9, 1, r.now + kTick);
        r.s.key(kF9, 2, r.now + 2 * kTick);
        r.s.key(kF9, 2, r.now + 3 * kTick);
        r.drain();
        int sends = 0;
        for (auto& l : r.log) if (l.find("slow turn:") != std::string::npos) sends++;
        chk(sends == 1, "a held key's repeats do not press it again");
    }
    {
        Session s;
        s.heldAtStart(kF8);
        s.key(kF8, 1, kTick);
        chk(s.take().empty(), "a key held when the tool started does nothing until it is let go");
        s.key(kF8, 0, 2 * kTick);
        s.key(kF8, 1, 3 * kTick);
        auto l = s.take();
        chk(!l.empty() && l[0].find("recording") != std::string::npos, "and works once pressed again");
    }
    {
        Rig r;
        r.hand360(0);
        r.press(kF9);
        r.runSend();
        r.hand(-40);
        r.correct();
        chk(r.said("slow turn: the hand corrected"), "hand movement after the turn is the correction");
        Rig q;
        q.hand360(0);
        q.press(kF9);
        q.now += kTick; q.s.tick(q.now);
        q.hand(12);
        q.runSend();
        q.correct();
        chk(q.said("the hand moved +12 counts while the tool was turning") && q.s.slowOff() == 0,
            "hand movement during the turn is noted, and counted in the correction");
    }
    {
        Session s;
        s.key(kF12, 1, kTick);
        chk(s.quit(), "F12 quits");
    }

    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
