// The arithmetic of Spinjitzu, with no device in it: calibration, how many
// counts each turn sends, how they are spread over time, and the boomerang.
// Every rule of SPEC.md sections 2 to 6 lives here so that it can be checked
// offline, on made-up input, before anyone plays. One thread owns these
// objects (SPEC 7b); none of them locks.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace spinjitzu {

using Ns = int64_t;   // nanoseconds on the monotonic clock
inline constexpr Ns kSecond = 1'000'000'000;
inline constexpr Ns kTick = 1'000'000;

// A hand 360 shorter than this is a key pressed twice by mistake, or the
// wrong mouse being read; and no path's C may be smaller.
inline constexpr int64_t kLeastCalibration = 200;

// Degrees are kept to a thousandth, as whole millidegrees, so that a typed
// 7.5 is exactly 7.5 every time it is turned.
inline constexpr int64_t kMilli = 1000;
inline constexpr int64_t kFullTurnMilli = 360 * kMilli;
inline constexpr int64_t kMostMilli = 1000 * kFullTurnMilli;   // a thousand turns

// The ranges the menu offers (SPEC 4).
inline constexpr double kSlowest = 30.0, kFastest = 20000.0;   // degrees a second
inline constexpr double kMostJitter = 30.0;                    // percent
inline constexpr double kLongestTurn = 5.0;                    // seconds
inline constexpr double kLongestFinish = 3.0;                  // seconds
inline constexpr double kFinishWithin = 2.0;                   // seconds, unhindered

// Typed degrees as millidegrees, halves away from zero, or nothing for a
// number that is not one or is past a thousand turns. The nudge keeps a
// typed 0.0005 from landing a hair under the half in binary.
inline std::optional<int64_t> millidegrees(double degrees) {
    if (!std::isfinite(degrees)) return std::nullopt;
    const double m = degrees * kMilli;
    if (std::fabs(m) > (double)kMostMilli) return std::nullopt;
    return std::llround(m + (m >= 0 ? 1e-7 : -1e-7));
}

inline double speedIn(double degPerSec) {
    return std::isfinite(degPerSec) ? std::clamp(degPerSec, kSlowest, kFastest) : kSlowest;
}
inline double jitterIn(double percent) {
    return std::isfinite(percent) ? std::clamp(percent, 0.0, kMostJitter) : 0.0;
}

// a / b to the nearest whole number, halves away from zero, for any signs:
// a turn left is the mirror of the same turn right.
inline __int128 roundDiv(__int128 a, __int128 b) {
    if (b < 0) { a = -a; b = -b; }
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

// a / b rounded down, for carrying remainders the same way in either
// direction.
inline int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// ── calibration (SPEC 2) ────────────────────────────────────────────────────

// The net horizontal movement between the two presses of the calibrate key.
// Where the hand stops is taken as exactly 360, however it got there.
class Calibrator {
public:
    void start() { on_ = true; net_ = 0; }
    void cancel() { on_ = false; net_ = 0; }
    bool active() const { return on_; }
    // Horizontal counts only: vertical movement is no part of a turn.
    void add(int64_t dx) { if (on_) net_ += dx; }
    int64_t net() const { return net_; }
    // C, or nothing: not started, or too small to be a 360.
    std::optional<int64_t> finish() {
        if (!on_) return std::nullopt;
        on_ = false;
        const int64_t c = std::llabs(net_);
        if (c < kLeastCalibration) return std::nullopt;
        return c;
    }

private:
    bool on_ = false;
    int64_t net_ = 0;
};

// A path's C after one correction (SPEC 2): the tool turned `cPath` counts
// through the path and the hand then moved `k` hand counts to reach the
// reference, positive where the tool fell short. Nothing if that is not a
// correction a turn could need.
inline std::optional<int64_t> corrected(int64_t cPath, int64_t cHand, int64_t k) {
    if (cPath < kLeastCalibration || cHand < kLeastCalibration) return std::nullopt;
    const int64_t covered = cHand - k;            // hand counts the tool's turn was worth
    if (covered <= cHand / 4) return std::nullopt;
    const __int128 c = roundDiv((__int128)cPath * cHand, covered);
    if (c < kLeastCalibration || c > (__int128)cPath * 4) return std::nullopt;
    return (int64_t)c;
}

// The hand's counts in a path's counts, the remainder carried, so that a
// path turning the game differently from the hand still adds up (SPEC 2).
class Scaler {
public:
    Scaler(int64_t cPath, int64_t cHand) : num_(cPath), den_(cHand) {}
    int64_t operator()(int64_t hand) {
        const int64_t total = hand * num_ + rem_;
        const int64_t out = floorDiv(total, den_);
        rem_ = total - out * den_;
        return out;
    }

private:
    int64_t num_, den_, rem_ = 0;
};

// ── what each turn sends (SPEC 3, E2; SPEC 8) ───────────────────────────────

// Turns rarely divide C evenly, so each one sends the counts that bring the
// total sent to the total asked, rounded: what one turn rounds away the next
// takes back, and any run of turns totalling D degrees has sent exactly
// round(D * C / 360). The total asked is never shifted by whole turns, since
// that would move a half-count tie to the other side of zero.
//
// A turn is booked when it ends, with what it actually sent. One cut short
// takes back the part not sent, so the next turn does not make it up.
class Ledger {
public:
    explicit Ledger(int64_t c) : c_(std::max<int64_t>(c, kLeastCalibration)) {}
    int64_t c() const { return c_; }
    int64_t sent() const { return sent_; }

    // The counts a turn of `milli` millidegrees is to send. An earlier turn
    // never ended counts as having sent everything.
    int64_t begin(int64_t milli) {
        if (open_) end(pending_);
        asked_ += (__int128)milli * c_;
        pending_ = (int64_t)(roundDiv(asked_, kFullTurnMilli) - sent_);
        open_ = true;
        return pending_;
    }
    // What the turn begun last actually sent.
    void end(int64_t delivered) {
        if (!open_) return;
        sent_ += delivered;
        asked_ -= (__int128)(pending_ - delivered) * kFullTurnMilli;
        open_ = false;
    }

private:
    int64_t c_;
    __int128 asked_ = 0;   // in 1 / 360000 of a count
    int64_t sent_ = 0;
    int64_t pending_ = 0;
    bool open_ = false;
};

// ── how a turn is spread over time (SPEC 4) ─────────────────────────────────

enum class Curve { Smooth, Constant };

// The share of the turn done at time t, 0 to 1. Smooth is the minimum-jerk
// path a hand takes between two points: it starts and ends at rest.
inline double curveAt(Curve c, double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (c == Curve::Constant) return t;
    return t * t * t * (10.0 + t * (-15.0 + 6.0 * t));
}

// The curve's steepest slope, as a multiple of the average.
inline double curvePeak(Curve c) { return c == Curve::Constant ? 1.0 : 1.875; }

// Small seeded random numbers: the same seed gives the same wobble, so a turn
// that misbehaves in a check can be run again exactly.
class Rng {
public:
    explicit Rng(uint32_t seed) : s_(seed * 2654435761u + 0x9E3779B9u) { if (!s_) s_ = 1; }
    uint32_t next() { s_ ^= s_ << 13; s_ ^= s_ >> 17; s_ ^= s_ << 5; return s_; }
    double unit() { return (next() >> 8) * (1.0 / 16777216.0); }   // 0 to 1
private:
    uint32_t s_;
};

// The jitter: a few slow waves of random rate and phase, scaled by sin(pi t)
// so that it is zero where the turn starts and where it ends. It only moves
// counts earlier or later; the end of the curve stays at 1.
class Wobble {
public:
    Wobble() = default;
    Wobble(uint32_t seed, double share) : share_(share) {
        Rng r(seed);
        double total = 0;
        for (Wave& w : waves_) {
            w.rate = 1.0 + 3.0 * r.unit();
            w.phase = 6.283185307179586 * r.unit();
            w.weight = 0.5 + r.unit();
            total += w.weight;
        }
        for (Wave& w : waves_) w.weight /= total;
    }
    double at(double t) const {
        if (share_ <= 0.0) return 0.0;
        double v = 0;
        for (const Wave& w : waves_) v += w.weight * std::sin(6.283185307179586 * w.rate * t + w.phase);
        return share_ * std::sin(3.141592653589793 * t) * v;
    }
    // Its steepest slope: what it can add to the curve's.
    double peak() const { return share_ * (3.141592653589793 + 6.283185307179586 * 4.0); }

private:
    struct Wave { double rate = 1, phase = 0, weight = 0; };
    Wave waves_[3];
    double share_ = 0.0;
};

// One turn under way: `counts` over its length, along the curve, bent by the
// jitter. Each step sends the rounded share due by then minus what was
// already sent, so the steps add up to `counts` exactly (E1, E3). A step that
// came late sends no more than four ticks' worth at the peak rate, and the
// rest follows.
class Motion {
public:
    struct Orders {
        double degPerSec = 360.0;
        Curve curve = Curve::Smooth;
        double jitterPercent = 0.0;
        uint32_t seed = 1;
    };

    void start(int64_t counts, int64_t milli, const Orders& o, Ns now) {
        total_ = counts;
        sent_ = 0;
        t0_ = now;
        const double secs = std::min(std::fabs((double)milli / kMilli) / speedIn(o.degPerSec), kLongestTurn);
        len_ = std::max<Ns>(kTick, (Ns)(secs * kSecond));
        curve_ = o.curve;
        wobble_ = Wobble(o.seed, jitterIn(o.jitterPercent) / 100.0);
        share_ = 0.0;
        const double perTick = std::fabs((double)counts) * (double)kTick / (double)len_;
        cap_ = std::max<int64_t>(1, (int64_t)std::ceil(4.0 * perTick * (curvePeak(curve_) + wobble_.peak())));
        done_ = counts == 0;
    }
    bool done() const { return done_; }
    int64_t total() const { return total_; }
    int64_t sent() const { return sent_; }
    Ns length() const { return len_; }

    // The counts to send now.
    int64_t step(Ns now) {
        if (done_) return 0;
        const double t = (double)(now - t0_) / (double)len_;
        int64_t due = total_;
        if (t < 1.0) {
            double share = curveAt(curve_, t) + wobble_.at(t);
            // Never backwards, never past the end before the end.
            share = std::clamp(std::max(share, share_), 0.0, 1.0);
            share_ = share;
            due = std::llround(share * (double)total_);
        }
        const int64_t n = std::clamp<int64_t>(due - sent_, -cap_, cap_);
        sent_ += n;
        if (t >= 1.0 && sent_ == total_) done_ = true;
        return n;
    }
    // Cut short: what was sent so far is all it sends.
    int64_t stop() { done_ = true; return sent_; }

private:
    int64_t total_ = 0, sent_ = 0, cap_ = 1;
    Ns t0_ = 0, len_ = kTick;
    double share_ = 0;
    Curve curve_ = Curve::Smooth;
    Wobble wobble_;
    bool done_ = true;
};

// ── the boomerang (SPEC 6) ──────────────────────────────────────────────────

enum class Finish { Forward, Shortest };

// Spins while its bind is held, then finishes on a whole multiple of C from
// where it started (E4). The hand's movement is added to the net the same as
// the tool's own, so the finish allows for it.
class Spin {
public:
    struct Orders {
        int dir = 1;              // +1 right, -1 left
        double countsPerSec = 0;  // spin speed
        Ns ramp = 150'000'000;    // from still to spin speed
        Finish finish = Finish::Forward;
    };
    enum class Phase { Spinning, Finishing, Home, GaveUp };

    void begin(int64_t c, const Orders& o, Ns now) {
        c_ = std::max<int64_t>(c, kLeastCalibration);
        o_ = o;
        o_.dir = o.dir >= 0 ? 1 : -1;
        o_.countsPerSec = std::isfinite(o.countsPerSec) ? std::max(o.countsPerSec, 1.0) : 1.0;
        t0_ = last_ = now;
        net_ = 0;
        carry_ = 0;
        v_ = 0;
        phase_ = Phase::Spinning;
    }
    Phase phase() const { return phase_; }
    bool done() const { return phase_ == Phase::Home || phase_ == Phase::GaveUp; }
    int64_t net() const { return net_; }
    int64_t goal() const { return goal_; }

    // Counts the hand moved, in the virtual mouse's counts. After the spin is over they are the user's own.
    void hand(int64_t dx) { if (!done()) net_ += dx; }

    // The bind let go: the goal is fixed from the net as it stands. Nothing is
    // sent here; the next step starts the finish.
    void release(Ns now) {
        if (phase_ != Phase::Spinning) return;
        const int64_t ahead = net_ * o_.dir;
        const int64_t turns = o_.finish == Finish::Forward ? -floorDiv(-ahead, c_)
                                                           : (int64_t)roundDiv(ahead, c_);
        goal_ = turns * c_ * o_.dir;
        left0_ = (goal_ - net_) * o_.dir;
        // Slowing evenly to a stop on the goal takes twice the distance over
        // the speed it starts from, so a slow spin starts its finish faster:
        // it comes home within kFinishWithin, never later.
        const double r = std::fabs((double)left0_);
        top_ = std::max({v_, floorSpeed(), r / (0.5 * kFinishWithin)});
        decel_ = r > 0 ? top_ * top_ / (2.0 * r) : 0.0;
        carry_ = 0;
        fin0_ = now;
        phase_ = goal_ == net_ ? Phase::Home : Phase::Finishing;
    }

    // The counts to send now.
    int64_t step(Ns now) {
        // A stalled tick is taken as a short one, so it cannot send a burst.
        const double dt = (double)std::clamp<Ns>(now - last_, 0, 20 * kTick) / kSecond;
        last_ = now;
        if (done()) return 0;
        int64_t n = 0;
        if (phase_ == Phase::Spinning) {
            const double up = o_.ramp > 0 ? std::min(1.0, (double)(now - t0_) / (double)o_.ramp) : 1.0;
            v_ = o_.countsPerSec * up;
            carry_ += v_ * dt;
            n = (int64_t)carry_;
            carry_ -= (double)n;
            n *= o_.dir;
        } else {
            if (now - fin0_ > (Ns)(kLongestFinish * kSecond)) { phase_ = Phase::GaveUp; return 0; }
            // The hand carried the view half a turn past the goal, or pulled it
            // half a turn further back than it had left: the nearer whole turn
            // becomes the goal.
            int64_t left = (goal_ - net_) * o_.dir;
            if (left < -c_ / 2) goal_ += c_ * o_.dir;
            else if (left > left0_ + c_ / 2) goal_ -= c_ * o_.dir;
            left = goal_ - net_;
            if (left == 0) { phase_ = Phase::Home; return 0; }
            const double r = std::fabs((double)left);
            const double v = std::clamp(std::sqrt(2.0 * decel_ * r), floorSpeed(), top_);
            v_ = v;
            carry_ += v * dt;
            int64_t m = (int64_t)carry_;
            carry_ -= (double)m;
            m = std::min<int64_t>(m, std::llabs(left));
            n = left > 0 ? m : -m;
            if (n == left) phase_ = Phase::Home;
        }
        net_ += n;
        return n;
    }

    // Cut short: it stops where it is.
    void stop() { if (!done()) phase_ = Phase::GaveUp; }

private:
    // The finish never crawls, and never outruns the spin itself.
    double floorSpeed() const { return std::min(o_.countsPerSec, std::max(0.05 * o_.countsPerSec, 200.0)); }

    int64_t c_ = kLeastCalibration, net_ = 0, goal_ = 0, left0_ = 0;
    Orders o_;
    Ns t0_ = 0, last_ = 0, fin0_ = 0;
    double carry_ = 0, v_ = 0, decel_ = 0, top_ = 0;
    Phase phase_ = Phase::Home;
};

}  // namespace spinjitzu
