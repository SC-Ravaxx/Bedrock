#pragma once
// How an HDR desktop is brought down to eight bits (DxgiCapture), and the
// way back to what an SDR capture of the same screen holds.

#include <algorithm>
#include <cmath>

namespace SCMiner::ToneMap {

// scRGB is linear light with 1.0 at SDR white (80 nits). Where the picture
// sits on that scale, measured with --capture on this game in HDR:
//
//     median 0.25   p90 0.44   p99 4.62   p99.9 5.31   max 5.75
//
// The world - terrain, sky - lives below half of SDR white. The HUD and its
// markers live at four to six times it, which matches the 400 nits Star
// Citizen reports. The two are decades apart in light, and everything worth
// reading is in the upper group.
//
// Treating 1.0 as white, with a shoulder above it into the last ten display
// levels, puts digits on 253 and the plate under them on 244 - nine levels
// apart where the reader needs forty - and no frame reads (0 of 108). So
// white goes where the HUD is, and only what is brighter gets compressed.
inline constexpr float WHITE = 4.0f;        // the scRGB level that becomes white
inline constexpr float SHOULDER_AT = 0.8f;  // where the curve starts to bend
inline constexpr float SHOULDER_RATE = 3.0f;

inline float toneMap(float linear) {
    if (linear <= 0.0f) return 0.0f;   // scRGB allows negatives for wide gamut
    const float t = linear / WHITE;
    if (t <= SHOULDER_AT) return t;
    // Never quite reaches 1.0, so a sun or a lamp above the HUD level still
    // separates instead of collapsing into one flat white the way GDI leaves it
    return SHOULDER_AT + (1.0f - SHOULDER_AT) * (1.0f - std::exp(-(t - SHOULDER_AT) * SHOULDER_RATE));
}

// The reader was tuned on GDI captures, which are sRGB-encoded. Duplication's
// float buffer is linear, so it has to be encoded back or every mid-tone would
// land too dark and the digit thresholds would all be wrong.
inline float encodeSrgb(float linear) {
    if (linear <= 0.0031308f) return linear * 12.92f;
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

inline float decodeSrgb(float encoded) {
    if (encoded <= 0.04045f) return encoded / 12.92f;
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

// An eight-bit level of the tone-mapped picture as an SDR capture - GDI, or
// Duplication of an SDR desktop - has it: linear light again, cut at SDR
// white. Exact up to 137, which is SDR white; all above it is 255, as the
// shoulder only begins at 231.
inline unsigned char sdrLevel(int toneMapped) {
    const float linear = WHITE * decodeSrgb((float)toneMapped / 255.0f);
    return (unsigned char)std::lround(255.0f * encodeSrgb((std::min)(linear, 1.0f)));
}

}  // namespace SCMiner::ToneMap
