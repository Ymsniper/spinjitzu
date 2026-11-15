// Spinjitzu: the window, and the worker thread that owns the devices and the
// core (SPEC 7b, 9, 10). The window never needs focus: binds are read from
// the devices directly, so the game keeps it.
// raylib names its keys KEY_A and so on, as the kernel's input header does
// with macros: raylib goes first, and nothing here uses its key names.
#include <imgui.h>
#include <raylib.h>
#include <rlImGui.h>

#include <cerrno>
#include <chrono>
#include <poll.h>
#include <set>
#include <sys/timerfd.h>
#include <thread>

#include "../bridge.hpp"
#include "../devices.hpp"
#include "../settings.hpp"

using namespace spinjitzu;

static const char* kOwnName = "spinjitzu";

// ── the worker ────────────────────────────────────────────────────────────────

static std::set<std::string> eventNodes() {
    std::set<std::string> s;
    if (DIR* d = opendir("/dev/input")) {
        while (dirent* e = readdir(d)) if (!strncmp(e->d_name, "event", 5)) s.insert(e->d_name);
        closedir(d);
    }
    return s;
}

static void worker(Bridge& b) {
    std::string node, problem;
    const int out = makeVirtualMouse(kOwnName, node);
    if (out < 0) problem = "Cannot make the virtual mouse (/dev/uinput). Run: sudo modprobe uinput, and add yourself to the uinput group.";
    else {
        // The desktop needs a moment to take the new device in; then our own
        // device goes flat, so a turn's speed never changes its size.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        setProfile(node, true);
    }

    Core core;
    std::vector<Dev> devs;
    std::set<std::string> seen;
    std::string wantMouse, hand, mouseNode;   // the mouse asked for by name, the one read, its node
    auto open = [&] {
        for (Dev& d : devs) if (d.fd >= 0) close(d.fd);
        devs = findDevices(kOwnName);
        seen = eventNodes();
        int keys = 0;
        for (const Dev& d : devs) {
            if (!d.keys) continue;
            keys++;
            unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1];
            heldKeys(d.fd, held);
            for (int c = 1; c < 768; c++) if (bit(held, c)) core.keyState(c, true);
        }
        if (!keys && problem.empty())
            problem = "Cannot read the keyboard or mouse. Run: sudo usermod -aG input $USER, then log out and in.";
    };
    open();
    const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    bool ticking = false;
    auto lastScan = std::chrono::steady_clock::now();
    b.tellDevices(hand, problem, "", node);

    for (;;) {
        const Bridge::Orders o = b.take();
        if (o.quit) break;
        if (o.settingsNew) {
            core.use(o.settings);
            if (o.settings.mouse != wantMouse) { mouseNode.clear(); wantMouse = o.settings.mouse; hand = wantMouse; b.tellDevices(hand, problem, "", node); }
        }
        if (o.captureNew) core.capture(o.capture, monoNow());

        std::vector<pollfd> pf;
        for (const Dev& d : devs) pf.push_back({d.fd, POLLIN, 0});
        pf.push_back({timer, POLLIN, 0});
        if (poll(pf.data(), pf.size(), ticking ? 5 : 30) < 0 && errno != EINTR) break;

        bool lost = false;
        for (size_t i = 0; i < devs.size(); i++) {
            Dev& d = devs[i];
            if (d.fd < 0) continue;
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (d.keys) core.keyDeviceGone();
                close(d.fd);
                d.fd = -1;
                lost = true;
                continue;
            }
            if (!(pf[i].revents & POLLIN)) continue;
            input_event ev[64];
            ssize_t n;
            while ((n = read(d.fd, ev, sizeof ev)) > 0)
                for (ssize_t k = 0; k < n / (ssize_t)sizeof(input_event); k++) {
                    const input_event& e = ev[k];
                    const Ns t = monoNow();
                    if (e.type == EV_SYN && e.code == SYN_DROPPED && d.keys) {
                        unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1];
                        heldKeys(d.fd, held);
                        for (int c = 1; c < 768; c++) core.keyState(c, bit(held, c));
                    }
                    if (d.keys && e.type == EV_KEY) core.key(e.code, e.value, t);
                    if (d.mouse && e.type == EV_REL && e.code == REL_X && e.value) {
                        if (hand.empty() || (d.name == hand && mouseNode.empty())) {
                            hand = d.name;
                            mouseNode = d.node;
                            b.tellDevices(hand, problem, mouseNode, node);
                        }
                        if (d.name == hand) core.hand(e.value, t);
                    }
                }
        }
        if (pf.back().revents & POLLIN) {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof expirations) > 0 && !sendX(out, core.tick(monoNow()))) core.sendFailed();
        }
        // Ticks only while something is turning: idle, the tool sleeps.
        if (core.idle() == ticking) {
            ticking = !core.idle();
            itimerspec it{};
            if (ticking) it.it_interval.tv_nsec = it.it_value.tv_nsec = kTick;
            timerfd_settime(timer, 0, &it, nullptr);
        }
        if (!ticking) core.tick(monoNow());
        b.tell(core.take());

        // A mouse or keyboard plugged in or out: everything is opened again,
        // when nothing is turning.
        const auto now = std::chrono::steady_clock::now();
        if (core.idle() && (lost || now - lastScan > std::chrono::seconds(2))) {
            lastScan = now;
            if (lost || eventNodes() != seen) open();
        }
    }
    for (Dev& d : devs) if (d.fd >= 0) close(d.fd);
    if (out >= 0) { ioctl(out, UI_DEV_DESTROY); close(out); }
}

// ── the window ────────────────────────────────────────────────────────────────

// One accent, #7cffa8 on near black, as in umbra's menu.
static const ImVec4 kAccent(0.486f, 1.000f, 0.659f, 1.0f);
static const ImVec4 kWarn(1.0f, 0.75f, 0.30f, 1.0f);
static const ImVec4 kBad(1.0f, 0.42f, 0.42f, 1.0f);
static ImVec4 accent(float a) { return ImVec4(kAccent.x, kAccent.y, kAccent.z, a); }

static void applyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowPadding = ImVec2(14, 12);
    st.ItemSpacing = ImVec2(8, 7);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.812f, 0.910f, 0.863f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.435f, 0.502f, 0.475f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.016f, 0.027f, 0.039f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.027f, 0.043f, 0.051f, 0.97f);
    c[ImGuiCol_Border] = accent(0.20f);
    c[ImGuiCol_FrameBg] = accent(0.07f);
    c[ImGuiCol_FrameBgHovered] = accent(0.15f);
    c[ImGuiCol_FrameBgActive] = accent(0.24f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = accent(0.62f);
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = accent(0.10f);
    c[ImGuiCol_ButtonHovered] = accent(0.24f);
    c[ImGuiCol_ButtonActive] = accent(0.38f);
    c[ImGuiCol_Header] = accent(0.12f);
    c[ImGuiCol_HeaderHovered] = accent(0.24f);
    c[ImGuiCol_HeaderActive] = accent(0.36f);
    c[ImGuiCol_Separator] = accent(0.22f);
    c[ImGuiCol_Tab] = accent(0.06f);
    c[ImGuiCol_TabHovered] = accent(0.28f);
    c[ImGuiCol_TabSelected] = accent(0.18f);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TextSelectedBg] = accent(0.30f);
    c[ImGuiCol_NavCursor] = kAccent;
}

// A slider with a box beside it to type the same number into.
static bool sliderTyped(const char* id, const char* label, double* v, double lo, double hi, const char* fmt) {
    ImGui::PushID(id);
    float f = (float)*v;
    ImGui::SetNextItemWidth(240);
    bool changed = ImGui::SliderFloat("##s", &f, (float)lo, (float)hi, fmt);
    if (changed) *v = f;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputDouble("##t", v, 0, 0, fmt)) changed = true;
    *v = std::isfinite(*v) ? std::clamp(*v, lo, hi) : lo;
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

struct Ui {
    Ui(Bridge& br, Settings st, std::string p) : b(br), s(std::move(st)), path(std::move(p)) {}
    Bridge& b;
    Settings s;
    std::string path;
    bool dirty = false;
    double changedAt = 0;
    int capturing = -1;
    std::string accel;            // what KDE said, when asked
    std::vector<std::string> log;

    int* slotKey(int slot) {
        if (slot == kSlotCalibrate) return &s.keyCalibrate;
        if (slot == kSlotTest) return &s.keyTest;
        if (slot == kSlotPanic) return &s.keyPanic;
        if (slot == kSlotBoomerang) return &s.boom.key;
        const int i = slot - kSlotTurn0;
        return i >= 0 && i < (int)s.turns.size() ? &s.turns[i].key : nullptr;
    }
    const char* slotName(int slot) {
        static std::string n;
        n = slot == kSlotCalibrate ? "hand 360" : slot == kSlotTest ? "test 360" : slot == kSlotPanic ? "panic"
          : slot == kSlotBoomerang ? "boomerang" : "turn " + std::to_string(slot - kSlotTurn0 + 1);
        return n.c_str();
    }
    void changed() { dirty = true; changedAt = GetTime(); b.settings(s); }

    // The key a slot is bound to, as a button: pressed, it waits for the next
    // key or button on any device.
    void bindButton(int slot) {
        int* k = slotKey(slot);
        if (!k) return;
        ImGui::PushID(slot);
        const std::string label = capturing == slot ? "press a key..." : keyName(*k);
        if (ImGui::Button((label + "##bind").c_str(), ImVec2(130, 0)) && capturing < 0) { capturing = slot; b.capture(slot); }
        ImGui::PopID();
    }

    void apply(const Shown& sh) {
        for (const std::string& l : sh.core.log) log.push_back(l);
        if (log.size() > 60) log.erase(log.begin(), log.end() - 60);
        if (sh.core.captureSlot >= 0) {
            const int slot = sh.core.captureSlot, code = sh.core.captured;
            capturing = -1;
            if (code > 0) {
                int clash = -1;
                for (int other = 0; other < kSlotTurn0 + (int)s.turns.size(); other++)
                    if (other != slot && slotKey(other) && *slotKey(other) == code) clash = other;
                if (clash >= 0) log.push_back(keyName(code) + " is already the " + slotName(clash) + " key: not bound");
                else if (int* k = slotKey(slot)) { *k = code; changed(); log.push_back("bound " + keyName(code) + " to " + slotName(slot)); }
            }
        }
        if (sh.core.newC) { s.cHand = sh.core.cHand; s.cVirtual = sh.core.cVirtual; s.checked = sh.core.checked; changed(); }
        if (!sh.mouse.empty() && sh.mouse != s.mouse) { s.mouse = sh.mouse; changed(); }
    }

    void header(const Shown& sh) {
        ImGui::TextColored(kAccent, "SPINJITZU");
        ImGui::SameLine();
        if (!s.cHand) ImGui::TextColored(kWarn, "| not calibrated");
        else {
            ImGui::TextDisabled("| %lld counts a turn by hand, %lld virtual", (long long)s.cHand, (long long)s.cVirtual);
            ImGui::SameLine();
            if (s.checked) ImGui::TextColored(kAccent, "checked");
            else ImGui::TextColored(kWarn, "not checked: test 360 in Calibrate");
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 140);
        if (ImGui::Checkbox("Armed", &s.armed)) changed();
        if (!sh.problem.empty()) ImGui::TextColored(kBad, "%s", sh.problem.c_str());
        ImGui::TextDisabled("%s", sh.core.doing.c_str());
        ImGui::Separator();
    }

    void calibrateTab(const Shown& sh) {
        ImGui::TextWrapped("In the game, crosshair on a sharp mark:");
        for (const char* step : {"press the hand 360 key, turn one full circle by hand back onto the mark, press it again;",
                                 "press the test key: the tool turns a full circle, you put the crosshair back on the mark, then the hand 360 key;",
                                 "test again until it lands on the mark and says checked."}) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", step);
        }
        ImGui::Spacing();
        ImGui::Text("Hand 360 key");  ImGui::SameLine(160); bindButton(kSlotCalibrate);
        ImGui::Text("Test 360 key");  ImGui::SameLine(160); bindButton(kSlotTest);
        ImGui::Spacing();
        if (sh.core.calibrating || sh.core.correcting)
            ImGui::TextColored(kAccent, "%lld counts", (long long)sh.core.calNet);
        ImGui::Separator();
        ImGui::BeginChild("log", ImVec2(0, 0));
        for (const std::string& l : log) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    void motionRow(const char* id, double* speed, Curve* curve, double* jitter) {
        ImGui::PushID(id);
        if (sliderTyped("speed", "degrees a second", speed, kSlowest, kFastest, "%.0f")) changed();
        int c = *curve == Curve::Smooth ? 0 : 1;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("curve", &c, "smooth (like a hand)\0constant\0")) { *curve = c ? Curve::Constant : Curve::Smooth; changed(); }
        bool none = *jitter <= 0;
        if (ImGui::Checkbox("no jitter", &none)) { *jitter = none ? 0 : 5; changed(); }
        if (!none) { ImGui::SameLine(); if (sliderTyped("jitter", "% jitter", jitter, 0.1, kMostJitter, "%.1f")) changed(); }
        ImGui::PopID();
    }

    void turnsTab() {
        if (!s.cHand) ImGui::TextColored(kWarn, "Calibrate first: turns need to know how far a full turn is.");
        int remove = -1;
        for (size_t i = 0; i < s.turns.size(); i++) {
            TurnBind& t = s.turns[i];
            ImGui::PushID((int)i);
            ImGui::Separator();
            bindButton(kSlotTurn0 + (int)i);
            ImGui::SameLine();
            int dir = t.dir < 0 ? 0 : 1;
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("##dir", &dir, "left\0right\0")) { t.dir = dir ? 1 : -1; changed(); }
            ImGui::SameLine();
            if (ImGui::Button("remove")) remove = (int)i;
            if (sliderTyped("deg", "degrees", &t.degrees, 0.001, 3600, "%.1f")) changed();
            if (ImGui::Checkbox("its own speed and jitter", &t.own)) changed();
            if (t.own) { ImGui::Indent(); motionRow("own", &t.speed, &t.curve, &t.jitter); ImGui::Unindent(); }
            ImGui::PopID();
        }
        if (remove >= 0) { s.turns.erase(s.turns.begin() + remove); changed(); }
        ImGui::Separator();
        if (ImGui::Button("+ Add turn")) { s.turns.push_back(TurnBind{}); changed(); capturing = kSlotTurn0 + (int)s.turns.size() - 1; b.capture(capturing); }
    }

    void boomerangTab() {
        ImGui::TextWrapped("Spins while its key is held; let go and it finishes facing where it started.");
        ImGui::Text("Key"); ImGui::SameLine(160); bindButton(kSlotBoomerang);
        int dir = s.boom.dir < 0 ? 0 : 1;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("direction", &dir, "left\0right\0")) { s.boom.dir = dir ? 1 : -1; changed(); }
        if (sliderTyped("spin", "degrees a second", &s.boom.speed, kSlowest, kFastest, "%.0f")) changed();
        int fin = s.boom.finish == Finish::Forward ? 0 : 1;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("finish", &fin, "keep spinning to the start\0shortest way back\0")) {
            s.boom.finish = fin ? Finish::Shortest : Finish::Forward;
            changed();
        }
        ImGui::TextDisabled("Your mouse movement while it spins is counted, so it still comes home.");
    }

    void outputTab(const Shown& sh) {
        ImGui::Text("Mouse read as your hand: %s", s.mouse.empty() ? "the first one you move" : s.mouse.c_str());
        if (!s.mouse.empty() && ImGui::Button("Use another: click, then move it")) { s.mouse.clear(); changed(); }
        ImGui::Text("Turns go out through: Spinjitzu's virtual mouse (%s)", sh.virtualNode.c_str());
        ImGui::Spacing();
        // KDE is asked on the button, never every frame.
        if (ImGui::Button("Read KDE's acceleration settings")) {
            auto of = [](const std::string& n) { const std::string a = n.empty() ? "" : accelOf(n); return a.empty() ? std::string("not known") : a; };
            accel = "your mouse: " + of(sh.mouseNode) + "; Spinjitzu's: " + of(sh.virtualNode);
        }
        if (!accel.empty()) ImGui::TextDisabled("%s", accel.c_str());
        ImGui::TextDisabled("Spinjitzu sets its own virtual mouse to flat, so a turn's speed never changes its size.");
        ImGui::Spacing();
        ImGui::Text("Panic key (stops everything)"); ImGui::SameLine(240); bindButton(kSlotPanic);
    }

    void footer(const Shown& sh) {
        const Core::Last& l = sh.core.last;
        ImGui::Separator();
        if (!l.any) { ImGui::TextDisabled("no turn yet"); return; }
        ImGui::Text("last turn: %.3f degrees, %lld counts", (double)l.milli / kMilli, (long long)l.counts);
        ImGui::SameLine();
        if (l.stopped) ImGui::TextColored(kWarn, "stopped: %lld of %lld sent", (long long)l.sent, (long long)l.counts);
        else if (l.sent == l.counts) ImGui::TextColored(kAccent, "all sent");
        else ImGui::TextDisabled("%lld sent so far", (long long)l.sent);
    }

    void frame() {
        const Shown sh = b.look();
        apply(sh);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)GetScreenWidth(), (float)GetScreenHeight()));
        ImGui::Begin("Spinjitzu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        header(sh);
        ImGui::BeginChild("tabs", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6));
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Calibrate")) { calibrateTab(sh); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Turns")) { turnsTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Boomerang")) { boomerangTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Motion")) {
                ImGui::TextDisabled("For every turn without its own.");
                motionRow("defaults", &s.speed, &s.curve, &s.jitter);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Output")) { outputTab(sh); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        footer(sh);
        ImGui::End();
        // Written once the changes stop, not on every step of a slider.
        if (dirty && GetTime() - changedAt > 0.5) { saveSettings(path, s); dirty = false; }
    }
};

int main() {
    const std::string path = settingsPath();
    Bridge b;
    Ui ui(b, loadSettings(path), path);
    b.settings(ui.s);
    std::thread w(worker, std::ref(b));

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(820, 640, "Spinjitzu");
    SetTargetFPS(60);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    applyTheme();

    while (!WindowShouldClose()) {
        // In the background the window has nothing to keep up with.
        SetTargetFPS(IsWindowFocused() ? 60 : 15);
        BeginDrawing();
        ClearBackground(Color{4, 7, 10, 255});
        rlImGuiBegin();
        ui.frame();
        rlImGuiEnd();
        EndDrawing();
    }
    if (ui.dirty) saveSettings(path, ui.s);
    b.quit();
    w.join();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
