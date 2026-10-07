#include "DxgiCapture.h"

#include <cmath>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace SCMiner {

namespace {

// How long to wait for a new desktop frame. A still screen produces none at
// all, which is not an error - the copy already held is still what is on
// screen - so this only has to be short enough not to stall the scan.
constexpr UINT ACQUIRE_TIMEOUT_MS = 16;

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
constexpr float TONE_WHITE = 4.0f;        // the scRGB level that becomes white
constexpr float TONE_SHOULDER_AT = 0.8f;  // where the curve starts to bend
constexpr float TONE_SHOULDER_RATE = 3.0f;

float toneMap(float linear) {
    if (linear <= 0.0f) return 0.0f;   // scRGB allows negatives for wide gamut
    const float t = linear / TONE_WHITE;
    if (t <= TONE_SHOULDER_AT) return t;
    // Never quite reaches 1.0, so a sun or a lamp above the HUD level still
    // separates instead of collapsing into one flat white the way GDI leaves it
    return TONE_SHOULDER_AT + (1.0f - TONE_SHOULDER_AT) *
           (1.0f - std::exp(-(t - TONE_SHOULDER_AT) * TONE_SHOULDER_RATE));
}

// The reader was tuned on GDI captures, which are sRGB-encoded. Duplication's
// float buffer is linear, so it has to be encoded back or every mid-tone would
// land too dark and the digit thresholds would all be wrong.
float encodeSrgb(float linear) {
    if (linear <= 0.0031308f) return linear * 12.92f;
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

DxgiCapture::~DxgiCapture() {
    reset();
}

void DxgiCapture::reset() {
    staging_.Reset();
    desktop_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    monitor_ = nullptr;
    format_ = DXGI_FORMAT_UNKNOWN;
    stagingSize_ = cv::Size(0, 0);
    stagingFormat_ = DXGI_FORMAT_UNKNOWN;
    content_ = false;
}


void DxgiCapture::buildToneCurve() {
    if (!toneCurve_.empty()) return;
    toneCurve_ = makeToneCurve();

    rawScale_.resize(65536);
    rawHistogram_.assign(256, 0);
    cv::Mat bits(1, 65536, CV_16UC1);
    for (int i = 0; i < 65536; i++) bits.at<unsigned short>(0, i) = static_cast<unsigned short>(i);
    cv::Mat halves(1, 65536, CV_16FC1, bits.data);
    cv::Mat floats;
    halves.convertTo(floats, CV_32F);
    for (int i = 0; i < 65536; i++) {
        float v = floats.at<float>(0, i);
        if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
        rawScale_[i] = static_cast<unsigned char>(
            (std::min)(255L, std::lround(v * 16.0f)));
    }
}

std::string DxgiCapture::rawStats() const {
    long long total = 0;
    for (long long n : rawHistogram_) total += n;
    if (total == 0) return {};

    auto percentile = [&](double share) {
        const long long want = static_cast<long long>(total * share);
        long long seen = 0;
        for (int i = 0; i < 256; i++) {
            seen += rawHistogram_[i];
            if (seen >= want) return i / 16.0;
        }
        return 16.0;
    };
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "scRGB (1.0 = SDR white): median %.2f  p90 %.2f  p99 %.2f  p99.9 %.2f  max %.2f",
                  percentile(0.50), percentile(0.90), percentile(0.99),
                  percentile(0.999), percentile(0.99999));
    return buffer;
}


std::vector<unsigned char> DxgiCapture::makeToneCurve() {
    std::vector<unsigned char> toneCurve_(65536);

    // Decode every half-float bit pattern once, through OpenCV rather than by

    // hand: it gets the denormals and infinities right, which a quick shift-
    // and-mask version does not.
    cv::Mat bits(1, 65536, CV_16UC1);
    for (int i = 0; i < 65536; i++) bits.at<unsigned short>(0, i) = static_cast<unsigned short>(i);
    cv::Mat halves(1, 65536, CV_16FC1, bits.data);
    cv::Mat floats;
    halves.convertTo(floats, CV_32F);

    for (int i = 0; i < 65536; i++) {
        float v = floats.at<float>(0, i);
        if (!std::isfinite(v)) v = (v > 0.0f) ? 1.0f : 0.0f;   // inf / NaN
        const float encoded = encodeSrgb((std::min)(toneMap(v), 1.0f));
        toneCurve_[i] = static_cast<unsigned char>(
            std::lround((std::min)((std::max)(encoded, 0.0f), 1.0f) * 255.0f));
    }
    return toneCurve_;
}

bool DxgiCapture::ensureFor(const cv::Rect& region) {
    if (disabled_) return false;

    const POINT centre{region.x + region.width / 2, region.y + region.height / 2};
    HMONITOR monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
    if (duplication_ && monitor == monitor_) return true;

    reset();
    monitor_ = monitor;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        lastError_ = "CreateDXGIFactory1 failed";
        disabled_ = true;
        return false;
    }

    for (UINT a = 0;; a++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor_) continue;

            // The device has to live on the adapter that drives this output,
            // or DuplicateOutput refuses - which is the whole story on a
            // machine with an iGPU beside the discrete card.
            D3D_FEATURE_LEVEL level{};
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                         nullptr, 0, D3D11_SDK_VERSION,
                                         &device_, &level, &context_))) {
                lastError_ = "D3D11CreateDevice failed";
                reset();
                return false;
            }

            outputRect_ = cv::Rect(desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                                   desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                                   desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

            // Ask for both formats and let the runtime hand back whichever the
            // desktop already is: plain BGRA on an SDR display, the float
            // buffer on an HDR one. Asking for BGRA alone would make Windows
            // do the tone mapping - the clipping this class exists to avoid.
            HRESULT hr = E_FAIL;
            ComPtr<IDXGIOutput5> output5;
            if (SUCCEEDED(output.As(&output5))) {
                DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
                                         DXGI_FORMAT_R16G16B16A16_FLOAT};
                hr = output5->DuplicateOutput1(device_.Get(), 0, 2, formats, &duplication_);
            }
            if (FAILED(hr)) {
                // Older Windows: no format choice, always BGRA. Fine for SDR.
                ComPtr<IDXGIOutput1> output1;
                if (SUCCEEDED(output.As(&output1))) {
                    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
                }
            }
            if (FAILED(hr) || !duplication_) {
                lastError_ = "DuplicateOutput failed (0x" + std::to_string(hr) + ")";
                reset();
                // A machine that will not duplicate at all should not be asked
                // again on every scan; GDI still works there.
                if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_UNSUPPORTED) disabled_ = true;
                return false;
            }

            DXGI_OUTDUPL_DESC duplDesc{};
            duplication_->GetDesc(&duplDesc);
            format_ = duplDesc.ModeDesc.Format;
            hdr_ = (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (hdr_) buildToneCurve();
            lastError_.clear();
            return true;
        }
    }

    lastError_ = "no DXGI output covers that region";
    reset();
    return false;
}

bool DxgiCapture::ensureStaging(int width, int height, DXGI_FORMAT format) {
    if (staging_ && stagingSize_ == cv::Size(width, height) && stagingFormat_ == format) {
        return true;
    }
    staging_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
        lastError_ = "staging texture failed";
        return false;
    }
    stagingSize_ = cv::Size(width, height);
    stagingFormat_ = format;
    return true;
}

cv::Mat DxgiCapture::convert(const D3D11_MAPPED_SUBRESOURCE& mapped, int width, int height) const {
    if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // scRGB, RGBA order. Straight through the lookup table - no float
        // maths per pixel, which at a few million pixels a second matters.
        cv::Mat bgr(height, width, CV_8UC3);
        std::fill(rawHistogram_.begin(), rawHistogram_.end(), 0LL);
        for (int y = 0; y < height; y++) {
            const auto* src = reinterpret_cast<const unsigned short*>(
                static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
            auto* dst = bgr.ptr<unsigned char>(y);
            for (int x = 0; x < width; x++) {
                dst[0] = toneCurve_[src[2]];   // B
                dst[1] = toneCurve_[src[1]];   // G
                dst[2] = toneCurve_[src[0]];   // R
                // The brightest channel is what decides where the curve has to
                // put this pixel, so that is what the histogram records
                const unsigned char raw = (std::max)(
                    rawScale_[src[0]], (std::max)(rawScale_[src[1]], rawScale_[src[2]]));
                rawHistogram_[raw]++;
                src += 4;
                dst += 3;
            }
        }
        return bgr;

    }

    // BGRA already: what GDI would have given, without the round trip
    const cv::Mat wrapped(height, width, CV_8UC4,
                          const_cast<void*>(mapped.pData), mapped.RowPitch);
    cv::Mat bgr;
    cv::cvtColor(wrapped, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

cv::Mat DxgiCapture::grab(const cv::Rect& region) {
    if (region.width < 1 || region.height < 1) return {};
    if (!ensureFor(region)) return {};

    // The first acquire after a duplication is created hands back a frame
    // whose surface has not been drawn into yet - copying it gives a black
    // screen. A real frame is one the desktop actually presented, and only
    // LastPresentTime says so. Until one has been seen, keep asking; after
    // that a timeout is normal and simply means nothing moved.
    const int attempts = content_ ? 1 : 12;
    for (int attempt = 0; attempt < attempts; attempt++) {
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};
        const HRESULT hr = duplication_->AcquireNextFrame(ACQUIRE_TIMEOUT_MS, &info, &resource);

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // A mode change, a game going fullscreen, a resolution switch:
            // the duplication is void and has to be built again.
            reset();
            return {};
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;   // nothing new on screen
        if (FAILED(hr)) {
            lastError_ = "AcquireNextFrame failed";
            reset();
            return {};
        }

        ComPtr<ID3D11Texture2D> frame;
        const bool real = info.LastPresentTime.QuadPart != 0;
        if (real && SUCCEEDED(resource.As(&frame))) {
            if (!desktop_) {
                D3D11_TEXTURE2D_DESC desc{};
                frame->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                if (FAILED(device_->CreateTexture2D(&desc, nullptr, &desktop_))) {
                    duplication_->ReleaseFrame();
                    lastError_ = "desktop copy failed";
                    reset();
                    return {};
                }
            }
            context_->CopyResource(desktop_.Get(), frame.Get());
            content_ = true;
        }
        duplication_->ReleaseFrame();
        if (content_) break;
    }


    // A timeout only means nothing on screen changed, so the copy already held
    // is still correct. Having none at all means we have never seen a frame.
    if (!desktop_) return {};

    cv::Rect local(region.x - outputRect_.x, region.y - outputRect_.y,
                   region.width, region.height);
    local &= cv::Rect(0, 0, outputRect_.width, outputRect_.height);
    if (local.width < 1 || local.height < 1) return {};

    if (!ensureStaging(local.width, local.height, format_)) return {};

    const D3D11_BOX box{static_cast<UINT>(local.x), static_cast<UINT>(local.y), 0,
                        static_cast<UINT>(local.x + local.width),
                        static_cast<UINT>(local.y + local.height), 1};
    context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, desktop_.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        lastError_ = "Map failed";
        return {};
    }
    cv::Mat result = convert(mapped, local.width, local.height);
    context_->Unmap(staging_.Get(), 0);
    return result;
}

} // namespace SCMiner
