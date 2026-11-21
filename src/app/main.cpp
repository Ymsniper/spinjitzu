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
#include "scroll.hpp"
#include "theme.hpp"

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
        // device goes flat, so a turn's speed never changes its size. Only
        // KDE's way is tried here: elsewhere the Output tab shows the command.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (desktop() == Desktop::KdeWayland) setProfile(node, true);
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
        if (o.armNew) core.arm(o.arm);

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

// Ink on paper (theme.hpp). Every control is drawn with an ink border, so
// none of them can go unseen again, as Armed once did.
static const ImVec4 kInk = col(theme::kSumi);
static const ImVec4 kFadedInk = col(theme::kFaded);
static const ImVec4 kSeal = col(theme::kShu);
static const ImVec4 kIndigo = col(theme::kAi);

static void applyTheme() {
    using namespace theme;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18, 14);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(10, 8);
    st.FrameRounding = 3;
    st.GrabRounding = 2;
    st.TabRounding = 0;
    st.WindowBorderSize = 0;
    st.FrameBorderSize = 1;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 0;
    st.GrabMinSize = 12;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kInk;
    c[ImGuiCol_TextDisabled] = kFadedInk;
    c[ImGuiCol_WindowBg] = col(kWashi, 0);   // the paper is drawn under it
    c[ImGuiCol_ChildBg] = col(kWashi, 0);
    c[ImGuiCol_PopupBg] = col(kWashi);
    c[ImGuiCol_Border] = col(kSumi, 0.8f);
    c[ImGuiCol_BorderShadow] = col(kSumi, 0);
    c[ImGuiCol_FrameBg] = col(kWell);
    c[ImGuiCol_FrameBgHovered] = col(mix(kSumi, kWell, 0.08));
    c[ImGuiCol_FrameBgActive] = col(mix(kSumi, kWell, 0.12));
    c[ImGuiCol_CheckMark] = kInk;
    c[ImGuiCol_SliderGrab] = kInk;
    c[ImGuiCol_SliderGrabActive] = kIndigo;
    c[ImGuiCol_Button] = col(kWashi);
    c[ImGuiCol_ButtonHovered] = col(kWell);
    c[ImGuiCol_ButtonActive] = col(mix(kSumi, kWell, 0.12));
    c[ImGuiCol_Header] = col(kWell);
    c[ImGuiCol_HeaderHovered] = col(mix(kSumi, kWell, 0.08));
    c[ImGuiCol_HeaderActive] = col(mix(kSumi, kWell, 0.12));
    c[ImGuiCol_Separator] = col(kSumi, 0.45f);
    c[ImGuiCol_Tab] = col(kWashi, 0);
    c[ImGuiCol_TabHovered] = col(kWell, 0.7f);
    c[ImGuiCol_TabSelected] = col(kWashi, 0);   // a cinnabar stroke marks it instead
    c[ImGuiCol_TabSelectedOverline] = kSeal;
    c[ImGuiCol_TabDimmed] = col(kWashi, 0);
    c[ImGuiCol_TabDimmedSelected] = col(kWashi, 0);
    c[ImGuiCol_TabDimmedSelectedOverline] = kSeal;
    c[ImGuiCol_ScrollbarBg] = col(kWashi, 0);
    c[ImGuiCol_ScrollbarGrab] = col(kSumi, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = col(kSumi, 0.5f);
    c[ImGuiCol_ScrollbarGrabActive] = col(kSumi, 0.65f);
    c[ImGuiCol_TextSelectedBg] = col(kAi, 0.25f);
    c[ImGuiCol_NavCursor] = kIndigo;
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
    Desktop desk = desktop();
    bool accelRead = false;
    std::string accel, yours, flatCmd;   // what the desktop says of Spinjitzu's mouse and of yours; the command for flat
    double copiedAt = -1e9;
    int xValues = 3;              // how many profiles X's libinput has, once read
    std::vector<std::string> log;
    int forceTab = -1;            // --shot: the tab to draw
    double fixedClock = -1;       // --shot: the time to draw at
    Art art;
    Texture2D paper{};
    double openAt = -1;           // when the scroll began to open; <0: it is open
    bool shownArmed = false;      // what the seal and the title show
    double armedAt = -1e9;        // when that last changed

    double clock() const { return fixedClock >= 0 ? fixedClock : GetTime(); }
    // The taskbar shows whether the turn keys work, while the game has focus.
    void title() const { SetWindowTitle(shownArmed ? "Spinjitzu スピン術 (armed)" : "Spinjitzu スピン術 (not armed)"); }

    int* slotKey(int slot) {
        if (slot == kSlotCalibrate) return &s.keyCalibrate;
        if (slot == kSlotTest) return &s.keyTest;
        if (slot == kSlotPanic) return &s.keyPanic;
        if (slot == kSlotBoomerang) return &s.boom.key;
        if (slot == kSlotArm) return &s.keyArm;
        const int i = slot - kSlotTurn0;
        return i >= 0 && i < (int)s.turns.size() ? &s.turns[i].key : nullptr;
    }
    const char* slotName(int slot) {
        static std::string n;
        n = slot == kSlotCalibrate ? "hand 360" : slot == kSlotTest ? "test 360" : slot == kSlotPanic ? "panic"
          : slot == kSlotBoomerang ? "boomerang" : slot == kSlotArm ? "arm" : "turn " + std::to_string(slot - kSlotTurn0 + 1);
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
        if (sh.core.armedNew && sh.core.armed != s.armed) { s.armed = sh.core.armed; changed(); }
        if (!sh.mouse.empty() && sh.mouse != s.mouse) { s.mouse = sh.mouse; changed(); }
    }

    void header(const Shown& sh) {
        const ImVec2 top = ImGui::GetCursorPos();
        logo(art, 84, theme::kSumi);
        ImGui::SameLine(0, 16);
        ImGui::BeginGroup();
        ImGui::PushFont(art.display, 30);
        ImGui::TextUnformatted("Spinjitzu");
        ImGui::PopFont();
        if (!s.cHand) ImGui::TextColored(kSeal, "Not calibrated");
        else {
            ImGui::Text("%lld counts a turn by hand, %lld virtual,", (long long)s.cHand, (long long)s.cVirtual);
            ImGui::SameLine();
            if (s.checked) ImGui::TextColored(kIndigo, "checked");
            else ImGui::TextColored(kSeal, "not checked: test 360 in Calibrate");
        }
        ImGui::TextDisabled("%s", sh.core.doing.c_str());
        ImGui::EndGroup();
        const ImVec2 below = ImGui::GetCursorPos();

        // The seal at the right, with what it means beside it; the seal and
        // its words are one button.
        const float side = 78, gap = 14, right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const char* state = s.armed ? "Armed" : "Not armed";
        const char* means = s.armed ? "turn keys work" : "turn keys do nothing";
        // The key that switches it from inside the game, bound right here.
        const std::string key = capturing == kSlotArm ? "press a key..." : "arm key: " + keyName(s.keyArm);
        ImGui::PushFont(art.display, 21);
        const ImVec2 stateSize = ImGui::CalcTextSize(state);
        ImGui::PopFont();
        const ImVec2 meansSize = ImGui::CalcTextSize(means);
        const ImVec2 keySize(ImGui::CalcTextSize(key.c_str()).x + 16, ImGui::GetFrameHeight());
        const float words = std::max({stateSize.x, meansSize.x, keySize.x});
        ImGui::SetCursorPos(ImVec2(right - side - gap - words, top.y + 2));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("armed", ImVec2(words + gap + side, side))) { s.armed = !s.armed; changed(); b.arm(s.armed); }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = p.y + side / 2 - (stateSize.y + meansSize.y + 4 + keySize.y) / 2;
        dl->AddText(art.display, 21, ImVec2(p.x + words - stateSize.x, ty), ImGui::GetColorU32(s.armed ? kSeal : kInk), state);
        dl->AddText(ImVec2(p.x + words - meansSize.x, ty + stateSize.y), ImGui::GetColorU32(kInk), means);
        ImGui::SetCursorScreenPos(ImVec2(p.x + words - keySize.x, ty + stateSize.y + meansSize.y + 4));
        if (ImGui::Button((key + "##armkey").c_str(), keySize) && capturing < 0) { capturing = kSlotArm; b.capture(kSlotArm); }
        seal(art, ImVec2(p.x + words + gap, p.y), side, shownArmed, clock() - armedAt);

        ImGui::SetCursorPos(ImVec2(below.x, std::max(below.y, top.y + side + 6)));
        if (!sh.problem.empty()) ImGui::TextColored(kSeal, "%s", sh.problem.c_str());
        rule(2.6f, 1);
    }

    void calibrateTab(const Shown& sh) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600);   // lines short enough to read
        ImGui::TextWrapped("In the game, crosshair on a sharp mark:");
        for (const char* step : {"press the hand 360 key, turn one full circle by hand back onto the mark, press it again;",
                                 "press the test key: the tool turns a full circle, you put the crosshair back on the mark, then the hand 360 key;",
                                 "test again until it lands on the mark and says checked;",
                                 "then the test key runs the speed check: the same circle four times as fast. Back on the mark by hand, "
                                 "then the hand 360 key: if it was off, the game sees acceleration on Spinjitzu's mouse."}) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", step);
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Text("Hand 360 key");  ImGui::SameLine(160); bindButton(kSlotCalibrate);
        ImGui::Text("Test 360 key");  ImGui::SameLine(160); bindButton(kSlotTest);
        ImGui::Spacing();
        if (s.checked) {
            if (sh.core.speedCheck == 1) ImGui::TextColored(kIndigo, "Speed check: it landed. The game does not see acceleration.");
            else if (sh.core.speedCheck == 2)
                ImGui::TextColored(kSeal, "Speed check: %lld counts off. The game sees acceleration on Spinjitzu's mouse: see Output.",
                                   (long long)sh.core.speedOff);
            else ImGui::TextDisabled("Speed check: not run yet.");
        }
        if (sh.core.calibrating || sh.core.correcting)
            ImGui::TextColored(kIndigo, "%lld counts", (long long)sh.core.calNet);
        rule(1.6f, 2);
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
        if (!s.cHand) ImGui::TextColored(kSeal, "Calibrate first: turns need to know how far a full turn is.");
        int remove = -1;
        for (size_t i = 0; i < s.turns.size(); i++) {
            TurnBind& t = s.turns[i];
            ImGui::PushID((int)i);
            if (i) rule(1.4f, 10 + (uint32_t)i);
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
        rule(1.4f, 3);
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
        ImGui::Text("Turns go out through: Spinjitzu's virtual mouse%s", sh.virtualNode.empty() ? "" : (" (" + sh.virtualNode + ")").c_str());
        rule(1.4f, 5);
        ImGui::PushFont(art.display, 19);
        ImGui::TextUnformatted("Acceleration");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600);
        ImGui::Text("Desktop: %s", desktopName(desk));
        // Asked once and on the button, never every frame: each ask runs a program.
        const bool again = ImGui::Button("Read again");
        if (again || (!accelRead && !sh.virtualNode.empty())) {
            accelRead = true;
            if (desk == Desktop::X11) accel = xinputProfile(run(std::string("xinput list-props 'pointer:") + kOwnName + "'"), &xValues);
            else accel = profileOf(desk, sh.virtualNode, kOwnName, kOwnVendor, kOwnProduct);
            yours = desk == Desktop::KdeWayland ? accelOf(sh.mouseNode) : "";
        }
        flatCmd = flatCommand(desk, sh.virtualNode, kOwnName, kOwnVendor, kOwnProduct, xValues);
        ImGui::Text("Spinjitzu's mouse: %s", accel.empty() ? "cannot be read here" : accel.c_str());
        if (!yours.empty()) ImGui::Text("Your mouse: %s", yours.c_str());
        if (desk == Desktop::KdeWayland) ImGui::TextDisabled("Spinjitzu sets its own mouse to flat when it starts.");
        else if (!flatCmd.empty()) {
            ImGui::TextUnformatted("To make it flat, run:");
            ImGui::SetNextItemWidth(std::min(600.0f, ImGui::GetContentRegionAvail().x - 90));
            ImGui::InputText("##flat", const_cast<char*>(flatCmd.c_str()), flatCmd.size() + 1, ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button(clock() - copiedAt < 1.5 ? "Copied" : "Copy")) { SetClipboardText(flatCmd.c_str()); copiedAt = clock(); }
            if (desk == Desktop::Gnome) ImGui::TextColored(kSeal, "GNOME has one setting for every mouse: this makes yours flat too.");
        } else ImGui::TextWrapped("Set the mouse named %s to flat in your desktop's mouse settings, if it has one.", kOwnName);
        ImGui::TextDisabled("Whether your game sees acceleration at all: the speed check in Calibrate.");
        ImGui::PopTextWrapPos();
        rule(1.4f, 6);
        ImGui::Text("Panic key (stops everything)"); ImGui::SameLine(240); bindButton(kSlotPanic);
    }

    void footer(const Shown& sh) {
        const Core::Last& l = sh.core.last;
        rule(2.6f, 4);
        if (!l.any) { ImGui::TextDisabled("no turn yet"); return; }
        ImGui::Text("last turn: %.3f degrees, %lld counts", (double)l.milli / kMilli, (long long)l.counts);
        ImGui::SameLine();
        if (l.stopped) ImGui::TextColored(kSeal, "stopped: %lld of %lld sent", (long long)l.sent, (long long)l.counts);
        else if (l.sent == l.counts) ImGui::TextColored(kIndigo, "all sent");
        else ImGui::TextDisabled("%lld sent so far", (long long)l.sent);
    }

    void frame() {
        const Shown sh = b.look();
        apply(sh);
        if (s.armed != shownArmed) {
            shownArmed = s.armed;
            armedAt = clock();
            title();
        }
        // Opening, nothing takes a click: the click that skips it lands on
        // no control under the roll.
        const bool opening = openAt >= 0 && clock() - openAt < kOpenHold + kOpenRoll;
        // The window's widgets sit on the paper, between the roller and the cover.
        const Frame f = frameFor((float)GetScreenWidth(), (float)GetScreenHeight());
        ImGui::SetNextWindowPos(ImVec2(f.contentL(), f.paperT + 6));
        ImGui::SetNextWindowSize(ImVec2(f.contentR() - f.contentL(), f.paperB - f.paperT - 12));
        ImGui::Begin("Spinjitzu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                           (opening ? ImGuiWindowFlags_NoInputs : 0));
        header(sh);
        ImGui::BeginChild("tabs", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6));
        // Tab names in the display face, what is under them in the body's.
        auto tab = [&](const char* name, int i) {
            ImGui::PushFont(art.display, 19);
            const bool open = ImGui::BeginTabItem(name, nullptr, forceTab == i ? ImGuiTabItemFlags_SetSelected : 0);
            ImGui::PopFont();
            if (open) {   // the open tab, underlined in cinnabar
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                brush(ImGui::GetWindowDrawList(), ImVec2(lo.x + 4, hi.y - 2), ImVec2(hi.x - 4, hi.y - 2), 3.2f,
                      ImGui::GetColorU32(kSeal), 50 + i);
            }
            return open;
        };
        if (ImGui::BeginTabBar("tabs")) {
            if (tab("Calibrate", 0)) { calibrateTab(sh); ImGui::EndTabItem(); }
            if (tab("Turns", 1)) { turnsTab(); ImGui::EndTabItem(); }
            if (tab("Boomerang", 2)) { boomerangTab(); ImGui::EndTabItem(); }
            if (tab("Motion", 3)) {
                ImGui::TextDisabled("For every turn without its own.");
                motionRow("defaults", &s.speed, &s.curve, &s.jitter);
                ImGui::EndTabItem();
            }
            if (tab("Output", 4)) { outputTab(sh); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        footer(sh);
        ImGui::End();
        if (opening) {
            ::spinjitzu::opening(art, (float)GetScreenWidth(), (float)GetScreenHeight(), clock() - openAt);
            if (fixedClock < 0 && (GetKeyPressed() || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)))
                openAt = -1;
        } else openAt = -1;
        // Written once the changes stop, not on every step of a slider.
        if (dirty && !path.empty() && GetTime() - changedAt > 0.5) { saveSettings(path, s); dirty = false; }
    }
};

int main(int argc, char** argv) {
    // spinjitzu --shot out.png [tab] : the window drawn off screen into a
    // picture, to look at without a desktop session in the way. No device is
    // opened and the settings file is only read.
    const bool shot = argc >= 3 && !strcmp(argv[1], "--shot");
    const std::string path = settingsPath();
    Bridge b;
    Ui ui(b, loadSettings(path), shot ? "" : path);
    b.settings(ui.s);
    b.arm(ui.s.armed);
    std::thread w;
    if (!shot) w = std::thread(worker, std::ref(b));

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT | (shot ? FLAG_WINDOW_HIDDEN : 0));
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(820, 640, "Spinjitzu");
    SetTargetFPS(60);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    applyTheme();
    ui.art = loadArt();
    ui.paper = makePaper();
    // Settled from the start: the seal stamps only when it is changed.
    ui.shownArmed = ui.s.armed;
    ui.title();
    if (!shot && !desktopStill()) ui.openAt = GetTime();

    auto draw = [&] {
        ClearBackground(Color{theme::kTable.r, theme::kTable.g, theme::kTable.b, 255});
        drawScroll(ui.paper, (float)GetScreenWidth(), (float)GetScreenHeight());
        rlImGuiBegin();
        ui.frame();
        rlImGuiEnd();
    };
    if (shot) {
        ui.forceTab = argc > 3 ? atoi(argv[3]) : 0;
        ui.fixedClock = argc > 4 ? atof(argv[4]) : 100;
        if (argc > 5) ui.shownArmed = ui.s.armed = atoi(argv[5]) != 0;
        ui.openAt = 0;
        // A hidden window's own picture is undefined: draw into a texture.
        const RenderTexture2D rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        for (int i = 0; i < 4; i++) {   // ImGui lays a window out over its first frames
            BeginDrawing();
            BeginTextureMode(rt);
            draw();
            EndTextureMode();
            EndDrawing();
        }
        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);
        // The texture keeps blending's alpha, which a screen ignores.
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        const bool ok = ExportImage(img, argv[2]);
        UnloadImage(img);
        UnloadRenderTexture(rt);
        rlImGuiShutdown();
        CloseWindow();
        return ok ? 0 : 1;
    }
    while (!WindowShouldClose()) {
        // In the background the window has nothing to keep up with.
        SetTargetFPS(IsWindowFocused() ? 60 : 15);
        BeginDrawing();
        draw();
        EndDrawing();
    }
    if (ui.dirty) saveSettings(path, ui.s);
    b.quit();
    w.join();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
