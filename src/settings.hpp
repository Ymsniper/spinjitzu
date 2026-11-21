// What the user set, kept in ~/.config/spinjitzu/spinjitzu.cfg as
// `key = value` lines. Numbers are written and read with to_chars and
// from_chars, so a locale with comma decimals reads the file the same; keys
// are kept by the kernel's names (KEY_F8, BTN_SIDE). Every value read is
// held to what the menu allows, and a line that does not parse is skipped.
#pragma once

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "devices.hpp"

namespace spinjitzu {

struct TurnBind {
    int key = 0;
    double degrees = 180;
    int dir = 1;                  // +1 right, -1 left
    bool own = false;             // its own speed, curve and jitter, not the defaults
    double speed = 720;
    Curve curve = Curve::Smooth;
    double jitter = 0;            // percent; 0 is no jitter
};

struct BoomBind {
    int key = 0;
    int dir = 1;
    double speed = 1080;          // degrees a second
    bool letThrough = true;
    Finish finish = Finish::Forward;
};

struct Settings {
    int keyCalibrate = KEY_F8, keyTest = KEY_F9, keyPanic = KEY_PAUSE;
    int keyArm = 0;               // switches Armed from inside the game; none at first
    int64_t cHand = 0, cVirtual = 0;
    bool checked = false;         // a test 360 through the virtual mouse landed on the mark
    bool armed = false;           // turn and boomerang keys work
    double speed = 720;
    Curve curve = Curve::Smooth;
    double jitter = 0;
    std::vector<TurnBind> turns;
    BoomBind boom;
    std::string mouse;            // the mouse read as the hand, by name; empty: the first that moves
};

inline std::string num(double v) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v);
    return std::string(b, r.ptr);
}
inline bool numIn(const std::string& s, double& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(v);
}
inline bool numIn(const std::string& s, int64_t& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

inline std::string toText(const Settings& s) {
    auto curve = [](Curve c) { return c == Curve::Smooth ? "smooth" : "constant"; };
    std::string o = "# spinjitzu settings\n";
    o += "calibrate = " + codeName(s.keyCalibrate) + "\n";
    o += "test = " + codeName(s.keyTest) + "\n";
    o += "panic = " + codeName(s.keyPanic) + "\n";
    o += "arm = " + codeName(s.keyArm) + "\n";
    o += "c_hand = " + std::to_string(s.cHand) + "\n";
    o += "c_virtual = " + std::to_string(s.cVirtual) + "\n";
    o += std::string("checked = ") + (s.checked ? "1" : "0") + "\n";
    o += std::string("armed = ") + (s.armed ? "1" : "0") + "\n";
    o += "speed = " + num(s.speed) + "\n";
    o += std::string("curve = ") + curve(s.curve) + "\n";
    o += "jitter = " + num(s.jitter) + "\n";
    o += "mouse = " + s.mouse + "\n";
    o += "boomerang = " + codeName(s.boom.key) + " " + std::to_string(s.boom.dir) + " " + num(s.boom.speed) + " " +
         (s.boom.letThrough ? "through" : "block") + " " + (s.boom.finish == Finish::Forward ? "forward" : "shortest") + "\n";
    for (const TurnBind& t : s.turns)
        o += "turn = " + codeName(t.key) + " " + num(t.degrees) + " " + std::to_string(t.dir) + " " + (t.own ? "own" : "default") +
             " " + num(t.speed) + " " + curve(t.curve) + " " + num(t.jitter) + "\n";
    return o;
}

inline std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') j++;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}

// Reads what it can; what it cannot keeps its default.
inline Settings fromText(const std::string& text) {
    Settings s;
    auto c = [](const std::string& v, int64_t& out) {
        int64_t x;
        if (numIn(v, x) && (x == 0 || (x >= kLeastCalibration && x < (int64_t)1e9))) out = x;
    };
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find(" = ");
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 3);
        const std::vector<std::string> w = words(v);
        double d;
        if (k == "calibrate") { if (int x = codeOf(v)) s.keyCalibrate = x; }
        else if (k == "test") { if (int x = codeOf(v)) s.keyTest = x; }
        else if (k == "panic") { if (int x = codeOf(v)) s.keyPanic = x; }
        else if (k == "arm") s.keyArm = codeOf(v);
        else if (k == "c_hand") c(v, s.cHand);
        else if (k == "c_virtual") c(v, s.cVirtual);
        else if (k == "checked") s.checked = v == "1";
        else if (k == "armed") s.armed = v == "1";
        else if (k == "speed") { if (numIn(v, d)) s.speed = speedIn(d); }
        else if (k == "curve") s.curve = v == "constant" ? Curve::Constant : Curve::Smooth;
        else if (k == "jitter") { if (numIn(v, d)) s.jitter = jitterIn(d); }
        else if (k == "mouse") s.mouse = v;
        else if (k == "boomerang" && w.size() == 5) {
            s.boom.key = codeOf(w[0]);
            s.boom.dir = w[1] == "-1" ? -1 : 1;
            if (numIn(w[2], d)) s.boom.speed = speedIn(d);
            s.boom.letThrough = w[3] != "block";
            s.boom.finish = w[4] == "shortest" ? Finish::Shortest : Finish::Forward;
        } else if (k == "turn" && w.size() == 7) {
            TurnBind t;
            t.key = codeOf(w[0]);
            if (!numIn(w[1], d) || !millidegrees(d)) continue;
            t.degrees = d;
            t.dir = w[2] == "-1" ? -1 : 1;
            t.own = w[3] == "own";
            if (numIn(w[4], d)) t.speed = speedIn(d);
            t.curve = w[5] == "constant" ? Curve::Constant : Curve::Smooth;
            if (numIn(w[6], d)) t.jitter = jitterIn(d);
            s.turns.push_back(t);
        }
    }
    return s;
}

inline std::string settingsPath() {
    const char* x = getenv("XDG_CONFIG_HOME");
    const char* h = getenv("HOME");
    const std::string dir = (x && *x) ? std::string(x) + "/spinjitzu" : std::string(h ? h : ".") + "/.config/spinjitzu";
    mkdir(dir.c_str(), 0755);
    return dir + "/spinjitzu.cfg";
}

inline Settings loadSettings(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return Settings{};
    std::string text;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    return fromText(text);
}

// Written beside the file and renamed over it, so a crash mid-write leaves
// the last good settings, never half of them.
inline bool saveSettings(const std::string& path, const Settings& s) {
    const std::string tmp = path + ".new", text = toText(s);
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace spinjitzu
