// The scroll's colours, kept free of any library so a check can hold every
// pair that carries text to its contrast (tests/theme_test.cpp).
#pragma once

#include <cmath>
#include <cstdint>

namespace spinjitzu::theme {

struct Rgb { uint8_t r, g, b; };

constexpr Rgb kWashi{0xE2, 0xD0, 0xA6};     // the paper
constexpr Rgb kWashiLow{0xD9, 0xC5, 0x97};  // the darkest the paper may get under text
constexpr Rgb kBurn{0xB7, 0x8F, 0x5A};      // the paper's burnt edge, never under text
constexpr Rgb kWell{0xD6, 0xC2, 0x91};      // inside a field, a box, a checkbox
constexpr Rgb kSumi{0x2A, 0x21, 0x19};      // ink
constexpr Rgb kFaded{0x5A, 0x49, 0x36};     // ink, for what is said in passing
constexpr Rgb kShu{0x9E, 0x21, 0x18};       // cinnabar: the seal, errors
constexpr Rgb kKin{0xC8, 0x96, 0x2C};       // gold leaf
constexpr Rgb kAi{0x24, 0x32, 0x4F};        // indigo brocade
constexpr Rgb kKuri{0x3B, 0x26, 0x17};      // the roller's wood
constexpr Rgb kTable{0x17, 0x11, 0x0C};     // behind the scroll
constexpr Rgb kWhite{0xFF, 0xFF, 0xFF};

// WCAG 2 relative luminance and contrast ratio.
inline double luminance(Rgb c) {
    auto lin = [](uint8_t v) {
        const double x = v / 255.0;
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}
inline double contrast(Rgb a, Rgb b) {
    const double x = luminance(a), y = luminance(b);
    return (std::fmax(x, y) + 0.05) / (std::fmin(x, y) + 0.05);
}

// `a` laid over `b` at `t` (0: all b, 1: all a).
constexpr Rgb mix(Rgb a, Rgb b, double t) {
    auto m = [t](uint8_t x, uint8_t y) { return (uint8_t)(y + (x - y) * t + 0.5); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

}  // namespace spinjitzu::theme
