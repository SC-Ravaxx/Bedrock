#pragma once

#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace SCMiner {

// Grabs the screen through the Desktop Duplication API instead of GDI.
//
// GDI's BitBlt hands back an 8-bit SDR image. On a display running in HDR that
// image has every highlight cut off at pure white: measured on the same scene,
// 22.7 % of the screen centre was 255/255/255 with HDR on against 0.4 % with it
// off, and the HUD text simply drowned in the white. No threshold, channel or
// template can recover that - the information is gone before the reader ever
// sees it.
//
// Duplication gives the desktop in its own format: the 16-bit float scRGB
// buffer when the display is in HDR, plain BGRA when it is not. The float
// buffer keeps the highlights, and this class tone-maps them down itself
// rather than letting Windows clip them.
//
// SDR is passed through byte for byte, so an SDR display gives the same
// image as GDI does.
class DxgiCapture {
public:
    ~DxgiCapture();

    // A region in virtual-screen coordinates, as 8-bit BGR. Empty when
    // duplication is unavailable (no permission, a remote session, a driver
    // that refuses) - the caller then falls back to GDI.
    cv::Mat grab(const cv::Rect& region);

    // Whether the last successful grab came from an HDR (float) desktop
    bool lastFrameWasHdr() const { return hdr_; }
    // The monitor the last grab was of, in virtual-screen coordinates: a
    // region that reaches past it comes back cut to it
    const cv::Rect& outputRect() const { return outputRect_; }

    // Where the light actually sits in the last HDR frame, in scRGB units
    // (1.0 = SDR white, 80 nits). Needed to set the tone curve from measurement
    // rather than from an assumption about what the game outputs. Empty for an
    // SDR frame, where the question does not arise.
    std::string rawStats() const;

    // The half-float to display-byte table, exposed so it can be checked
    // without an HDR display to hand: on a machine that never runs HDR the
    // branch that uses it is never executed, and a mistake in it would only
    // ever surface on somebody else's setup.
    static std::vector<unsigned char> makeToneCurve();
    const std::string& lastError() const { return lastError_; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Builds (or rebuilds) the duplication for the monitor the region sits on
    bool ensureFor(const cv::Rect& region);
    void reset();
    bool ensureStaging(int width, int height, DXGI_FORMAT format);
    cv::Mat convert(const D3D11_MAPPED_SUBRESOURCE& mapped, int width, int height) const;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> desktop_;   // our own copy of the last frame
    ComPtr<ID3D11Texture2D> staging_;   // the cropped region, CPU-readable

    HMONITOR monitor_ = nullptr;
    cv::Rect outputRect_;               // the monitor in virtual-screen coords
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    cv::Size stagingSize_{0, 0};
    DXGI_FORMAT stagingFormat_ = DXGI_FORMAT_UNKNOWN;
    bool hdr_ = false;
    bool content_ = false;              // a presented frame has been seen

    bool disabled_ = false;             // duplication refused; stop retrying
    std::string lastError_;

    // Maps every possible half-float bit pattern straight to a display byte,
    // tone-mapping and sRGB-encoding on the way. Built once - doing the curve
    // per pixel with expf and powf would cost more than the capture itself.
    std::vector<unsigned char> toneCurve_;
    void buildToneCurve();

    // scRGB value * 16, clamped to 255 - so one byte covers 0 .. 16.0 in
    // sixteenths, which is the range that matters
    std::vector<unsigned char> rawScale_;
    mutable std::vector<long long> rawHistogram_;
};

} // namespace SCMiner
