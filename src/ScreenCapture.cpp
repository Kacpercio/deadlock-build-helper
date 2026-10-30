#include "ScreenCapture.h"

#include <opencv2/imgproc.hpp>

#include <iostream>

using Microsoft::WRL::ComPtr;

ScreenCapture::ScreenCapture(int monitorIndex)
    : monitorIndex_(monitorIndex)
{
    if (!initialize()) {
        std::cerr << "[capture] could not start Desktop Duplication on monitor " << monitorIndex_ << '\n';
    }
}

void ScreenCapture::reset()
{
    staging_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}

bool ScreenCapture::initialize()
{
    reset();

    // Walk every adapter's outputs. On hybrid-GPU laptops the monitor may hang off the
    // iGPU while the default adapter is the dGPU, so "adapter 0, output N" is not enough.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    int seen = 0;
    bool found = false;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            if (seen++ == monitorIndex_) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        std::cerr << "[capture] monitor_index " << monitorIndex_ << " does not exist (" << seen << " found)\n";
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);
    desktopRect_ = outputDesc.DesktopCoordinates;

    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) {
        return false;
    }

    // Fails with E_ACCESSDENIED on the secure desktop (UAC / lock screen); grab() retries later.
    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        std::cerr << "[capture] DuplicateOutput failed, hr=0x" << std::hex << hr << std::dec << '\n';
        reset();
        return false;
    }
    return true;
}

std::optional<cv::Mat> ScreenCapture::grab(unsigned timeoutMs)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!duplication_ && !initialize()) {
            return std::nullopt;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nothing was presented since the last grab, so the cached frame is still on screen.
            if (lastFrame_.empty()) {
                return std::nullopt;
            }
            return lastFrame_.clone();
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            duplication_.Reset();
            continue;
        }
        if (FAILED(hr)) {
            return std::nullopt;
        }

        const bool copied = copyToMat(resource.Get());
        duplication_->ReleaseFrame();
        if (!copied) {
            return std::nullopt;
        }
        // Clone: lastFrame_'s buffer is reused by the next grab.
        return lastFrame_.clone();
    }
    return std::nullopt;
}

bool ScreenCapture::copyToMat(IDXGIResource* resource)
{
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        // HDR desktops duplicate as R16G16B16A16_FLOAT.
        std::cerr << "[capture] unsupported desktop format " << desc.Format
                  << " - turn off Windows HDR for this monitor\n";
        return false;
    }
    if (!ensureStaging(desc)) {
        return false;
    }

    context_->CopyResource(staging_.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    // Wrap the mapped GPU memory without copying (RowPitch may exceed width * 4).
    const cv::Mat bgra(static_cast<int>(desc.Height), static_cast<int>(desc.Width), CV_8UC4,
                       mapped.pData, mapped.RowPitch);
    cv::cvtColor(bgra, lastFrame_, cv::COLOR_BGRA2BGR);
    context_->Unmap(staging_.Get(), 0);
    return true;
}

bool ScreenCapture::ensureStaging(const D3D11_TEXTURE2D_DESC& source)
{
    if (staging_) {
        D3D11_TEXTURE2D_DESC current{};
        staging_->GetDesc(&current);
        if (current.Width == source.Width && current.Height == source.Height) {
            return true;
        }
        staging_.Reset();
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = source.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging_));
}
