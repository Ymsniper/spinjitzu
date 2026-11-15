// The bridge between the window and the worker, on two real threads: the
// window changing settings and asking for bindings as fast as it can, the
// worker running a real core on made-up keys. Every binding asked for comes
// back exactly once, and no log line is lost or doubled. Run it under the
// thread sanitizer too (cmake -DSANITIZE=thread).
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include "../src/bridge.hpp"

using namespace spinjitzu;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}

int main() {
    Bridge b;
    const int kAsks = 3000;
    std::atomic<int> asked{0};
    std::atomic<bool> stop{false};

    std::thread worker([&] {
        Core core;
        Ns now = 0;
        int answered = 0;
        while (true) {
            const Bridge::Orders o = b.take();
            if (o.quit) break;
            if (o.settingsNew) core.use(o.settings);
            now += kTick;
            if (o.captureNew) { core.capture(o.capture, now); now += kTick; core.key(KEY_A + answered % 20, 1, now); now += kTick; core.key(KEY_A + answered % 20, 0, now); answered++; }
            core.tick(now);
            b.tell(core.take());
        }
    });

    int got = 0, logs = 0;
    bool slotsRight = true;
    Settings s;
    int waiting = -1;
    for (int frame = 0; got < kAsks && frame < 10'000'000; frame++) {
        s.speed = 30 + frame % 1000;
        b.settings(s);
        s.armed = frame % 2;
        if (waiting < 0 && asked < kAsks) { waiting = asked % 7; b.capture(waiting); asked++; }
        const Shown sh = b.look();
        for (const std::string& l : sh.core.log) logs += l.rfind("pressed ", 0) == 0;
        if (sh.core.captureSlot >= 0) {
            slotsRight = slotsRight && sh.core.captureSlot == waiting && sh.core.captured > 0;
            got++;
            waiting = -1;
        }
    }
    b.quit();
    worker.join();
    (void)stop;
    chk(got == kAsks && slotsRight, "every binding asked for comes back once, for the slot that asked");
    chk(logs == kAsks, "every log line arrives once");
    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
