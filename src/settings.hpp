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

// What belongs to one game, kept per profile: each game turns by its own
// counts. A struct of its own, so a field added later is plainly one or the
// other and a profile switch moves all of it at once.
struct Game {
    int64_t cHand = 0, cVirtual = 0;
    bool checked = false;         // a test 360 through the virtual mouse landed on the mark
    double speed = 720;
    Curve curve = Curve::Smooth;
    double jitter = 0;
    std::vector<TurnBind> turns;
    BoomBind boom;
};

struct Settings {
    int keyCalibrate = KEY_F8, keyTest = KEY_F9, keyPanic = KEY_PAUSE;
    int keyArm = 0;               // switches Armed from inside the game; none at first
    bool armed = false;           // turn and boomerang keys work
    std::string mouse;            // the mouse read as the hand, by name; empty: the first that moves
    Game game;                    // the profile in use
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

inline std::string globalText(const Settings& s) {
    std::string o;
    o += "calibrate = " + codeName(s.keyCalibrate) + "\n";
    o += "test = " + codeName(s.keyTest) + "\n";
    o += "panic = " + codeName(s.keyPanic) + "\n";
    o += "arm = " + codeName(s.keyArm) + "\n";
    o += std::string("armed = ") + (s.armed ? "1" : "0") + "\n";
    o += "mouse = " + s.mouse + "\n";
    return o;
}

inline std::string gameText(const Game& g) {
    auto curve = [](Curve c) { return c == Curve::Smooth ? "smooth" : "constant"; };
    std::string o;
    o += "c_hand = " + std::to_string(g.cHand) + "\n";
    o += "c_virtual = " + std::to_string(g.cVirtual) + "\n";
    o += std::string("checked = ") + (g.checked ? "1" : "0") + "\n";
    o += "speed = " + num(g.speed) + "\n";
    o += std::string("curve = ") + curve(g.curve) + "\n";
    o += "jitter = " + num(g.jitter) + "\n";
    o += "boomerang = " + codeName(g.boom.key) + " " + std::to_string(g.boom.dir) + " " + num(g.boom.speed) + " " +
         (g.boom.letThrough ? "through" : "block") + " " + (g.boom.finish == Finish::Forward ? "forward" : "shortest") + "\n";
    for (const TurnBind& t : g.turns)
        o += "turn = " + codeName(t.key) + " " + num(t.degrees) + " " + std::to_string(t.dir) + " " + (t.own ? "own" : "default") +
             " " + num(t.speed) + " " + curve(t.curve) + " " + num(t.jitter) + "\n";
    return o;
}

inline std::string toText(const Settings& s) { return "# spinjitzu settings\n" + globalText(s) + gameText(s.game); }

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
        else if (k == "c_hand") c(v, s.game.cHand);
        else if (k == "c_virtual") c(v, s.game.cVirtual);
        else if (k == "checked") s.game.checked = v == "1";
        else if (k == "armed") s.armed = v == "1";
        else if (k == "speed") { if (numIn(v, d)) s.game.speed = speedIn(d); }
        else if (k == "curve") s.game.curve = v == "constant" ? Curve::Constant : Curve::Smooth;
        else if (k == "jitter") { if (numIn(v, d)) s.game.jitter = jitterIn(d); }
        else if (k == "mouse") s.mouse = v;
        else if (k == "boomerang" && w.size() == 5) {
            s.game.boom.key = codeOf(w[0]);
            s.game.boom.dir = w[1] == "-1" ? -1 : 1;
            if (numIn(w[2], d)) s.game.boom.speed = speedIn(d);
            s.game.boom.letThrough = w[3] != "block";
            s.game.boom.finish = w[4] == "shortest" ? Finish::Shortest : Finish::Forward;
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
            s.game.turns.push_back(t);
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

// ── profiles ──────────────────────────────────────────────────────────────────
// One game each, by a name the user gives. All in the one file: the global
// keys, `profile = <the one in use>`, then a `[name]` section per profile with
// the same lines as before. No file is named after what the user typed.

struct Library {
    Settings s;                       // what the tool works by: the globals and the profile in use
    std::vector<std::string> names;   // every profile, in the order they were made
    std::vector<Game> games;          // each one's game; the one in use lives in s.game until it is left
    size_t current = 0;
    bool migrated = false;            // read from a file written before profiles
};

constexpr size_t kLongestName = 40;   // bytes

inline std::string trimmed(const std::string& n) {
    const size_t a = n.find_first_not_of(" \t"), b = n.find_last_not_of(" \t");
    return a == std::string::npos ? "" : n.substr(a, b - a + 1);
}
inline std::string folded(std::string n) {   // ASCII letters only: no locale decides it
    for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return n;
}

// Why `name` cannot be a profile's (other than profile `except`'s); empty if it can.
inline std::string nameProblem(const Library& lib, const std::string& name, size_t except = (size_t)-1) {
    if (name.empty()) return "a profile needs a name";
    if (name.size() > kLongestName) return "a name of 40 letters at most";
    if (name != trimmed(name)) return "no spaces at either end";
    if (name.find_first_of("]\n\r") != std::string::npos) return "a name cannot hold ]";
    for (size_t i = 0; i < lib.names.size(); i++)
        if (i != except && folded(lib.names[i]) == folded(name)) return "there is a profile named " + lib.names[i] + " already";
    return "";
}

// "Game 2", "Game 3": the first such name not taken.
inline std::string freeName(const Library& lib) {
    for (int i = (int)lib.names.size() + 1;; i++)
        if (const std::string n = "Game " + std::to_string(i); nameProblem(lib, n).empty()) return n;
}

inline void switchTo(Library& lib, size_t i) {
    if (i >= lib.names.size() || i == lib.current) return;
    lib.games[lib.current] = lib.s.game;
    lib.current = i;
    lib.s.game = lib.games[i];
}

// A new profile keeps the turns, the boomerang and the motion of the one in
// use, but not its calibration: that belongs to the old game.
inline size_t addProfile(Library& lib, const std::string& name) {
    Game g = lib.s.game;
    g.cHand = g.cVirtual = 0;
    g.checked = false;
    lib.names.push_back(name);
    lib.games.push_back(g);
    switchTo(lib, lib.names.size() - 1);
    return lib.current;
}

// The last profile cannot go. The one in use going, its neighbour takes over:
// the one before it, or after it when it was the first.
inline bool deleteProfile(Library& lib, size_t i) {
    if (lib.names.size() < 2 || i >= lib.names.size()) return false;
    if (i == lib.current) switchTo(lib, i > 0 ? i - 1 : 1);
    lib.names.erase(lib.names.begin() + (long)i);
    lib.games.erase(lib.games.begin() + (long)i);
    if (lib.current > i) lib.current--;
    return true;
}

inline std::string libraryText(const Library& lib) {
    std::string o = "# spinjitzu settings\n" + globalText(lib.s) + "profile = " + lib.names[lib.current] + "\n";
    for (size_t i = 0; i < lib.names.size(); i++)
        o += "\n[" + lib.names[i] + "]\n" + gameText(i == lib.current ? lib.s.game : lib.games[i]);
    return o;
}

// A file from before profiles is one profile, "Default", calibration and all.
// A section whose name is not one a profile could have is skipped.
inline Library libraryFrom(const std::string& text) {
    Library lib;
    std::string head, chosen;
    std::vector<std::string> bodies;
    bool inSection = false, anySection = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            const std::string name = line.substr(1, line.size() - 2);
            anySection = true;
            inSection = nameProblem(lib, name).empty();
            if (inSection) { lib.names.push_back(name); bodies.emplace_back(); }
            continue;
        }
        if (inSection) bodies.back() += line + "\n";
        else if (anySection) continue;   // the lines of a skipped section
        else if (line.rfind("profile = ", 0) == 0) chosen = line.substr(10);
        else head += line + "\n";
    }
    lib.s = fromText(head);
    if (lib.names.empty()) {
        lib.names.push_back("Default");
        lib.games.push_back(lib.s.game);
        lib.migrated = text.find(" = ") != std::string::npos;
        return lib;
    }
    for (const std::string& b : bodies) lib.games.push_back(fromText(b).game);
    for (size_t i = 0; i < lib.names.size(); i++) if (lib.names[i] == chosen) lib.current = i;
    lib.s.game = lib.games[lib.current];
    return lib;
}

inline std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string text;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    return text;
}

inline bool writeFile(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".new";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

inline Library loadLibrary(const std::string& path) { return libraryFrom(readFile(path)); }

// The first save over a file from before profiles keeps it beside, once.
inline bool saveLibrary(const std::string& path, Library& lib) {
    if (lib.migrated) {
        const std::string old = path + ".before-profiles";
        if (FILE* f = fopen(old.c_str(), "r")) fclose(f);
        else writeFile(old, readFile(path));
        lib.migrated = false;
    }
    return writeFile(path, libraryText(lib));
}

}  // namespace spinjitzu
