// The devices: every input device read directly, never grabbed, a virtual
// mouse of our own to turn with, and what KDE has set for each one's
// acceleration. Shared by the calibration check and the app.
#pragma once

#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include "engine.hpp"

namespace spinjitzu {

inline Ns monoNow() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Ns)t.tv_sec * kSecond + t.tv_nsec;
}

inline bool bit(const unsigned long* bits, int n) {
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

struct Dev {
    int fd = -1;
    std::string path, name, node;   // node: eventN
    bool keys = false, mouse = false;   // keys: it has keys or buttons
    unsigned vendor = 0, product = 0;
};

// Every device with keys, buttons or horizontal movement, except our own
// virtual mouse, which must never be read as the hand.
inline std::vector<Dev> findDevices(const std::string& ownName) {
    std::vector<Dev> out;
    DIR* d = opendir("/dev/input");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5)) continue;
        Dev v;
        v.node = e->d_name;
        v.path = std::string("/dev/input/") + e->d_name;
        v.fd = open(v.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (v.fd < 0) continue;
        char name[256] = "";
        ioctl(v.fd, EVIOCGNAME(sizeof name), name);
        v.name = name;
        input_id id{};
        ioctl(v.fd, EVIOCGID, &id);
        v.vendor = id.vendor;
        v.product = id.product;
        unsigned long ev[1] = {}, keys[KEY_MAX / (8 * sizeof(long)) + 1] = {}, rels[1] = {};
        ioctl(v.fd, EVIOCGBIT(0, sizeof ev), ev);
        if (bit(ev, EV_KEY)) ioctl(v.fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        if (bit(ev, EV_REL)) ioctl(v.fd, EVIOCGBIT(EV_REL, sizeof rels), rels);
        v.keys = bit(ev, EV_KEY) && v.name != ownName;
        v.mouse = bit(ev, EV_REL) && bit(rels, REL_X) && v.name != ownName;
        if (!v.keys && !v.mouse) { close(v.fd); continue; }
        out.push_back(v);
    }
    closedir(d);
    return out;
}

// The keys a device reports held right now.
inline void heldKeys(int fd, unsigned long (&held)[KEY_MAX / (8 * sizeof(long)) + 1]) {
    memset(held, 0, sizeof held);
    ioctl(fd, EVIOCGKEY(sizeof held), held);
}

// A key or button as people know it: F8, not KEY_F8.
inline std::string keyName(int code) {
    if (code <= 0) return "none";
    const char* n = libevdev_event_code_get_name(EV_KEY, code);
    if (!n) return "key " + std::to_string(code);
    const std::string s = n;
    return s.rfind("KEY_", 0) == 0 ? s.substr(4) : s;
}

// The kernel's own name for a code, for files: KEY_F8, BTN_SIDE.
inline std::string codeName(int code) {
    const char* n = code > 0 ? libevdev_event_code_get_name(EV_KEY, code) : nullptr;
    return n ? n : "none";
}
inline int codeOf(const std::string& name) {
    const int c = libevdev_event_code_from_name(EV_KEY, name.c_str());
    return c > 0 && c < 768 ? c : 0;
}

// What KDE has set for a device's acceleration, read through KWin. Empty
// where KDE does not say.
inline std::string accelOf(const std::string& node) {
    if (node.empty()) return "";
    const std::string cmd = "busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/" + node +
                            " org.kde.KWin.InputDevice pointerAcceleration pointerAccelerationProfileFlat"
                            " pointerAccelerationProfileAdaptive 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char b[128];
    while (fgets(b, sizeof b, p)) { out += b; if (!out.empty() && out.back() == '\n') out.back() = ' '; }
    pclose(p);
    // busctl prints "d 0" "b true" "b false": speed, flat, adaptive
    double speed = 0;
    char flat[8] = "", adaptive[8] = "";
    if (sscanf(out.c_str(), "d %lf b %7s b %7s", &speed, flat, adaptive) != 3) return "";
    char r[96];
    snprintf(r, sizeof r, "%s, speed %+.2f", !strcmp(flat, "true") ? "flat" : !strcmp(adaptive, "true") ? "adaptive" : "none", speed);
    return r;
}

// Sets one of KDE's acceleration profiles on a device, through KWin.
inline bool setProfile(const std::string& node, bool flat) {
    if (node.empty()) return false;
    const std::string cmd = "busctl --user set-property org.kde.KWin /org/kde/KWin/InputDevice/" + node +
                            " org.kde.KWin.InputDevice " +
                            (flat ? "pointerAccelerationProfileFlat" : "pointerAccelerationProfileAdaptive") +
                            " b true 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

// Our own relative mouse. `node` gets its eventN.
inline int makeVirtualMouse(const std::string& name, std::string& node) {
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    // Buttons, or the desktop does not take it for a mouse.
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    uinput_setup u{};
    u.id.bustype = BUS_USB;
    u.id.vendor = 0x5a17;
    u.id.product = 0x0360;
    snprintf(u.name, sizeof u.name, "%s", name.c_str());
    if (ioctl(fd, UI_DEV_SETUP, &u) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    char sys[64] = "";
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0) {
        const std::string dir = std::string("/sys/devices/virtual/input/") + sys;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) if (!strncmp(e->d_name, "event", 5)) node = e->d_name;
            closedir(d);
        }
    }
    return fd;
}

// Horizontal counts through the virtual mouse. False if they did not go.
inline bool sendX(int fd, int64_t dx) {
    if (!dx) return true;
    input_event ev[2]{};
    ev[0].type = EV_REL; ev[0].code = REL_X; ev[0].value = (int)dx;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    return write(fd, ev, sizeof ev) == (ssize_t)sizeof ev;
}

}  // namespace spinjitzu
