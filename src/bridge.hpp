// Between the window and the worker that owns the devices and the core. The
// window hands over settings and commands; the worker hands back what to
// show. One lock, held only to copy, so neither side ever waits on the other
// for long.
#pragma once

#include <mutex>
#include <string>
#include "core.hpp"

namespace spinjitzu {

struct Shown {                    // what the worker says, for the window
    Core::Out core;
    std::string mouse;            // the mouse read as the hand, once one moved
    std::string problem;          // what stops the tool working, and the fix
    std::string mouseNode, virtualNode;
};

class Bridge {
public:
    // window side
    void settings(const Settings& s) { std::lock_guard<std::mutex> l(m_); settings_ = s; settingsNew_ = true; }
    void capture(int slot) { std::lock_guard<std::mutex> l(m_); capture_ = slot; captureNew_ = true; }
    void quit() { std::lock_guard<std::mutex> l(m_); quit_ = true; }
    // What the worker said since the last look: the log and the one-off
    // results are handed over once.
    Shown look() {
        std::lock_guard<std::mutex> l(m_);
        Shown s = shown_;
        shown_.core.log.clear();
        shown_.core.captureSlot = -1;
        shown_.core.newC = false;
        return s;
    }

    // worker side
    struct Orders { bool settingsNew = false, captureNew = false, quit = false; Settings settings; int capture = -1; };
    Orders take() {
        std::lock_guard<std::mutex> l(m_);
        Orders o;
        o.settingsNew = settingsNew_;
        if (settingsNew_) o.settings = settings_;
        o.captureNew = captureNew_;
        o.capture = capture_;
        o.quit = quit_;
        settingsNew_ = captureNew_ = false;
        return o;
    }
    void tell(const Core::Out& out) {
        std::lock_guard<std::mutex> l(m_);
        Core::Out& c = shown_.core;
        std::vector<std::string> log = std::move(c.log);
        const int slot = c.captureSlot, code = c.captured;
        const bool newC = c.newC;
        c = out;
        log.insert(log.end(), out.log.begin(), out.log.end());
        if (log.size() > 200) log.erase(log.begin(), log.end() - 200);
        c.log = std::move(log);
        // A one-off the window has not looked at yet is kept until it does.
        if (out.captureSlot < 0 && slot >= 0) { c.captureSlot = slot; c.captured = code; }
        if (!out.newC && newC) c.newC = true;
    }
    void tellDevices(const std::string& mouse, const std::string& problem, const std::string& mouseNode,
                     const std::string& virtualNode) {
        std::lock_guard<std::mutex> l(m_);
        shown_.mouse = mouse;
        shown_.problem = problem;
        shown_.mouseNode = mouseNode;
        shown_.virtualNode = virtualNode;
    }

private:
    std::mutex m_;
    Settings settings_;
    bool settingsNew_ = false, captureNew_ = false, quit_ = false;
    int capture_ = -1;
    Shown shown_;
};

}  // namespace spinjitzu
