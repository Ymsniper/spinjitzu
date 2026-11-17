// Every colour pair that carries text, held to WCAG contrast: 7:1 for the
// ink people read, 4.5:1 for what is said in passing, 3:1 for the borders
// that show a control is there.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -o theme_test tests/theme_test.cpp && ./theme_test
#include <cstdio>
#include <string>
#include "../src/app/theme.hpp"

using namespace spinjitzu::theme;

static int g_bad = 0, g_n = 0;
static void chk(bool ok, const std::string& what) {
    g_n++;
    if (!ok) { g_bad++; printf("FAIL  %s\n", what.c_str()); }
}
static void atLeast(Rgb fg, Rgb bg, double least, const std::string& what) {
    const double c = contrast(fg, bg);
    chk(c >= least, what + ": " + std::to_string(c) + ", wants " + std::to_string(least));
}

int main() {
    for (const Rgb paper : {kWashi, kWashiLow}) {
        atLeast(kSumi, paper, 7.0, "ink on the paper");
        atLeast(kFaded, paper, 4.5, "faded ink on the paper");
        atLeast(kShu, paper, 4.5, "an error in cinnabar on the paper");
        atLeast(kAi, paper, 4.5, "indigo on the paper");
        atLeast(mix(kSumi, paper, 0.8), paper, 3.0, "a control's border, ink at 80%, on the paper");
    }
    atLeast(kSumi, kWell, 7.0, "ink inside a field or a checkbox");
    atLeast(kFaded, kWell, 4.5, "faded ink inside a field");
    atLeast(kSumi, mix(kSumi, kWell, 0.12), 7.0, "ink on a hovered field");
    atLeast(kWhite, kShu, 4.5, "the seal's white characters on cinnabar");
    atLeast(kKin, kAi, 4.5, "gold on the indigo cover");
    atLeast(kKin, kTable, 4.5, "gold on the table");
    printf("%s (%d of %d failed)\n", g_bad ? "FAILED" : "all good", g_bad, g_n);
    return g_bad ? 1 : 0;
}
