// What the scroll is drawn with: the fonts and the logo carried in the
// binary (assets/, each with its licence beside it).
#pragma once

#include <algorithm>
#include <imgui.h>
#include <raylib.h>

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

}  // namespace spinjitzu
