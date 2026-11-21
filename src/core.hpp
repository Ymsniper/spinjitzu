// What the app does, with no window and no device in it: keys, buttons and
// the hand's movement go in, timestamped; counts for the virtual mouse and
// what to show come out. One thread owns it (SPEC 7b): the worker that reads
// the devices. The window only hands it settings and reads what it says.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "settings.hpp"

namespace spinjitzu {

// What can be bound, as the window numbers it: the three fixed keys, the
// boomerang, the arm key, then each turn.
enum Slot { kSlotCalibrate, kSlotTest, kSlotPanic, kSlotBoomerang, kSlotArm, kSlotTurn0 };

class Core {
public:
    struct Last {                 // the last turn, for the footer
        bool any = false, stopped = false;
        int64_t milli = 0, counts = 0, sent = 0;
    };
    struct Out {                  // what the window shows
        std::string doing = "idle";
        int64_t calNet = 0;       // counts so far in a hand 360 or a correction
        bool calibrating = false, correcting = false;
        int captureSlot = -1;     // a capture that ended: its slot, and the code (0: cancelled)
        int captured = 0;
        bool newC = false;        // the calibration changed: save these
        bool armedNew = false;    // Armed changed: show and save `armed`
        bool armed = false;
        int speedCheck = 0;       // 0: not done, 1: the fast 360 landed, 2: it did not
        int64_t speedOff = 0;     // by how many counts it did not
        int64_t cHand = 0, cVirtual = 0;
        bool checked = false;
        Last last;
        std::vector<std::string> log;
    };

    // The settings to work by. A turn under way keeps the orders it began with.
    // Armed is not taken from them: settings the window sent before it heard
    // of an arm key press would put the old state back. It comes by arm().
    void use(const Settings& s) {
        const bool newLedger = s.cVirtual != s_.cVirtual;
        s_ = s;
        if (newLedger) ledger_ = Ledger(std::max<int64_t>(s_.cVirtual, kLeastCalibration));
    }

    // Armed, as the window set it; told back, so both ends agree.
    void arm(bool on) {
        armed_ = on;
        out_.armedNew = true;
        out_.armed = on;
    }

    // The next key or button pressed binds `slot`. Keys held now count only
    // once let go: the click that asked for it binds nothing.
    void capture(int slot, Ns now) {
        capturing_ = slot;
        captureFrom_ = now;
        say("press a key or a mouse button (Esc cancels)");
    }
    bool capturing() const { return capturing_ >= 0; }

    void key(int code, int value, Ns t) {
        if (code <= 0 || code >= 768 || value == 2) return;   // repeats are not presses
        const bool was = down_[code];
        down_[code] = value != 0;
        if (value == 0) {
            if (phase_ == Phase::Spinning && code == spinKey_) spin_.release(t);
            return;
        }
        if (was) return;
        if (capturing_ >= 0) {
            if (t < captureFrom_) return;
            // Left click fires in every game, and it is how the window is clicked.
            if (code == BTN_LEFT) { say("left click cannot be bound: press another"); return; }
            out_.captureSlot = capturing_;
            out_.captured = code == KEY_ESC ? 0 : code;
            capturing_ = -1;
            say(code == KEY_ESC ? "binding cancelled" : "pressed " + keyName(code));
            return;
        }
        if (code == s_.keyPanic) { panic(); return; }
        if (code == s_.keyArm) {
            armed_ = !armed_;
            out_.armedNew = true;
            out_.armed = armed_;
            say(armed_ ? "armed (" + keyName(code) + ")" : "not armed (" + keyName(code) + ")");
            return;
        }
        if (code == s_.keyCalibrate) { calibrateKey(); return; }
        if (code == s_.keyTest) { testKey(t); return; }
        const bool bound = code == s_.boom.key || std::any_of(s_.turns.begin(), s_.turns.end(), [&](const TurnBind& tb) { return tb.key == code; });
        if (!bound) return;
        if (!armed_) { say(keyName(code) + " pressed, but not armed: click the seal at the top right"); return; }
        if (s_.cVirtual < kLeastCalibration) { say(keyName(code) + " pressed, but not calibrated yet"); return; }
        if (phase_ != Phase::Idle) return;
        if (code == s_.boom.key) { startSpin(code, t); return; }
        for (const TurnBind& tb : s_.turns)
            if (tb.key == code) { startTurn(tb, t); return; }
    }

    // What the system says is held, after it dropped events.
    void keyState(int code, bool held) { if (code > 0 && code < 768) down_[code] = held; }

    // The hand's horizontal counts. They always reach the game, so a
    // spin always counts them.
    void hand(int64_t dx, Ns) {
        if (phase_ == Phase::Recording) { cal_.add(dx); out_.calNet = cal_.net(); }
        else if (phase_ == Phase::Testing || phase_ == Phase::Correcting) { correction_ += dx; out_.calNet = correction_; }
        else if (phase_ == Phase::Spinning) spin_.hand(scaler_(dx));
    }

    // A device with keys went away: a boomerang held from it can never be
    // let go, so it stops where it is.
    void keyDeviceGone() {
        if (phase_ == Phase::Spinning && !spin_.done()) { spin_.stop(); say("the boomerang's key went away: stopped where it was"); phase_ = Phase::Idle; }
    }

    // Counts to send now.
    int64_t tick(Ns now) {
        int64_t n = 0;
        if (phase_ == Phase::Testing) {
            n = motion_.step(now);
            if (motion_.done()) { phase_ = Phase::Correcting; say("put the crosshair back on the mark by hand, then " + keyName(s_.keyCalibrate)); }
        } else if (phase_ == Phase::Turning) {
            n = motion_.step(now);
            out_.last.sent = motion_.sent();
            if (motion_.done()) { ledger_.end(motion_.sent()); phase_ = Phase::Idle; }
        } else if (phase_ == Phase::Spinning) {
            n = spin_.step(now);
            if (spin_.done()) {
                say(spin_.phase() == Spin::Phase::Home ? "boomerang home" : "boomerang gave up: the hand kept pulling it away");
                phase_ = Phase::Idle;
            }
        }
        out_.doing = doing();
        return n;
    }

    // A write to the virtual mouse failed: what is under way stops.
    void sendFailed() { if (phase_ != Phase::Idle) { say("the virtual mouse did not take a write"); panic(); } }

    bool idle() const { return phase_ == Phase::Idle; }

    // What the window shows; the log and the one-off fields are handed over once.
    Out take() {
        out_.calibrating = phase_ == Phase::Recording;
        out_.correcting = phase_ == Phase::Correcting || phase_ == Phase::Testing;
        out_.doing = doing();
        Out o = out_;
        out_.log.clear();
        out_.captureSlot = -1;
        out_.newC = false;
        out_.armedNew = false;
        return o;
    }

private:
    enum class Phase { Idle, Recording, Testing, Correcting, Turning, Spinning };

    void say(const std::string& s) { out_.log.push_back(s); }

    std::string doing() const {
        switch (phase_) {
        case Phase::Recording: return "hand 360: turn back onto the mark, then " + keyName(s_.keyCalibrate);
        case Phase::Testing: return speedTest_ ? "speed check turning" : "test 360 turning";
        case Phase::Correcting: return "back on the mark by hand, then " + keyName(s_.keyCalibrate);
        case Phase::Turning: return "turning";
        case Phase::Spinning: return "boomerang";
        default: return capturing_ >= 0 ? "waiting for a key" : armed_ ? "armed" : "not armed";
        }
    }

    // Three times the spread of the hand 360s, or 0.3% of a turn while there
    // is only one: a landing nearer than this is on the mark.
    double tolerance() const {
        double t = std::max(0.003 * (double)s_.cHand, 2.0);
        if (hands_.size() >= 2) {
            double mean = 0, var = 0;
            for (int64_t x : hands_) mean += (double)x;
            mean /= (double)hands_.size();
            for (int64_t x : hands_) var += ((double)x - mean) * ((double)x - mean);
            t = std::max(t, 3.0 * std::sqrt(var / (double)(hands_.size() - 1)));
        }
        return t;
    }

    void calibrateKey() {
        if (phase_ == Phase::Idle) {
            cal_.start();
            out_.calNet = 0;
            phase_ = Phase::Recording;
            say("hand 360: turn one full circle back onto the mark, then " + keyName(s_.keyCalibrate));
        } else if (phase_ == Phase::Recording) {
            const int64_t net = cal_.net();
            const auto c = cal_.finish();
            phase_ = Phase::Idle;
            if (!c) { say("a hand turn of " + std::to_string(net) + " counts is too small to be a 360"); return; }
            hands_.push_back(*c);
            std::vector<int64_t> m = hands_;
            std::sort(m.begin(), m.end());
            s_.cHand = m[m.size() / 2];
            // A new hand figure: the virtual mouse starts from it and is
            // checked again.
            s_.cVirtual = s_.cHand;
            s_.checked = false;
            ledger_ = Ledger(s_.cVirtual);
            publishC();
            say("hand 360 #" + std::to_string(hands_.size()) + ": " + std::to_string(*c) + " counts; using " +
                std::to_string(s_.cHand) + ". Now " + keyName(s_.keyTest) + " to test it");
        } else if (phase_ == Phase::Correcting) {
            const int64_t k = correction_;
            phase_ = Phase::Idle;
            if (speedTest_) {
                // Only a report: a figure taken at one speed through
                // acceleration is wrong at every other, so C is left alone.
                speedTest_ = false;
                const bool landed = std::fabs((double)k) <= tolerance();
                out_.speedCheck = landed ? 1 : 2;
                out_.speedOff = k;
                say(landed ? "speed check: the fast 360 landed, so the game does not see acceleration on Spinjitzu's mouse"
                           : "speed check: the fast 360 was " + std::to_string(k) +
                                 " counts off, so the game sees acceleration on Spinjitzu's mouse: see Output");
                return;
            }
            const auto c = corrected(s_.cVirtual, s_.cHand, k);
            if (!c) { say("that correction is not one a turn could need: do the hand 360 again"); return; }
            s_.checked = std::fabs((double)k) <= tolerance();
            s_.cVirtual = *c;
            ledger_ = Ledger(s_.cVirtual);
            publishC();
            say("test 360: corrected " + std::to_string(k) + " counts by hand; virtual " + std::to_string(s_.cVirtual) +
                (s_.checked ? ", on the mark" : ", " + keyName(s_.keyTest) + " again to check it"));
        }
    }

    void testKey(Ns t) {
        if (phase_ != Phase::Idle) return;
        if (s_.cHand < kLeastCalibration) { say("do a hand 360 first (" + keyName(s_.keyCalibrate) + ")"); return; }
        // Once a test has landed, the next one checks for acceleration: the
        // same 360 four times as fast lands on the mark only if speed does
        // not change how far the game turns.
        speedTest_ = s_.checked;
        Motion::Orders o;
        o.degPerSec = speedTest_ ? 4 * 360.0 : 360.0;
        motion_.start(s_.cVirtual, kFullTurnMilli, o, t);
        correction_ = 0;
        out_.calNet = 0;
        phase_ = Phase::Testing;
        say((speedTest_ ? "speed check: a 360 at four times the speed, " : "test 360: ") + std::to_string(s_.cVirtual) + " counts");
    }

    void startTurn(const TurnBind& tb, Ns t) {
        const auto m = millidegrees(tb.degrees);
        if (!m || *m == 0) return;
        const int64_t milli = *m * (tb.dir < 0 ? -1 : 1);
        const int64_t n = ledger_.begin(milli);
        Motion::Orders o;
        o.degPerSec = tb.own ? tb.speed : s_.speed;
        o.curve = tb.own ? tb.curve : s_.curve;
        o.jitterPercent = tb.own ? tb.jitter : s_.jitter;
        o.seed = ++seed_;
        motion_.start(n, milli, o, t);
        out_.last = {true, false, milli, n, 0};
        phase_ = Phase::Turning;
    }

    void startSpin(int code, Ns t) {
        Spin::Orders o;
        o.dir = s_.boom.dir;
        o.countsPerSec = speedIn(s_.boom.speed) * (double)s_.cVirtual / 360.0;
        o.finish = s_.boom.finish;
        spin_.begin(s_.cVirtual, o, t);
        scaler_ = Scaler(s_.cVirtual, std::max<int64_t>(s_.cHand, kLeastCalibration));
        spinKey_ = code;
        phase_ = Phase::Spinning;
    }

    void panic() {
        if (phase_ == Phase::Turning) {
            ledger_.end(motion_.stop());
            out_.last.sent = motion_.sent();
            out_.last.stopped = true;
        } else if (phase_ == Phase::Spinning) {
            spin_.stop();
        } else if (phase_ == Phase::Recording) {
            cal_.cancel();
        }
        speedTest_ = false;
        capturing_ = -1;
        if (phase_ != Phase::Idle) say("stopped");
        phase_ = Phase::Idle;
    }

    void publishC() {
        out_.newC = true;
        out_.cHand = s_.cHand;
        out_.cVirtual = s_.cVirtual;
        out_.checked = s_.checked;
    }

    Settings s_;
    Ledger ledger_{kLeastCalibration};
    Calibrator cal_;
    Motion motion_;
    Spin spin_;
    Scaler scaler_{1, 1};
    Phase phase_ = Phase::Idle;
    std::vector<int64_t> hands_;
    int64_t correction_ = 0;
    bool speedTest_ = false;      // the test under way is the speed check
    int capturing_ = -1, spinKey_ = 0;
    Ns captureFrom_ = 0;
    bool armed_ = false;
    bool down_[768] = {};
    uint32_t seed_ = 0;
    Out out_;
};

}  // namespace spinjitzu
