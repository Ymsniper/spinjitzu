// The calibration check on real devices: the keyboard and the mouse read
// directly, a virtual mouse of its own to turn with, a 1 ms timer, and
// spike.log in the folder it is run from. See session.hpp for the keys.
// `--bind` binds each action to any key or mouse button, as a game's
// binding screen does, and keeps them in spike.keys.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o build/spike src/spike/main.cpp $(pkg-config --cflags --libs libevdev)
//   build/spike [--bind]
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include "../devices.hpp"
#include "session.hpp"

using namespace spinjitzu;
using namespace spinjitzu::spike;

static const char* kOwnName = "spinjitzu check";
static volatile sig_atomic_t g_stop = 0;
static FILE* g_log = nullptr;

static void logLine(const std::string& s) {
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t.tv_sec));
    printf("%s.%03ld %s\n", stamp, t.tv_nsec / 1000000, s.c_str());
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s.%03ld %s\n", stamp, t.tv_nsec / 1000000, s.c_str()); fflush(g_log); }
}

static std::string accelText(const std::string& node) {
    const std::string a = accelOf(node);
    return a.empty() ? "acceleration not reported by KDE" : "profile " + a;
}

// F7: both mice on the flat acceleration profile, and back. KDE's own
// settings are changed through KWin, logged, and put back when the check
// ends, so the control costs the user no trip to the settings.
struct Flat {
    struct Saved { std::string node; bool adaptive; };
    std::vector<Saved> saved;
    bool on = false;

    static bool get(const std::string& node, const char* prop) {
        const std::string cmd = "busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/" + node +
                                " org.kde.KWin.InputDevice " + prop + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        char b[32] = "";
        if (p) { if (!fgets(b, sizeof b, p)) b[0] = 0; pclose(p); }
        return strstr(b, "true") != nullptr;
    }
    static void set(const std::string& node, const char* prop) {
        const std::string cmd = "busctl --user set-property org.kde.KWin /org/kde/KWin/InputDevice/" + node +
                                " org.kde.KWin.InputDevice " + prop + " b true 2>/dev/null";
        if (system(cmd.c_str()) != 0) logLine("KDE did not take the change for " + node);
    }
    void toggle(const std::vector<std::string>& nodes) {
        if (!on) {
            saved.clear();
            for (const std::string& n : nodes) {
                if (n.empty()) continue;
                saved.push_back({n, get(n, "pointerAccelerationProfileAdaptive")});
                set(n, "pointerAccelerationProfileFlat");
            }
            on = true;
        } else {
            restore();
        }
        for (const std::string& n : nodes) if (!n.empty()) logLine("F7: " + n + " now " + accelText(n));
    }
    void restore() {
        if (!on) return;
        for (const Saved& v : saved)
            set(v.node, v.adaptive ? "pointerAccelerationProfileAdaptive" : "pointerAccelerationProfileFlat");
        on = false;
    }
};

static void onSignal(int) { g_stop = 1; }


static const char* kKeysFile = "spike.keys";

// The keys file: one `action = NAME` a line, NAME as the kernel names it
// (KEY_F8, BTN_SIDE), so it reads back the same on any keyboard layout.
static void loadKeys(int keys[kActions]) {
    FILE* f = fopen(kKeysFile, "r");
    if (!f) return;
    char word[32], name[64];
    while (fscanf(f, " %31[a-z] = %63s", word, name) == 2)
        for (int a = 0; a < kActions; a++)
            if (!strcmp(word, kActionWord[a])) {
                const int code = libevdev_event_code_from_name(EV_KEY, name);
                if (code > 0 && code < 768) keys[a] = code;
            }
    fclose(f);
}

static void saveKeys(const int keys[kActions]) {
    FILE* f = fopen(kKeysFile, "w");
    if (!f) { logLine(std::string("cannot write ") + kKeysFile + ": " + strerror(errno)); return; }
    for (int a = 0; a < kActions; a++) {
        const char* n = libevdev_event_code_get_name(EV_KEY, keys[a]);
        if (n) fprintf(f, "%s = %s\n", kActionWord[a], n);
    }
    fclose(f);
    logLine(std::string("keys kept in ") + kKeysFile);
}

// A game's binding screen: each action in turn waits for the next key or
// button pressed on any device. Esc keeps what it had; a key already doing
// another action is refused. Keys held when the wait starts count only once
// let go, so the Enter that started the tool binds nothing.
static void bindKeys(std::vector<Dev>& devs, int keys[kActions]) {
    for (int a = 0; a < kActions && !g_stop; a++) {
        bool down[768] = {};
        for (const Dev& v : devs) {
            if (!v.keys) continue;
            unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1] = {};
            ioctl(v.fd, EVIOCGKEY(sizeof held), held);
            for (int c = 0; c < 768; c++) down[c] = down[c] || bit(held, c);
        }
        logLine(std::string("press the key or mouse button for ") + kActionName[a] + " (now " + keyName(keys[a]) + ", Esc keeps it)");
        for (bool done = false; !done && !g_stop;) {
            std::vector<pollfd> pf;
            for (const Dev& v : devs) pf.push_back({v.fd, POLLIN, 0});
            if (poll(pf.data(), pf.size(), 200) < 0 && errno != EINTR) return;
            for (size_t i = 0; i < devs.size() && !done; i++) {
                if (!(pf[i].revents & POLLIN) || !devs[i].keys) continue;
                input_event ev[64];
                ssize_t n;
                while (!done && (n = read(devs[i].fd, ev, sizeof ev)) > 0)
                    for (ssize_t k = 0; k < n / (ssize_t)sizeof(input_event) && !done; k++) {
                        const input_event& e = ev[k];
                        if (e.type != EV_KEY || e.code >= 768 || e.value == 2) continue;
                        const bool was = down[e.code];
                        down[e.code] = e.value != 0;
                        if (e.value != 1 || was) continue;
                        if (e.code == KEY_ESC) { done = true; break; }
                        int taken = -1;
                        for (int b = 0; b < kActions; b++) if (b != a && keys[b] == e.code) taken = b;
                        if (taken >= 0) { logLine(keyName(e.code) + " is already " + kActionName[taken] + ": press another"); continue; }
                        keys[a] = e.code;
                        done = true;
                    }
            }
        }
        logLine(std::string(kActionName[a]) + " = " + keyName(keys[a]));
    }
    saveKeys(keys);
}

int main(int argc, char** argv) {
    const bool rebind = argc > 1 && !strcmp(argv[1], "--bind");
    g_log = fopen("spike.log", "a");
    if (!g_log) { perror("spike.log"); return 1; }
    fprintf(g_log, "\n");
    logLine(std::string("spinjitzu calibration check, session ") + (getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "unknown"));

    std::string ownNode;
    const int out = makeVirtualMouse(kOwnName, ownNode);
    if (out < 0) {
        logLine(std::string("cannot make a virtual mouse: ") + strerror(errno) + ". Load the uinput module, or check the uinput group.");
        return 1;
    }
    // The desktop needs a moment to take the new device in before it moves.
    usleep(1000000);
    logLine("virtual mouse " + ownNode + ": " + accelText(ownNode));

    std::vector<Dev> devs = findDevices(kOwnName);
    int withKeys = 0;
    for (const Dev& v : devs) withKeys += v.keys;
    if (!withKeys) { logLine("no keyboard or mouse buttons can be read: are you in the input group?"); return 1; }

    termios saved{};
    const bool tty = isatty(0) && tcgetattr(0, &saved) == 0;
    if (tty) { termios t = saved; t.c_lflag &= ~(ECHO | ICANON); tcsetattr(0, TCSANOW, &t); }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
    Flat flat;

    int keys[kActions];
    std::copy(kDefaultKey, kDefaultKey + kActions, keys);
    loadKeys(keys);
    if (rebind) bindKeys(devs, keys);
    Session s;
    std::string list;
    for (int a = 0; a < kActions; a++) {
        s.bind((Action)a, keys[a], keyName(keys[a]));
        list += std::string(a ? ", " : "") + kActionName[a] + " " + keyName(keys[a]);
    }
    logLine("keys: " + list + (rebind ? "" : "  (build/spike --bind to change them)"));
    auto bound = [&](int code) { for (int a = 0; a < kActions; a++) if (keys[a] == code) return true; return false; };
    for (const Dev& v : devs) {
        if (!v.keys) continue;
        unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1] = {};
        ioctl(v.fd, EVIOCGKEY(sizeof held), held);
        for (int a = 0; a < kActions; a++) if (bit(held, keys[a])) s.heldAtStart(keys[a]);
    }

    const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    bool ticking = false;
    int mouse = -1;   // index into devs once the hand has moved one
    bool sayMouse = false;
    logLine("game in focus, crosshair on something sharp. Move the mouse once so it can be picked, then " + keyName(keys[kHand]) + ".");

    while (!g_stop && !s.quit()) {
        std::vector<pollfd> pf;
        for (const Dev& v : devs) pf.push_back({v.fd, POLLIN, 0});
        pf.push_back({timer, POLLIN, 0});
        if (poll(pf.data(), pf.size(), 200) < 0 && errno != EINTR) break;

        for (size_t i = 0; i < devs.size(); i++) {
            if (devs[i].fd < 0) continue;
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                logLine("device gone: " + devs[i].name);
                close(devs[i].fd);
                devs[i].fd = -1;
                if ((int)i == mouse) { logLine("the mouse being read is gone: stopping"); g_stop = 1; }
                continue;
            }
            if (!(pf[i].revents & POLLIN)) continue;
            input_event ev[64];
            ssize_t n;
            while ((n = read(devs[i].fd, ev, sizeof ev)) > 0) {
                for (ssize_t k = 0; k < n / (ssize_t)sizeof(input_event); k++) {
                    const input_event& e = ev[k];
                    // One clock for everything: the turns run on it.
                    const Ns t = monoNow();
                    if (e.type == EV_SYN && e.code == SYN_DROPPED) {
                        s.spoiled("the system dropped events");
                        unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1] = {};
                        if (devs[i].keys && ioctl(devs[i].fd, EVIOCGKEY(sizeof held), held) >= 0)
                            for (int a = 0; a < kActions; a++) s.keyState(keys[a], bit(held, keys[a]));
                    }
                    if (devs[i].mouse && e.type == EV_REL && e.code == REL_X && e.value) {
                        if (mouse < 0) { mouse = (int)i; sayMouse = true; }
                        if ((int)i == mouse) s.hand(e.value, t);
                    }
                    if (devs[i].keys && e.type == EV_KEY) {
                        // Only the check's own keys: the rest are the game's.
                        if (e.code == keys[kFlat]) {
                            if (e.value == 1 && mouse >= 0) flat.toggle({devs[mouse].node, ownNode});
                            else if (e.value == 1) logLine("move the mouse first, so it can be picked");
                            continue;
                        }
                        if (!bound(e.code)) continue;
                        if (e.value == 1 && e.code == keys[kHand] && mouse < 0) { logLine("move the mouse first, so it can be picked"); continue; }
                        s.key(e.code, e.value, t);
                    }
                }
            }
        }
        if (pf.back().revents & POLLIN) {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof expirations) > 0 && !sendX(out, s.tick(monoNow()))) {
                logLine(std::string("writing to the virtual mouse failed: ") + strerror(errno));
                s.spoiled("a write to the virtual mouse failed");
            }
        }
        // Asked of KDE out here, so that the asking never holds up reading
        // the mouse.
        if (sayMouse) {
            sayMouse = false;
            const Dev& m = devs[mouse];
            char b[200];
            snprintf(b, sizeof b, "reading %s (%04x:%04x, %s): ", m.name.c_str(), m.vendor, m.product, m.path.c_str());
            logLine(b + accelText(m.node));
        }
        for (const std::string& l : s.take()) logLine(l);
        if (s.wantsTicks() != ticking) {
            ticking = s.wantsTicks();
            itimerspec it{};
            if (ticking) it.it_interval.tv_nsec = it.it_value.tv_nsec = kTick;
            timerfd_settime(timer, 0, &it, nullptr);
        }
    }

    for (const std::string& l : s.take()) logLine(l);
    if (flat.on) {
        flat.restore();
        logLine("acceleration settings put back as they were");
    }
    if (tty) { tcflush(0, TCIFLUSH); tcsetattr(0, TCSANOW, &saved); }
    ioctl(out, UI_DEV_DESTROY);
    close(out);
    logLine("done; spike.log has all of this");
    fclose(g_log);
    return 0;
}
