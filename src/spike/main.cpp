// The calibration check on real devices: the keyboard and the mouse read
// directly, a virtual mouse of its own to turn with, a 1 ms timer, and
// spike.log in the folder it is run from. See session.hpp for the keys.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o build/spike src/spike/main.cpp && build/spike
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
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

static Ns monoNow() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Ns)t.tv_sec * kSecond + t.tv_nsec;
}

static bool bit(const unsigned long* bits, int n) {
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

struct Dev {
    int fd = -1;
    std::string path, name, node;   // node: eventN
    bool keyboard = false, mouse = false;
    unsigned vendor = 0, product = 0;
};

// What KDE has set for a device's acceleration, read only.
static std::string accelOf(const std::string& node) {
    if (node.empty()) return "unknown";
    const std::string cmd = "busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/" + node +
                            " org.kde.KWin.InputDevice pointerAcceleration pointerAccelerationProfileFlat"
                            " pointerAccelerationProfileAdaptive 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "unknown";
    std::string out;
    char b[128];
    while (fgets(b, sizeof b, p)) { out += b; if (!out.empty() && out.back() == '\n') out.back() = ' '; }
    pclose(p);
    // busctl prints "d 0" "b true" "b false": speed, flat, adaptive
    double speed = 0;
    char flat[8] = "", adaptive[8] = "";
    if (sscanf(out.c_str(), "d %lf b %7s b %7s", &speed, flat, adaptive) != 3) return "not reported by KDE";
    char r[96];
    snprintf(r, sizeof r, "profile %s, speed %+.2f", !strcmp(flat, "true") ? "flat" : !strcmp(adaptive, "true") ? "adaptive" : "none", speed);
    return r;
}

static int makeVirtualMouse(std::string& node) {
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
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
    snprintf(u.name, sizeof u.name, "%s", kOwnName);
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

static void send(int fd, int64_t dx) {
    if (!dx) return;
    input_event ev[2]{};
    ev[0].type = EV_REL; ev[0].code = REL_X; ev[0].value = (int)dx;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof ev) != (ssize_t)sizeof ev) logLine(std::string("writing to the virtual mouse failed: ") + strerror(errno));
}

static std::vector<Dev> findDevices() {
    std::vector<Dev> out;
    DIR* d = opendir("/dev/input");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5)) continue;
        Dev v;
        v.node = e->d_name;
        v.path = std::string("/dev/input/") + e->d_name;
        v.fd = open(v.path.c_str(), O_RDONLY | O_NONBLOCK);
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
        v.keyboard = bit(ev, EV_KEY) && bit(keys, KEY_F8);
        v.mouse = bit(ev, EV_REL) && bit(rels, REL_X) && v.name != kOwnName;
        if (!v.keyboard && !v.mouse) { close(v.fd); continue; }
        int clk = CLOCK_MONOTONIC;
        ioctl(v.fd, EVIOCSCLOCKID, &clk);
        out.push_back(v);
    }
    closedir(d);
    return out;
}

static void onSignal(int) { g_stop = 1; }

int main() {
    g_log = fopen("spike.log", "a");
    if (!g_log) { perror("spike.log"); return 1; }
    fprintf(g_log, "\n");
    logLine(std::string("spinjitzu calibration check, session ") + (getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "unknown"));

    std::string ownNode;
    const int out = makeVirtualMouse(ownNode);
    if (out < 0) {
        logLine(std::string("cannot make a virtual mouse: ") + strerror(errno) + ". Load the uinput module, or check the uinput group.");
        return 1;
    }
    // The desktop needs a moment to take the new device in before it moves.
    usleep(1000000);
    logLine("virtual mouse " + ownNode + ": " + accelOf(ownNode));

    std::vector<Dev> devs = findDevices();
    Session s;
    int keyboards = 0;
    for (const Dev& v : devs) {
        if (!v.keyboard) continue;
        keyboards++;
        unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1] = {};
        ioctl(v.fd, EVIOCGKEY(sizeof held), held);
        for (int k : {kF8, kF9, kF10, kF12}) if (bit(held, k)) s.heldAtStart(k);
    }
    if (!keyboards) { logLine("no keyboard with an F8 key can be read: are you in the input group?"); return 1; }

    termios saved{};
    const bool tty = isatty(0) && tcgetattr(0, &saved) == 0;
    if (tty) { termios t = saved; t.c_lflag &= ~(ECHO | ICANON); tcsetattr(0, TCSANOW, &t); }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    bool ticking = false;
    int mouse = -1;   // index into devs once the hand has moved one
    logLine("game in focus, crosshair on something sharp. Move the mouse once so it can be picked, then F8.");

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
                    const Ns t = (Ns)e.input_event_sec * kSecond + (Ns)e.input_event_usec * 1000;
                    if (e.type == EV_SYN && e.code == SYN_DROPPED) logLine("events were dropped by the system: redo this step");
                    if (devs[i].mouse && e.type == EV_REL && e.code == REL_X && e.value) {
                        if (mouse < 0) {
                            mouse = (int)i;
                            char b[160];
                            snprintf(b, sizeof b, "reading %s (%04x:%04x, %s): ", devs[i].name.c_str(), devs[i].vendor, devs[i].product, devs[i].path.c_str());
                            logLine(b + accelOf(devs[i].node));
                        }
                        if ((int)i == mouse) s.hand(e.value, t);
                    }
                    if (devs[i].keyboard && e.type == EV_KEY) {
                        if (e.value == 1 && e.code == kF8 && mouse < 0) { logLine("move the mouse first, so it can be picked"); continue; }
                        s.key(e.code, e.value, t);
                    }
                }
            }
        }
        if (pf.back().revents & POLLIN) {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof expirations) > 0) send(out, s.tick(monoNow()));
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
    if (tty) { tcflush(0, TCIFLUSH); tcsetattr(0, TCSANOW, &saved); }
    ioctl(out, UI_DEV_DESTROY);
    close(out);
    logLine("done; spike.log has all of this");
    fclose(g_log);
    return 0;
}
