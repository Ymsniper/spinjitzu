// Checks for src/engine.hpp against SPEC.md, on made-up input. Every check
// here has a deliberate break in tests/breaks.sh that makes it fail.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o engine_test tests/engine_test.cpp && ./engine_test
#include <cstdio>
#include <string>
#include <vector>
#include "../src/engine.hpp"

using namespace spinjitzu;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}

// The exact answer for E2, worked out from the whole total asked with
// nothing carried or shifted: round(D * C / 360), halves away from zero.
static int64_t oracle(__int128 askedTimes360000) { return (int64_t)roundDiv(askedTimes360000, kFullTurnMilli); }

// Runs a turn to its end at 1 ms ticks, with a tick `late` ms late at tick
// `lateAt`. Returns every step.
static std::vector<int64_t> run(Motion& m, Ns t0, int lateAt = -1, int late = 0) {
    std::vector<int64_t> steps;
    Ns now = t0;
    for (int i = 0; i < 100000 && !m.done(); i++) {
        now += kTick;
        if (i == lateAt) now += (Ns)late * kTick;
        steps.push_back(m.step(now));
    }
    return steps;
}

int main() {
    Rng rng(12345);
    auto pick = [&](int64_t lo, int64_t hi) { return lo + (int64_t)(rng.unit() * (double)(hi - lo + 1)); };

    // ── typed degrees ───────────────────────────────────────────────────────
    chk(millidegrees(7.5) == 7500 && millidegrees(-720) == -720000, "typed degrees become millidegrees");
    chk(millidegrees(0.0005) == 1 && millidegrees(-0.0005) == -1, "a typed half of a millidegree rounds away from zero");
    chk(!millidegrees(NAN) && !millidegrees(INFINITY) && !millidegrees(360001.0), "not a number, or past a thousand turns, is refused");
    chk(speedIn(NAN) == kSlowest && speedIn(0) == kSlowest && speedIn(1e9) == kFastest, "speed is held to its range");
    chk(jitterIn(-5) == 0 && jitterIn(99) == kMostJitter && jitterIn(NAN) == 0, "jitter is held to its range");

    // ── calibration ─────────────────────────────────────────────────────────
    {
        Calibrator c;
        chk(!c.finish(), "finishing a calibration never started gives nothing");
        c.start(); for (int i = 0; i < 199; i++) c.add(1);
        chk(!c.finish(), "a 199-count 360 is refused");
        c.start(); c.add(150); c.add(-30); c.add(80);
        chk(c.finish() == 200, "200 counts, with an overshoot taken back, is a 360");
        chk(!c.finish(), "finishing twice gives nothing the second time");
        c.start(); c.add(-5000); c.add(120);
        chk(c.finish() == 4880, "a 360 to the left counts the same as one to the right");
        c.add(999);
        chk(c.net() == -4880, "movement outside a calibration is not counted");
    }
    chk(corrected(16000, 16000, 0) == 16000, "no correction keeps C");
    chk(corrected(16000, 16000, 160) == 16162, "the tool fell short by 1%: its C grows by 1 / 0.99");
    chk(corrected(16000, 16000, -160) == 15842, "the tool went 1% too far: its C shrinks by 1 / 1.01");
    chk(!corrected(16000, 16000, 16000) && !corrected(100, 16000, 0), "a correction no turn could need is refused");
    {
        Scaler s(3, 7);
        int64_t hand = 0, out = 0;
        bool exact = true;
        for (int i = 0; i < 20000; i++) {
            const int64_t d = pick(-40, 40);
            hand += d;
            out += s(d);
            exact = exact && out == floorDiv(hand * 3, 7);
        }
        chk(exact, "hand counts in path counts never lose the remainder");
    }

    // ── E2: turns do not drift ──────────────────────────────────────────────
    {
        Ledger l(1000);
        int64_t sent = 0;
        const int64_t whole = l.begin(360000);
        l.end(whole);
        const int64_t tie = l.begin(-180);
        l.end(tie);
        sent = whole + tie;
        chk(sent == 1000 && l.sent() == oracle((__int128)359820 * 1000), "a whole turn then a half-count tie back rounds as the whole total does");
    }
    {
        const int64_t cs[] = {200, 211, 1000, 7919, 16384, 1000003};
        bool same = true, mirror = true;
        for (int64_t c : cs) {
            Ledger l(c), m(c);
            __int128 asked = 0;
            for (int i = 0; i < 5000; i++) {
                int64_t milli;
                const int kind = (int)pick(0, 3);
                if (kind == 0) milli = pick(-720000, 720000);
                else if (kind == 1) milli = pick(-5, 5);
                else if (kind == 2) milli = (pick(0, 1) ? 1 : -1) * (int64_t)(i % 4 == 0 ? 90000 : 7500);
                else milli = (pick(0, 1) ? 1 : -1) * 180;   // half-count ties at C = 1000
                asked += (__int128)milli * c;
                const int64_t n = l.begin(milli);
                l.end(n);
                const int64_t back = m.begin(-milli);
                m.end(back);
                same = same && l.sent() == oracle(asked);
                mirror = mirror && m.sent() == -l.sent();
            }
        }
        chk(same, "after every turn the total sent is round(D * C / 360), for every C");
        chk(mirror, "the same turns to the left send exactly the mirror");
    }
    {
        bool fours = true;
        for (int64_t c = 200; c < 3000; c += 7) {
            Ledger l(c);
            for (int i = 0; i < 4; i++) l.end(l.begin(90000));
            const bool one = l.sent() == c;
            l.end(l.begin(-720000));
            fours = fours && one && l.sent() == c - 2 * c;
        }
        chk(fours, "four 90s make exactly one C, and -720 two Cs back");
    }
    {
        Ledger l(1000);
        const int64_t n = l.begin(90000);
        l.end(100);
        const int64_t after = l.begin(0);
        l.end(after);
        const int64_t again = l.begin(90000);
        l.end(again);
        chk(n == 250 && after == 0 && again == 250 && l.sent() == 350,
            "a turn cut short keeps what it sent; the next turn does not make up the rest");
    }

    // ── E1, E3: a turn adds up, whatever its speed and jitter ───────────────
    {
        bool sums = true, oneWay = true, inTime = true, mirrored = true;
        for (int i = 0; i < 1500; i++) {
            const int64_t milli = pick(1, 720000) * (pick(0, 1) ? 1 : -1);
            const int64_t counts = (int64_t)roundDiv((__int128)milli * 16384, kFullTurnMilli);
            Motion::Orders o;
            o.degPerSec = (double)pick(30, 20000);
            o.curve = pick(0, 1) ? Curve::Smooth : Curve::Constant;
            o.jitterPercent = (double)pick(0, 30);
            o.seed = (uint32_t)i + 1;
            Motion m, b;
            m.start(counts, milli, o, 0);
            b.start(-counts, -milli, o, 0);
            const auto s = run(m, 0), t = run(b, 0);
            int64_t sum = 0;
            for (int64_t x : s) { sum += x; if ((x > 0 && counts < 0) || (x < 0 && counts > 0)) oneWay = false; }
            sums = sums && sum == counts && m.done();
            inTime = inTime && (Ns)s.size() <= m.length() / kTick + 8 && !s.empty();
            if (s.size() != t.size()) mirrored = false;
            else for (size_t k = 0; k < s.size(); k++) if (s[k] != -t[k]) mirrored = false;
        }
        chk(sums, "the steps of a turn add up to its total, for every speed, curve and jitter");
        chk(oneWay, "no step goes against the turn, even at 30% jitter");
        chk(inTime, "every turn ends on time, and takes at least one tick");
        chk(mirrored, "a turn left is step for step the mirror of the same turn right");
    }
    {
        Motion m;
        Motion::Orders o;
        o.degPerSec = 30;
        m.start(-32768, -720000, o, 0);
        chk(m.length() == (Ns)(kLongestTurn * kSecond), "a turn that would take 24 s is sped up to 5 s");
        Motion q;
        o.degPerSec = NAN;
        q.start(500, 90000, o, 0);
        const auto s = run(q, 0);
        int64_t sum = 0;
        for (int64_t x : s) sum += x;
        chk(q.done() && sum == 500, "a speed that is not a number still ends, at the slowest speed");
    }
    {
        Motion m;
        Motion::Orders o;
        o.degPerSec = 2000;
        m.start(16384, 360000, o, 0);
        const auto s = run(m, 0, 20, 100);
        int64_t sum = 0, most = 0;
        for (int64_t x : s) { sum += x; most = std::max<int64_t>(most, std::llabs(x)); }
        const double peakPerTick = 16384.0 * 1.875 / (180.0);
        chk(sum == 16384 && most <= (int64_t)std::ceil(4 * peakPerTick) + 1,
            "a tick 100 ms late catches up over a few ticks, and the turn still ends on its total");
    }
    {
        Motion m;
        Motion::Orders o;
        m.start(800, 90000, o, 0);
        m.step(50 * kTick);
        const int64_t sent = m.sent();
        chk(m.stop() == sent && m.done() && m.step(60 * kTick) == 0, "a turn stopped sends nothing more");
    }

    // ── E4: a boomerang comes home ──────────────────────────────────────────
    {
        const int64_t cs[] = {200, 997, 16384, 52000};
        int home = 0, tries = 0, gaveUp = 0;
        bool exact = true;
        for (int64_t c : cs)
            for (int i = 0; i < 400; i++) {
                Spin s;
                Spin::Orders o;
                o.dir = pick(0, 1) ? 1 : -1;
                o.countsPerSec = (double)pick(30, 20000) * (double)c / 360.0;   // the menu's degrees a second
                o.finish = pick(0, 1) ? Finish::Forward : Finish::Shortest;
                const bool letThrough = pick(0, 1);
                Ns now = 0;
                s.begin(c, o, now);
                const int held = (int)pick(0, 3000);
                int64_t truth = 0;   // what reached the game: the tool's and the hand's
                auto handNow = [&] {
                    if (!letThrough) return;
                    const int64_t d = pick(-6, 6);
                    s.hand(d);
                    truth += d;
                };
                for (int k = 0; k < held; k++) {
                    now += kTick;
                    handNow();
                    truth += s.step(now);
                }
                // released between two ticks, with the hand moving on that tick
                now += kTick / 2;
                handNow();
                s.release(now);
                for (int k = 0; k < 10000 && !s.done(); k++) {
                    now += kTick;
                    handNow();
                    truth += s.step(now);
                }
                tries++;
                if (s.phase() == Spin::Phase::Home) {
                    home++;
                    exact = exact && truth == s.net() && truth % c == 0;
                }
                if (s.phase() == Spin::Phase::GaveUp) gaveUp++;
            }
        printf("     boomerangs: %d of %d home, %d gave up\n", home, tries, gaveUp);
        chk(exact, "every spin that came home has moved a whole number of Cs, hand included, release between ticks included");
        chk(gaveUp == 0 && home == tries, "with an ordinary hand moving, every spin comes home");
    }
    {
        Spin s;
        Spin::Orders o;
        o.countsPerSec = 10000;
        s.begin(1000, o, 0);
        s.hand(1000);
        s.release(0);
        chk(s.phase() == Spin::Phase::Home && s.step(kTick) == 0, "a spin released on a whole turn is home at once");
        s.hand(55);
        chk(s.net() == 1000, "hand movement after the spin is over is the user's own");
        Spin b;
        b.begin(1000, o, 0);
        b.hand(-400);
        b.release(0);
        chk(b.goal() == 0, "a hand pulled back behind the start: the finish goes forward to the start");
    }
    {
        Spin s;
        Spin::Orders o;
        o.countsPerSec = 4000;
        s.begin(1000, o, 0);
        Ns now = 0;
        for (int k = 0; k < 100; k++) { now += kTick; s.step(now); }
        s.release(now);
        const int64_t first = s.goal();
        s.hand(first - s.net() + 900);    // the hand carries it 0.9 of a turn past the goal
        for (int k = 0; k < 5000 && !s.done(); k++) { now += kTick; s.step(now); }
        chk(s.goal() == first + 1000 && s.phase() == Spin::Phase::Home && s.net() % 1000 == 0,
            "carried 0.9 of a turn past the goal, the finish takes the next whole turn instead");
    }
    {
        Spin s;
        Spin::Orders o;
        o.countsPerSec = 4000;
        s.begin(1000, o, 0);
        Ns now = 0;
        for (int k = 0; k < 100; k++) { now += kTick; s.step(now); }
        s.release(now);
        bool stopped = false;
        for (int k = 0; k < 10000 && !s.done(); k++) { now += kTick; s.hand(-40 * (s.goal() > s.net() ? 1 : -1)); s.step(now); }
        stopped = s.phase() == Spin::Phase::GaveUp;
        chk(stopped && now <= (Ns)((kLongestFinish + 0.2) * kSecond), "a hand that keeps pushing it away: the finish gives up after 3 s");
    }
    {
        Spin s;
        Spin::Orders o;
        o.countsPerSec = 100000;
        o.ramp = 0;
        s.begin(16384, o, 0);
        s.step(kTick);
        const int64_t burst = s.step(kTick + 100 * kTick);
        chk(burst <= 2001, "a spin tick 100 ms late sends no more than 20 ms worth");
    }

    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
