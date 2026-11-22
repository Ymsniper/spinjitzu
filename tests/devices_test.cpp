// Checks for the desktop side of src/devices.hpp, on made-up environments
// and made-up program output: which desktop it is, what it says of a
// mouse's acceleration, and the command that makes ours flat there.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o devices_test tests/devices_test.cpp $(pkg-config --cflags --libs libevdev) && ./devices_test
#include <cstdio>
#include <string>
#include "../src/devices.hpp"

using namespace spinjitzu;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}

int main() {
    chk(desktopFrom("KDE", "wayland", false, false) == Desktop::KdeWayland, "KDE on Wayland goes through KWin");
    chk(desktopFrom("KDE", "x11", false, false) == Desktop::X11, "KDE on X11 goes through xinput: KWin's input settings are Wayland only");
    chk(desktopFrom("ubuntu:GNOME", "wayland", false, false) == Desktop::Gnome, "GNOME on Wayland, by any name it gives itself");
    chk(desktopFrom("sway", "wayland", true, false) == Desktop::Sway && desktopFrom("Hyprland", "wayland", false, true) == Desktop::Hyprland,
        "Sway and Hyprland are known by their sockets");
    chk(desktopFrom("niri", "wayland", false, false) == Desktop::Other, "anything else is said to be unknown");

    chk(swayId(kOwnVendor, kOwnProduct, "spinjitzu") == "23063:864:spinjitzu" && swayId(1, 2, "a b") == "1:2:a_b",
        "Sway's name for an input: decimal ids, spaces as underscores");

    int values = 0;
    const std::string props =
        "Device 'spinjitzu': Device Enabled (220): 1 libinput Accel Speed (300): 0.000000 "
        "libinput Accel Profile Enabled (301): 0, 1, 0 libinput Accel Profile Enabled Default (302): 1, 0, 0";
    chk(xinputProfile(props, &values) == "flat" && values == 3, "xinput: flat, on a libinput with three profiles");
    chk(xinputProfile("libinput Accel Profile Enabled (301):\t1, 0", &values) == "adaptive" && values == 2,
        "xinput: adaptive, on an older libinput with two");
    chk(xinputProfile("no such device") == "", "xinput with nothing to say says nothing");

    const std::string json =
        "[ { \"identifier\": \"1:2:other\", \"libinput\": { \"accel_profile\": \"flat\" } }, "
        "{ \"identifier\": \"23063:864:spinjitzu\", \"libinput\": { \"accel_speed\": 0.0, \"accel_profile\": \"adaptive\" } } ]";
    chk(swayProfile(json, "23063:864:spinjitzu") == "adaptive" && swayProfile(json, "9:9:none") == "",
        "Sway: the profile of our own input, not the one before it");

    chk(flatCommand(Desktop::X11, "event9", "spinjitzu", kOwnVendor, kOwnProduct, 2) ==
            "xinput set-prop 'pointer:spinjitzu' 'libinput Accel Profile Enabled' 0 1",
        "xinput's command sets as many profiles as the device has");
    chk(flatCommand(Desktop::Sway, "event9", "spinjitzu", kOwnVendor, kOwnProduct) == "swaymsg input 23063:864:spinjitzu accel_profile flat",
        "Sway's command names our input");
    chk(flatCommand(Desktop::KdeWayland, "event9", "spinjitzu", kOwnVendor, kOwnProduct).find("/InputDevice/event9 ") != std::string::npos,
        "KDE's command names our device node");
    chk(flatCommand(Desktop::Other, "event9", "spinjitzu", kOwnVendor, kOwnProduct).empty(), "no command for a desktop not known");

    chk(keyName(BTN_SIDE) == "Mouse 4" && keyName(BTN_EXTRA) == "Mouse 5" && keyName(KEY_F8) == "F8" && keyName(0) == "none" &&
            codeName(BTN_SIDE) == "BTN_SIDE",
        "keys shown as people know them, mouse side buttons as games name them; the file keeps the kernel's names");

    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
