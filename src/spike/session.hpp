// The calibration check, with no device in it. It answers the question the
// whole tool rests on (SPEC 2): does a count sent through our virtual mouse
// turn the game as far as a count from the hand? Keys, hand movement and
// ticks go in, timestamped; counts to send and lines for the log come out.
//
//   F8   a full turn by hand: press, turn back onto the mark, press. Again
//        for a better figure: the median of all of them is used.
//        After a turn by the tool: the crosshair is back on the mark.
//   F9   the tool turns one full turn twice, slowly then fast, and after
//        each the hand puts the crosshair back on the mark. Landing apart
//        means acceleration is in the virtual mouse's path.
//   F10  five full turns in a row, at the corrected figure: any scaling left
//        shows five times over, the hand's own error only once.
//   F7   (in main) switches both mice to the flat acceleration profile and
//        back: the control that tells the game's raw input from acceleration.
//   F12  quits. Esc is left to the game.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include "../engine.hpp"

namespace spinjitzu::spike {

enum Key { kF8 = 66, kF9 = 67, kF10 = 68, kF12 = 88 };   // linux/input-event-codes.h

class Session {
public:
    // Seconds the slow and the fast turn take, and the speed of the five.
    static constexpr double kSlowSec = 3.0, kFastSec = 0.3, kFiveDegPerSec = 720.0;

    // A key the system already reports held when the tool starts: its press
    // was not ours to see, so it acts only after a release.
    void heldAtStart(int code) { if (code >= 0 && code < 768) down_[code] = true; }

    void key(int code, int value, Ns now) {
        if (code < 0 || code >= 768 || value == 2) return;   // repeats are not presses
        const bool was = down_[code];
        down_[code] = value != 0;
        if (value != 1 || was) return;
        press(code, now);
    }

    // What the system reports held, after it dropped events: a press or a
    // release lost there must not swallow the next press.
    void keyState(int code, bool held) { if (code >= 0 && code < 768) down_[code] = held; }

    // The system dropped events, or a send failed: the step under way has
    // lost counts, so it gives no figure.
    void spoiled(const char* why) {
        if (phase_ == Phase::Recording) { cal_.cancel(); phase_ = Phase::Idle; say("%s: hand 360 dropped, do it again (F8)", why); return; }
        if (phase_ == Phase::Sending || phase_ == Phase::Correcting) {
            void_ = true;
            say("%s: this turn will give no figure; finish it with F8 and do F9 again", why);
        }
    }

    // The hand's horizontal counts, from the mouse being read.
    void hand(int64_t dx, Ns now) {
        if (cal_.active() || phase_ == Phase::Sending || phase_ == Phase::Correcting) {
            if (!moved_) { first_ = now; moved_ = true; }
            last_ = now;
            fastest_ = std::max<int64_t>(fastest_, std::llabs(dx));
        }
        if (cal_.active()) cal_.add(dx);
        if (phase_ == Phase::Sending) { duringSend_ += dx; correction_ += dx; }
        else if (phase_ == Phase::Correcting) correction_ += dx;
    }

    // Counts to send through the virtual mouse now.
    int64_t tick(Ns now) {
        if (phase_ != Phase::Sending) return 0;
        const int64_t n = motion_.step(now);
        sentThisTurn_ += n;
        if (motion_.done()) {
            phase_ = Phase::Correcting;
            say("sent %lld counts. Put the crosshair back on the mark by hand, then F8.", (long long)sentThisTurn_);
        }
        return n;
    }

    bool wantsTicks() const { return phase_ == Phase::Sending; }
    bool quit() const { return quit_; }

    // Lines for the terminal and the log, oldest first.
    std::vector<std::string> take() { std::vector<std::string> out(lines_.begin(), lines_.end()); lines_.clear(); return out; }

    // Figures, for the checks.
    int64_t c() const { return c_; }
    int64_t cVirtual() const { return cVirtual_; }
    bool speedMatters() const { return speedMatters_; }
    bool fiveDone() const { return fiveDone_; }
    int64_t fiveOff() const { return fiveOff_; }
    int64_t slowOff() const { return kSlow_; }

private:
    enum class Phase { Idle, Recording, Sending, Correcting };
    enum class Step { None, Slow, Fast, Five };

    template <class... A>
    void say(const char* fmt, A... a) {
        char b[512];
        snprintf(b, sizeof b, fmt, a...);
        lines_.push_back(b);
    }
    void say(const char* s) { lines_.push_back(s); }

    // How far apart two landings may be and still be the same landing: three
    // times the spread of the hand's own 360s, or 0.3% of a turn while there
    // is only one to go by.
    double tolerance() const {
        double t = 0.003 * (double)c_;
        if (cs_.size() >= 2) {
            double mean = 0, var = 0;
            for (int64_t x : cs_) mean += (double)x;
            mean /= (double)cs_.size();
            for (int64_t x : cs_) var += ((double)x - mean) * ((double)x - mean);
            t = std::max(t, 3.0 * std::sqrt(var / (double)(cs_.size() - 1)));
        }
        return std::max(t, 2.0);
    }

    // One full turn over `seconds`, or for the five, five turns at their speed.
    void startSend(int64_t counts, double seconds, Step step, Ns now) {
        Motion::Orders o;
        o.curve = Curve::Smooth;
        const bool five = step == Step::Five;
        o.degPerSec = five ? kFiveDegPerSec : 360.0 / seconds;
        motion_.start(counts, five ? 5 * kFullTurnMilli : kFullTurnMilli, o, now);
        step_ = step;
        phase_ = Phase::Sending;
        sentThisTurn_ = 0;
        correction_ = 0;
        duringSend_ = 0;
        void_ = false;
        resetPace();
    }

    // How long the hand took and its fastest report, for telling a spread in
    // the figures from the speed the hand moved at.
    void resetPace() { moved_ = false; first_ = last_ = 0; fastest_ = 0; }
    std::string pace() const {
        char b[96];
        if (!moved_) return "the hand did not move";
        snprintf(b, sizeof b, "over %.2f s, fastest %lld counts in one report", (double)(last_ - first_) / kSecond, (long long)fastest_);
        return b;
    }

    void press(int code, Ns now) {
        if (code == kF12) { quit_ = true; say("quitting"); return; }
        if (phase_ == Phase::Sending) { say("a key while the tool turns is ignored"); return; }

        if (code == kF8) {
            if (phase_ == Phase::Correcting) { corrected(now); return; }
            if (phase_ == Phase::Recording) {
                const int64_t net = cal_.net();
                const auto c = cal_.finish();
                phase_ = Phase::Idle;
                if (!c) { say("a hand turn of %lld counts is too small to be a 360: refused", (long long)net); return; }
                const std::string how = pace();
                cs_.push_back(*c);
                std::vector<int64_t> s = cs_;
                std::sort(s.begin(), s.end());
                c_ = s[s.size() / 2];
                say("hand 360 #%zu: %lld counts (net %+lld), %s. Using the median, C = %lld, tolerance %.1f counts.",
                    cs_.size(), (long long)*c, (long long)net, how.c_str(), (long long)c_, tolerance());
                return;
            }
            cal_.start();
            resetPace();
            phase_ = Phase::Recording;
            say("recording: turn one full circle by hand back onto the mark, then F8");
            return;
        }
        if (phase_ == Phase::Recording) { say("finish the hand turn with F8 first"); return; }
        if (code == kF9) {
            if (!c_) { say("F9 needs a hand 360 first (F8)"); return; }
            say("slow turn: %lld counts over %.1f s", (long long)c_, kSlowSec);
            startSend(c_, kSlowSec, Step::Slow, now);
            return;
        }
        if (code == kF10) {
            if (!cVirtual_) { say("F10 needs F9 done first"); return; }
            if (speedMatters_) { say("F10 means nothing while acceleration is in the path: F7 for flat, then F9 again"); return; }
            Ledger l(cVirtual_);
            const int64_t n = l.begin(5 * kFullTurnMilli);
            say("five turns: %lld counts at C = %lld", (long long)n, (long long)cVirtual_);
            startSend(n, 0, Step::Five, now);
            return;
        }
    }

    // The crosshair is back on the mark: what the hand moved since the tool
    // started turning is how far the tool fell short (positive) or overshot.
    void corrected(Ns now) {
        (void)now;
        phase_ = Phase::Idle;
        const int64_t k = correction_;
        if (void_) { say("that turn lost counts and gives no figure: F9 again"); return; }
        say("correction %s", pace().c_str());
        if (duringSend_ != 0)
            say("note: the hand moved %+lld counts while the tool was turning; counted in the correction", (long long)duringSend_);
        if (step_ == Step::Slow) {
            kSlow_ = k;
            say("slow turn: the hand corrected %+lld counts", (long long)k);
            say("fast turn: %lld counts over %.1f s", (long long)c_, kFastSec);
            startSend(c_, kFastSec, Step::Fast, now);
            return;
        }
        if (step_ == Step::Fast) {
            kFast_ = k;
            const double tol = tolerance();
            say("fast turn: the hand corrected %+lld counts", (long long)k);
            speedMatters_ = std::fabs((double)(kSlow_ - kFast_)) > tol;
            if (speedMatters_)
                say("RESULT speed: the slow and the fast turn landed %lld counts apart (tolerance %.1f). "
                    "Acceleration is in the path, on the desktop or in the game.", (long long)std::llabs(kSlow_ - kFast_), tol);
            else
                say("RESULT speed: the slow and the fast turn landed the same (%lld apart, tolerance %.1f). "
                    "Acceleration does not touch the virtual mouse.", (long long)std::llabs(kSlow_ - kFast_), tol);
            const int64_t kMean = (int64_t)std::llround(0.5 * (double)(kSlow_ + kFast_));
            const auto cv = spinjitzu::corrected(c_, c_, kMean);
            if (!cv) { say("RESULT scale: those corrections are not ones a turn could need; do the hand 360 again"); return; }
            cVirtual_ = *cv;
            const bool same = std::fabs((double)kMean) <= tol;
            say("RESULT scale: C for the virtual mouse = %lld against %lld by hand (ratio %.5f). %s", (long long)cVirtual_,
                (long long)c_, (double)cVirtual_ / (double)c_,
                same ? "It turns the game as the hand does." : "It turns the game by a different amount.");
            say("F10 checks it with five turns in a row.");
            return;
        }
        if (step_ == Step::Five) {
            fiveDone_ = true;
            fiveOff_ = k;
            const double tol = tolerance();
            say("five turns: the hand corrected %+lld counts, %.1f a turn", (long long)k, (double)k / 5.0);
            say("RESULT five: %s", std::fabs((double)k) <= tol
                ? "landed on the mark. The corrected figure holds."
                : "off by more than one landing's tolerance. Something scales the virtual mouse unevenly.");
        }
    }

    Phase phase_ = Phase::Idle;
    Step step_ = Step::None;
    Calibrator cal_;
    Motion motion_;
    std::vector<int64_t> cs_;
    int64_t c_ = 0, cVirtual_ = 0, kSlow_ = 0, kFast_ = 0, fiveOff_ = 0;
    int64_t sentThisTurn_ = 0, correction_ = 0, duringSend_ = 0;
    bool speedMatters_ = false, fiveDone_ = false, quit_ = false, void_ = false, moved_ = false;
    Ns first_ = 0, last_ = 0;
    int64_t fastest_ = 0;
    bool down_[768] = {};
    std::deque<std::string> lines_;
};

}  // namespace spinjitzu::spike
