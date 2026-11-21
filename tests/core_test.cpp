// Checks for src/core.hpp and src/settings.hpp, on made-up keys, hand
// movement and a made-up clock, with a made-up game that turns by the
// virtual mouse's counts and the hand's.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o core_test tests/core_test.cpp $(pkg-config --cflags --libs libevdev) && ./core_test
#include <cstdio>
#include <string>
#include "../src/core.hpp"

using namespace spinjitzu;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}

struct Rig {
    Core c;
    Settings s;
    Ns now = 0;
    int64_t sent = 0, view = 0;   // what the game turned, in counts
    std::vector<std::string> log;
    Rig() { c.use(s); }
    void drain() {
        Core::Out o = c.take();
        for (auto& l : o.log) log.push_back(l);
        if (o.newC) { s.cHand = o.cHand; s.cVirtual = o.cVirtual; s.checked = o.checked; c.use(s); }
        if (o.armedNew) s.armed = o.armed;   // as the window does
    }
    void press(int k) { now += 10 * kTick; c.key(k, 1, now); now += 10 * kTick; c.key(k, 0, now); drain(); }
    void hand(int64_t d) { now += kTick; view += d; c.hand(d, now); }
    void run(int most = 20000) {
        for (int i = 0; i < most; i++) { now += kTick; const int64_t n = c.tick(now); sent += n; view += n; if (c.idle() && i > 2) break; }
        drain();
    }
    void calibrate(int64_t counts) {
        press(s.keyCalibrate);
        for (int64_t d = 0; d < counts;) { const int64_t step = std::min<int64_t>(40, counts - d); hand(step); d += step; }
        press(s.keyCalibrate);
    }
    bool said(const std::string& part) const { for (auto& l : log) if (l.find(part) != std::string::npos) return true; return false; }
};

int main() {
    // ── calibration ─────────────────────────────────────────────────────────
    {
        Rig r;
        r.calibrate(16000);
        chk(r.s.cHand == 16000 && r.s.cVirtual == 16000 && !r.s.checked, "a hand 360 gives C, and the virtual mouse starts from it, not checked yet");
        r.press(r.s.keyTest);
        r.run();
        chk(r.sent == 16000, "the test 360 sends the virtual mouse's C");
        r.press(r.s.keyCalibrate);
        chk(r.s.checked && r.s.cVirtual == 16000, "landed on the mark: checked");
    }
    {
        Rig r;
        r.calibrate(16000);
        r.press(r.s.keyTest);
        r.run();
        for (int i = 0; i < 4; i++) r.hand(40);   // fell 160 counts short: corrected by hand
        r.press(r.s.keyCalibrate);
        chk(r.s.cVirtual == 16162 && !r.s.checked, "short by 1%: the virtual mouse's C grows by 1 / 0.99, and needs another test");
    }
    {
        Rig r;
        r.press(r.s.keyTest);
        chk(r.said("do a hand 360 first") && r.c.idle(), "a test before any hand 360 is refused");
        r.calibrate(150);
        chk(r.said("too small") && r.s.cHand == 0, "a hand turn of 150 counts is refused");
    }

    // ── the speed check ─────────────────────────────────────────────────────
    auto checkedRig = [](Rig& r) {
        r.calibrate(16000);
        r.press(r.s.keyTest);
        r.run();
        r.press(r.s.keyCalibrate);
    };
    // Ticks until a test 360 has gone out and the hand is asked for.
    auto testTicks = [](Rig& r) {
        r.press(r.s.keyTest);
        int ticks = 0;
        for (; ticks < 20000; ticks++) {
            r.now += kTick;
            const int64_t n = r.c.tick(r.now);
            r.sent += n;
            r.view += n;
            const Core::Out o = r.c.take();
            for (auto& l : o.log) r.log.push_back(l);
            if (o.doing.rfind("back on the mark", 0) == 0) break;
        }
        return ticks;
    };
    {
        Rig slow;
        slow.calibrate(16000);
        const int slowTicks = testTicks(slow);
        Rig r;
        checkedRig(r);
        const std::string before = toText(r.s);
        const int64_t sentBefore = r.sent;
        const int fastTicks = testTicks(r);
        r.press(r.s.keyCalibrate);
        chk(r.said("speed check: a 360 at four times") && r.sent - sentBefore == 16000 && fastTicks * 3 < slowTicks,
            "once checked, the test key runs the speed check: the same 360, four times as fast");
        chk(r.said("the fast 360 landed") && toText(r.s) == before, "a speed check that lands says so, and the calibration is exactly as it was");
    }
    {
        Rig r;
        checkedRig(r);
        const std::string before = toText(r.s);
        testTicks(r);
        for (int i = 0; i < 8; i++) r.hand(-40);   // the game turned 320 counts too far
        r.press(r.s.keyCalibrate);
        chk(r.said("the fast 360 was -320 counts off") && toText(r.s) == before,
            "a speed check that lands off says by how much, and still leaves the calibration alone");
        r.log.clear();
        testTicks(r);
        chk(r.said("speed check: a 360 at four times"), "the speed check can be run again");
    }

    // ── turns ───────────────────────────────────────────────────────────────
    auto armed = [](Rig& r) {
        r.calibrate(1000);
        TurnBind t;
        t.key = KEY_E;
        t.degrees = 90;
        r.s.turns.push_back(t);
        TurnBind back;
        back.key = KEY_Q;
        back.degrees = 0.18;
        back.dir = -1;
        r.s.turns.push_back(back);
        r.c.use(r.s);
        r.c.arm(true);
    };
    {
        Rig r;
        armed(r);
        for (int i = 0; i < 4; i++) { r.press(KEY_E); r.run(); }
        chk(r.sent == 1000, "four 90s send exactly C");
        r.press(KEY_Q); r.run();
        r.press(KEY_E); r.run();
        chk(r.sent == 1250, "turns of odd sizes still add up: 450 degrees less 0.18, at C 1000");
    }
    {
        Rig r;
        armed(r);
        r.c.arm(false);
        r.press(KEY_E); r.run();
        chk(r.sent == 0, "not armed: a turn key does nothing");
    }
    {
        Rig r;
        armed(r);
        r.c.arm(false);
        r.press(KEY_E);
        r.press(KEY_Z);
        chk(r.said("E pressed, but not armed") && !r.said("Z pressed"), "a turn key pressed while not armed says why nothing happened; an unbound key says nothing");
    }
    {
        Rig r;
        r.c.capture(kSlotTurn0, r.now);
        r.now += kTick; r.c.key(BTN_LEFT, 1, r.now);
        Core::Out o = r.c.take();
        r.now += kTick; r.c.key(BTN_LEFT, 0, r.now);
        r.now += kTick; r.c.key(KEY_K, 1, r.now);
        Core::Out p = r.c.take();
        chk(o.captureSlot < 0 && p.captureSlot == kSlotTurn0 && p.captured == KEY_K, "left click cannot be bound; the next key is");
    }
    {
        Rig r;
        armed(r);
        r.now += kTick; r.c.key(KEY_E, 1, r.now);
        r.now += kTick; r.c.key(KEY_E, 2, r.now);
        for (int i = 0; i < 50; i++) { r.now += kTick; r.sent += r.c.tick(r.now); }
        r.c.key(KEY_E, 2, r.now);
        r.c.key(KEY_E, 0, r.now);
        r.press(KEY_E);                       // let go and pressed again while the first turn runs
        r.run();
        chk(r.sent == 250, "a held key's repeats and a press during a turn start nothing");
    }
    {
        Rig r;
        armed(r);
        r.now += kTick; r.c.key(KEY_E, 1, r.now);
        for (int i = 0; i < 100; i++) { r.now += kTick; r.sent += r.c.tick(r.now); }
        r.press(r.s.keyPanic);
        r.drain();
        const Core::Out o = r.c.take();
        const int64_t part = r.sent;
        chk(part > 0 && part < 250 && o.last.stopped && o.last.sent == part, "panic mid-turn stops it, and the footer says how much went");
        r.c.key(KEY_E, 0, r.now);
        r.press(KEY_E); r.run();
        chk(r.sent == part + 250, "the next turn does not make up the part not sent");
    }

    // ── the arm key ─────────────────────────────────────────────────────────
    {
        Rig r;
        armed(r);
        r.s.keyArm = KEY_F6; r.c.use(r.s);
        r.press(KEY_F6);
        chk(!r.s.armed && r.said("not armed (F6)"), "the arm key switches Armed off, and the window hears of it");
        r.press(KEY_E); r.run();
        chk(r.sent == 0, "switched off by the arm key, a turn key does nothing");
        r.press(KEY_F6);
        r.press(KEY_E); r.run();
        chk(r.s.armed && r.sent == 250, "switched back on, the turn key turns");
    }
    {
        Rig r;
        armed(r);
        r.s.keyArm = KEY_E; r.c.use(r.s);    // the window never allows it, the core holds anyway
        r.press(KEY_E); r.run();
        r.press(KEY_E); r.run();
        chk(r.sent == 0 && r.s.armed, "an arm key that is also a turn key only switches Armed, never turns");
    }
    {
        Rig r;
        armed(r);
        r.s.keyArm = KEY_F6; r.c.use(r.s);
        r.now += kTick; r.c.key(KEY_F6, 1, r.now); r.now += kTick; r.c.key(KEY_F6, 0, r.now);
        Settings stale = r.s;                // sent before the window heard of the press
        stale.speed = 999;
        r.c.use(stale);
        r.press(KEY_E); r.run();
        chk(r.sent == 0 && !r.s.armed, "settings sent before the window heard of the arm key do not undo it");
    }
    {
        Rig r;
        armed(r);
        r.s.keyArm = KEY_F6; r.c.use(r.s);
        r.drain();
        r.c.capture(kSlotTest, r.now);
        r.now += kTick; r.c.key(KEY_F6, 1, r.now);
        const Core::Out o = r.c.take();
        chk(o.captureSlot == kSlotTest && o.captured == KEY_F6 && !o.armedNew, "while waiting for a key, the arm key binds and does not switch");
    }

    // ── binding ─────────────────────────────────────────────────────────────
    {
        Rig r;
        r.now += kTick; r.c.key(BTN_LEFT, 1, r.now);   // the click on "Bind" is still down
        r.c.capture(kSlotTurn0, r.now);
        r.now += kTick; r.c.key(BTN_LEFT, 0, r.now);
        Core::Out o = r.c.take();
        chk(o.captureSlot < 0, "the click that asked for a binding binds nothing");
        r.now += kTick; r.c.key(BTN_SIDE, 1, r.now);
        o = r.c.take();
        chk(o.captureSlot == kSlotTurn0 && o.captured == BTN_SIDE, "the next button pressed is bound, a mouse's side button too");
        r.c.capture(kSlotBoomerang, r.now);
        r.now += kTick; r.c.key(KEY_ESC, 1, r.now);
        o = r.c.take();
        chk(o.captureSlot == kSlotBoomerang && o.captured == 0, "Esc cancels");
    }
    {
        Rig r;
        armed(r);
        r.c.capture(kSlotTest, r.now);
        r.now += kTick; r.c.key(KEY_E, 1, r.now);
        r.run();
        chk(r.sent == 0 && r.c.take().log.empty(), "while waiting for a key, a turn key binds and does not turn");
    }

    // ── the boomerang ───────────────────────────────────────────────────────
    {
        Rig r;
        r.calibrate(16000);
        r.s.boom.key = KEY_R;
        r.s.boom.speed = 1440;
        r.c.use(r.s);
        r.c.arm(true);
        r.now += kTick; r.c.key(KEY_R, 1, r.now);
        for (int i = 0; i < 700; i++) {
            r.now += kTick;
            if (i % 3 == 0) r.hand(i % 2 ? 7 : -4);   // the hand moves the whole time
            const int64_t n = r.c.tick(r.now);
            r.sent += n; r.view += n;
        }
        r.c.key(KEY_R, 0, r.now);
        for (int i = 0; i < 5000 && !r.c.idle(); i++) {
            r.now += kTick;
            if (i % 5 == 0) r.hand(3);
            const int64_t n = r.c.tick(r.now);
            r.sent += n; r.view += n;
        }
        r.drain();
        chk(r.said("boomerang home") && r.view % 16000 == 0 && r.view != 0,
            "the boomerang spins while held and comes home a whole number of turns round, the hand moving the whole time");
    }
    {
        Rig r;
        r.calibrate(16000);
        r.s.boom.key = KEY_R;
        r.c.use(r.s);
        r.c.arm(true);
        r.now += kTick; r.c.key(KEY_R, 1, r.now);
        for (int i = 0; i < 100; i++) { r.now += kTick; r.c.tick(r.now); }
        r.c.keyDeviceGone();
        r.drain();
        chk(r.c.idle() && r.said("went away"), "the boomerang's key device gone: it stops, never spinning on with no way to let go");
    }

    // ── settings ────────────────────────────────────────────────────────────
    {
        Settings s;
        s.keyCalibrate = BTN_EXTRA;
        s.keyArm = KEY_F6;
        s.cHand = 16384;
        s.cVirtual = 16511;
        s.checked = true;
        s.speed = 1234.5;
        s.jitter = 7.25;
        s.curve = Curve::Constant;
        s.mouse = "E-Signal AQIRYS Polaris Wired Gaming Mouse";
        s.boom = {KEY_R, -1, 999.5, true, Finish::Shortest};
        s.turns.push_back({KEY_E, 7.5, -1, true, 333, Curve::Constant, 12});
        s.turns.push_back({BTN_SIDE, 180, 1, false, 720, Curve::Smooth, 0});
        const Settings b = fromText(toText(s));
        chk(b.keyCalibrate == BTN_EXTRA && b.keyArm == KEY_F6 && fromText("").keyArm == 0 && b.cHand == 16384 && b.cVirtual == 16511 && b.checked && b.speed == 1234.5 &&
                b.jitter == 7.25 && b.curve == Curve::Constant && b.mouse == s.mouse && b.boom.key == KEY_R &&
                b.boom.dir == -1 && b.boom.speed == 999.5 && b.boom.finish == Finish::Shortest && b.turns.size() == 2 &&
                b.turns[0].key == KEY_E && b.turns[0].degrees == 7.5 && b.turns[0].dir == -1 && b.turns[0].own &&
                b.turns[0].speed == 333 && b.turns[0].jitter == 12 && b.turns[1].key == BTN_SIDE,
            "settings read back exactly as written");
        const Settings bad = fromText("c_hand = abc\nspeed = nan\njitter = 99\nturn = KEY_E 1e12 1 own 720 smooth 0\nturn = KEY_Q\ncalibrate = KEY_NOTHING\n");
        chk(bad.cHand == 0 && bad.speed == 720 && bad.jitter == kMostJitter && bad.turns.empty() && bad.keyCalibrate == KEY_F8,
            "a broken file: what does not parse keeps its default, out of range is held to range");
        const std::string p = "/tmp/spinjitzu_core_test.cfg";
        chk(saveSettings(p, s) && loadSettings(p).cVirtual == 16511 && loadSettings("/nonexistent/x.cfg").cHand == 0,
            "settings saved and loaded from a file; a missing file gives the defaults");
        remove(p.c_str());
    }

    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
