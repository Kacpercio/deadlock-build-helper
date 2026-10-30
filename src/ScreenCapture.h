#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>

#include <optional>

// Grabs the desktop image of one monitor through the DXGI Desktop Duplication API.
// This reads what DWM composited for the monitor - the same pixels you see - and never
// touches the game process. Works for borderless-windowed games; BitBlt on a DirectX
// game window usually returns a black image, which is why it is not used here.
//
// Not thread-safe: create it anywhere, then use it from one thread at a time.
class ScreenCapture {
public:
    explicit ScreenCapture(int monitorIndex = 0);

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool ok() const { return duplication_ != nullptr; }

    // Monitor position in virtual-desktop coordinates; the overlay window is placed here.
    RECT desktopRect() const { return desktopRect_; }

    // Latest frame as 8-bit BGR. Recreates the duplication if Windows revoked it
    // (resolution change, fullscreen toggle, UAC prompt).
    std::optional<cv::Mat> grab(unsigned timeoutMs = 500);

    // Opens the duplication ahead of grab(), which then has nothing to wait for.
    void prepare();

    // Stops duplicating until the next grab(). An open duplication makes Windows keep compositing
    // the desktop for it, which costs the game frames even when nothing is being read.
    void suspend();

private:
    bool initialize();
    void reset();
    bool copyToMat(IDXGIResource* resource);
    bool ensureStaging(const D3D11_TEXTURE2D_DESC& source);

    int monitorIndex_;
    RECT desktopRect_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    cv::Mat lastFrame_;
    bool justOpened_ = false;
};
