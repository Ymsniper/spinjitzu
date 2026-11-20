// The scroll: the fonts and the logo carried in the binary (assets/, each
// with its licence beside it), the paper, the roller, the brocade cover, the
// brush strokes and the seal.
#pragma once

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <raylib.h>
#include <vector>

#include "kLogo.h"
#include "kNotoBlack.h"
#include "kNotoMedium.h"
#include "kShojumaru.h"
#include "theme.hpp"

namespace spinjitzu {

struct Art {
    ImFont* body = nullptr;       // Noto Serif CJK JP Medium: what is read
    ImFont* display = nullptr;    // Shojumaru: the title, the tabs
    ImFont* seal = nullptr;       // Noto Serif CJK JP Black: スピン術 on the seal
    Texture2D logo{};             // white on clear, tinted when drawn
};

inline ImFont* addFont(const unsigned char* data, size_t size, float px) {
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;   // the bytes live in the binary
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), (int)size, px, &c);
}

// After the window and ImGui are up.
inline Art loadArt() {
    Art a;
    a.body = addFont(kNotoMedium, sizeof kNotoMedium, 17);
    a.display = addFont(kShojumaru, sizeof kShojumaru, 30);
    a.seal = addFont(kNotoBlack, sizeof kNotoBlack, 30);
    ImGui::GetIO().FontDefault = a.body;

    Image img = LoadImageFromMemory(".png", kLogo, sizeof kLogo);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    // Gold on the taskbar, square or the desktop stretches it.
    Image icon = ImageCopy(img);
    const int side = std::max(icon.width, icon.height);
    ImageResizeCanvas(&icon, side, side, (side - icon.width) / 2, (side - icon.height) / 2, Color{255, 255, 255, 0});
    ImageResize(&icon, 128, 128);
    ImageColorTint(&icon, Color{theme::kKin.r, theme::kKin.g, theme::kKin.b, 255});
    SetWindowIcon(icon);
    UnloadImage(icon);
    a.logo = LoadTextureFromImage(img);
    GenTextureMipmaps(&a.logo);   // drawn at a fifth of its size: no shimmer
    SetTextureFilter(a.logo, TEXTURE_FILTER_TRILINEAR);
    UnloadImage(img);
    return a;
}

inline ImVec4 col(theme::Rgb c, float a = 1.0f) { return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a); }

// The logo `h` high, in ink or gold.
inline void logo(const Art& a, float h, theme::Rgb tint) {
    const float w = h * (float)a.logo.width / (float)a.logo.height;
    ImGui::ImageWithBg(ImTextureID(a.logo.id), ImVec2(w, h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), col(tint));
}

// ── the paper ─────────────────────────────────────────────────────────────────

inline uint32_t mixBits(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline float unit(uint32_t x) { return (float)(mixBits(x) & 0xffffff) / (float)0xffffff; }   // 0..1

// Washi that tiles: soft mottling, grain, fibres and a few foxing spots. No
// pixel is darker than kWashiLow, so the contrast check on that colour
// holds wherever text lands.
inline Texture2D makePaper() {
    const int n = 512;
    std::vector<float> v((size_t)n * n, 0.0f);
    auto at = [&](int x, int y) -> float& { return v[(size_t)((y % n + n) % n) * n + (x % n + n) % n]; };
    // Value noise on a lattice that wraps, so the tile has no seam.
    auto noise = [&](int x, int y, int cells, uint32_t seed) {
        const float fx = (float)x * cells / n, fy = (float)y * cells / n;
        const int i = (int)fx, j = (int)fy;
        float tx = fx - i, ty = fy - j;
        tx = tx * tx * (3 - 2 * tx);
        ty = ty * ty * (3 - 2 * ty);
        auto l = [&](int a, int b) { return unit(((uint32_t)(a % cells) * 73856093u) ^ ((uint32_t)(b % cells) * 19349663u) ^ seed) * 2 - 1; };
        const float top = l(i, j) + (l(i + 1, j) - l(i, j)) * tx, bot = l(i, j + 1) + (l(i + 1, j + 1) - l(i, j + 1)) * tx;
        return top + (bot - top) * ty;
    };
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float m = 0.55f * noise(x, y, 4, 11) + 0.30f * noise(x, y, 16, 23) + 0.15f * noise(x, y, 64, 37);
            at(x, y) = 0.028f * m + 0.010f * (unit((uint32_t)(y * n + x) * 2654435761u) * 2 - 1);
        }
    for (int f = 0; f < 420; f++) {   // fibres: short curling strands, lighter or darker
        float x = unit(f * 3 + 1) * n, y = unit(f * 3 + 2) * n, a = unit(f * 3 + 3) * 6.2832f;
        const float len = 18 + unit(f * 7 + 5) * 70, curl = (unit(f * 7 + 6) - 0.5f) * 0.08f;
        const float shade = (unit(f * 7 + 4) < 0.6f ? 0.022f : -0.016f);
        for (float t = 0; t < len; t += 0.7f) {
            at((int)x, (int)y) += shade * (1 - std::fabs(t / len - 0.5f) * 1.6f);
            x += std::cos(a) * 0.7f; y += std::sin(a) * 0.7f; a += curl;
        }
    }
    for (int f = 0; f < 7; f++) {   // foxing
        const float cx = unit(f + 900) * n, cy = unit(f + 950) * n, r = 4 + unit(f + 990) * 12;
        for (int y = (int)(cy - r); y <= (int)(cy + r); y++)
            for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
                const float d = std::hypot(x - cx, y - cy) / r;
                if (d < 1) at(x, y) -= 0.035f * (1 - d * d);
            }
    }
    Image img = GenImageColor(n, n, BLANK);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* px = (Color*)img.data;
    using namespace theme;
    for (int i = 0; i < n * n; i++) {
        auto ch = [&](uint8_t base, uint8_t low) {
            return (unsigned char)std::clamp(base * (1.0f + v[i]), (float)low, 255.0f);
        };
        px[i] = Color{ch(kWashi.r, kWashiLow.r), ch(kWashi.g, kWashiLow.g), ch(kWashi.b, kWashiLow.b), 255};
    }
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    return t;
}

// ── the frame ─────────────────────────────────────────────────────────────────

inline Color rl(theme::Rgb c, float a = 1.0f) { return Color{c.r, c.g, c.b, (unsigned char)(a * 255 + 0.5f)}; }

// Where the parts of the open scroll are, for a window w by h.
struct Frame {
    float rollerX, rollerW, paperL, paperR, paperT, paperB, coverL, coverR;
    float contentL() const { return rollerX + rollerW + 4; }
    float contentR() const { return coverL - 4; }
};
inline Frame frameFor(float w, float h) {
    Frame f;
    f.paperT = 16;
    f.paperB = h - 16;
    f.rollerX = 10;
    f.rollerW = 20;
    f.paperL = f.rollerX + f.rollerW * 0.5f;
    f.coverR = w - 10;
    f.coverL = f.coverR - 30;
    f.paperR = f.coverL;
    return f;
}

// A wooden roller standing from top to bottom, gold knobs at both ends:
// lit from the left, like the rest of the scroll.
inline void roller(float x, float w, float top, float bottom) {
    using namespace theme;
    const Color dark = rl(mix(kTable, kKuri, 0.45)), light = rl(mix(kWashi, kKuri, 0.22));
    DrawRectangleGradientH((int)x, (int)top, (int)(w * 0.35f), (int)(bottom - top), dark, light);
    DrawRectangleGradientH((int)(x + w * 0.35f), (int)top, (int)(w * 0.65f) + 1, (int)(bottom - top), light, dark);
    const Color kd = rl(mix(kTable, kKin, 0.35)), kl = rl(mix(kWhite, kKin, 0.45));
    for (const float y : {top - 11, bottom + 1}) {
        DrawRectangleGradientH((int)(x - 2), (int)y, (int)((w + 4) * 0.35f), 10, kd, kl);
        DrawRectangleGradientH((int)(x - 2 + (w + 4) * 0.35f), (int)y, (int)((w + 4) * 0.65f) + 1, 10, kl, kd);
    }
}

// The open scroll: paper from the roller to the brocade cover.
inline void drawScroll(const Texture2D& paper, float w, float h) {
    using namespace theme;
    const Frame f = frameFor(w, h);
    const float ph = f.paperB - f.paperT;
    DrawRectangleGradientV((int)f.paperL + 4, (int)f.paperB, (int)(f.coverR - f.paperL) - 4, 7, Color{0, 0, 0, 120}, Color{0, 0, 0, 0});
    DrawTextureRec(paper, Rectangle{0, 0, f.paperR - f.paperL, ph}, Vector2{f.paperL, f.paperT}, WHITE);
    // Burnt top and bottom edges, where no text goes.
    DrawRectangleGradientV((int)f.paperL, (int)f.paperT, (int)(f.paperR - f.paperL), 10, rl(kBurn, 0.85f), rl(kBurn, 0));
    DrawRectangleGradientV((int)f.paperL, (int)f.paperB - 10, (int)(f.paperR - f.paperL), 10, rl(kBurn, 0), rl(kBurn, 0.85f));
    // The brocade cover, gold lozenges on indigo, and its bamboo stave.
    DrawRectangle((int)f.coverL, (int)f.paperT, (int)(f.coverR - f.coverL), (int)ph, rl(kAi));
    for (int row = 0; f.paperT + 6 + row * 9 < f.paperB - 4; row++)
        for (float x = f.coverL + 5 + (row % 2) * 5; x < f.coverR - 8; x += 10)
            DrawPoly(Vector2{x, f.paperT + 6 + row * 9}, 4, 2.0f, 0, rl(kKin, 0.38f));
    DrawRectangleGradientH((int)f.coverL - 7, (int)f.paperT, 7, (int)ph, rl(kTable, 0), rl(kTable, 0.35f));
    DrawRectangleGradientH((int)f.coverR - 6, (int)f.paperT - 2, 6, (int)ph + 4, rl(mix(kKin, kWashi, 0.35)), rl(mix(kKuri, kKin, 0.5)));
    // The cord that ties it shut, hanging loose while it is open.
    const float cy = f.paperT + ph * 0.42f;
    DrawSplineSegmentBezierQuadratic(Vector2{f.coverR - 3, cy}, Vector2{f.coverL - 10, cy + ph * 0.05f},
                                     Vector2{f.coverL + 8, cy + ph * 0.2f}, 2.2f, rl(kKin));
    for (int t = -2; t <= 2; t++)
        DrawLineEx(Vector2{f.coverL + 8, cy + ph * 0.2f}, Vector2{f.coverL + 8 + t * 1.6f, cy + ph * 0.2f + 16}, 1.2f, rl(kKin, 0.9f));
    // The roller, and its shadow on the paper.
    DrawRectangleGradientH((int)(f.rollerX + f.rollerW), (int)f.paperT, 10, (int)ph, Color{0, 0, 0, 70}, Color{0, 0, 0, 0});
    roller(f.rollerX, f.rollerW, f.paperT - 3, f.paperB + 3);
}

// ── brush work ────────────────────────────────────────────────────────────────

// A brush stroke from a to b: pressed hardest early, lifting off at both
// ends, with a waver that stays put from frame to frame for the same seed.
inline void brush(ImDrawList* dl, ImVec2 a, ImVec2 b, float thick, ImU32 col, uint32_t seed) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 2) return;
    const int n = std::clamp((int)(len / 4), 12, 240);
    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len), nrm(-d.y, d.x);
    const float ph1 = unit(seed) * 6.28f, ph2 = unit(seed + 1) * 6.28f;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const unsigned alpha = (col >> IM_COL32_A_SHIFT) & 0xff;
    // One strip, four rows: a one-pixel fade on each side of the ink, as
    // ImGui draws its own lines, so the stroke has no seams.
    dl->PrimReserve(n * 18, (n + 1) * 4);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i <= n; i++) {
        const float t = (float)i / n;
        const float w = thick * std::max(0.08f, std::pow(std::sin(3.14159f * std::pow(t, 0.7f)), 0.55f));
        const float wob = 0.14f * thick * std::sin(t * 7.5f + ph1) + 0.08f * thick * std::sin(t * 17.0f + ph2);
        const ImVec2 p(a.x + (b.x - a.x) * t + nrm.x * wob, a.y + (b.y - a.y) * t + nrm.y * wob);
        const float in = std::max(0.0f, w / 2 - 0.5f), out = in + 1.0f;
        // Thinner than a pixel: lighter, not narrower.
        const ImU32 c = (col & ~IM_COL32_A_MASK) | ((ImU32)(alpha * std::min(1.0f, w)) << IM_COL32_A_SHIFT);
        const ImU32 clear = col & ~IM_COL32_A_MASK;
        dl->PrimWriteVtx(ImVec2(p.x + nrm.x * out, p.y + nrm.y * out), uv, clear);
        dl->PrimWriteVtx(ImVec2(p.x + nrm.x * in, p.y + nrm.y * in), uv, c);
        dl->PrimWriteVtx(ImVec2(p.x - nrm.x * in, p.y - nrm.y * in), uv, c);
        dl->PrimWriteVtx(ImVec2(p.x - nrm.x * out, p.y - nrm.y * out), uv, clear);
    }
    for (int i = 0; i < n; i++) {
        const ImDrawIdx p0 = (ImDrawIdx)(base + i * 4), q0 = (ImDrawIdx)(p0 + 4);
        for (int r = 0; r < 3; r++) {
            dl->PrimWriteIdx(p0 + r); dl->PrimWriteIdx(q0 + r); dl->PrimWriteIdx(q0 + r + 1);
            dl->PrimWriteIdx(p0 + r); dl->PrimWriteIdx(q0 + r + 1); dl->PrimWriteIdx(p0 + r + 1);
        }
    }
}

// A brush rule across the width left on this line, in place of a hairline.
inline void rule(float thick = 2.4f, uint32_t seed = 1) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    brush(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 5), ImVec2(p.x + w, p.y + 5), thick,
          ImGui::GetColorU32(col(theme::kSumi, 0.85f)), seed);
    ImGui::Dummy(ImVec2(w, 10));
}

// ── the seal ──────────────────────────────────────────────────────────────────

// Moves, turns and fades everything drawn into `dl` since `from`.
inline void transformSince(ImDrawList* dl, int from, ImVec2 c, float scale, float degrees, float alpha) {
    const float r = degrees * 3.14159f / 180, cs = std::cos(r) * scale, sn = std::sin(r) * scale;
    for (int i = from; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float x = v.pos.x - c.x, y = v.pos.y - c.y;
        v.pos = ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
        const unsigned a = (unsigned)((v.col >> IM_COL32_A_SHIFT) & 0xff);
        v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)(a * std::clamp(alpha, 0.0f, 1.0f)) << IM_COL32_A_SHIFT);
    }
}

// A square cut by hand: a little uneven along every edge.
inline void roughSquare(ImVec2 c, float half, uint32_t seed, std::vector<ImVec2>& out) {
    out.clear();
    const ImVec2 corner[4] = {ImVec2(-1, -1), ImVec2(1, -1), ImVec2(1, 1), ImVec2(-1, 1)};
    for (int side = 0; side < 4; side++) {
        const ImVec2 p = corner[side], q = corner[(side + 1) % 4];
        for (int i = 0; i < 8; i++) {
            const float t = i / 8.0f, j = (unit(seed + side * 8 + i) - 0.5f) * 0.035f;
            const float x = p.x + (q.x - p.x) * t, y = p.y + (q.y - p.y) * t;
            out.push_back(ImVec2(c.x + half * x * (1 + j), c.y + half * y * (1 + j)));
        }
    }
}

// The Armed seal, `side` across with its top left at p. Armed: cinnabar
// with スピン術 cut out in white, read down the right column first; it
// stamps on as it arms. Not armed: the empty frame in full ink, never
// faint, so it cannot be missed the way the checkbox was.
inline void seal(const Art& a, ImVec2 p, float side, bool armed, double since) {
    using namespace theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + side / 2, p.y + side / 2);
    std::vector<ImVec2> sq;
    roughSquare(c, side / 2, 7, sq);
    const float k = (float)std::clamp(since / (armed ? 0.14 : 0.12), 0.0, 1.0);
    auto impression = [&] {
        dl->AddConcavePolyFilled(sq.data(), (int)sq.size(), ImGui::GetColorU32(col(kShu)));
        const float g = side * 0.40f;
        const char* glyph[4] = {"ス", "ピ", "ン", "術"};
        const ImVec2 cell[4] = {ImVec2(0.25f, -0.23f), ImVec2(0.25f, 0.23f), ImVec2(-0.25f, -0.23f), ImVec2(-0.25f, 0.23f)};
        for (int i = 0; i < 4; i++) {
            const ImVec2 sz = a.seal->CalcTextSizeA(g, 1e9f, 0, glyph[i]);
            dl->AddText(a.seal, g, ImVec2(c.x + cell[i].x * side - sz.x / 2, c.y + cell[i].y * side - sz.y / 2),
                        ImGui::GetColorU32(col(kWhite)), glyph[i]);
        }
        for (int i = 0; i < 9; i++)   // where the paste did not take
            dl->AddCircleFilled(ImVec2(c.x + (unit(40 + i) - 0.5f) * side * 0.9f, c.y + (unit(60 + i) - 0.5f) * side * 0.9f),
                                0.6f + unit(80 + i) * 1.3f, ImGui::GetColorU32(col(kWashi, 0.55f)));
    };
    if (armed) {
        const float e = 1 - (1 - k) * (1 - k) * (1 - k);
        const int from = dl->VtxBuffer.Size;
        impression();
        transformSince(dl, from, c, 1.25f - 0.25f * e, -3.0f + 2.0f * e, std::min(1.0f, (float)since / 0.06f));
    } else {
        if (k < 1) {   // the seal lifting off
            const int from = dl->VtxBuffer.Size;
            impression();
            transformSince(dl, from, c, 1 + 0.08f * k, -1.0f, 1 - k);
        }
        const int from = dl->VtxBuffer.Size;
        dl->AddPolyline(sq.data(), (int)sq.size(), ImGui::GetColorU32(col(kSumi)), ImDrawFlags_Closed, 2.5f);
        transformSince(dl, from, c, 1, -1.0f, k);
    }
}

}  // namespace spinjitzu
