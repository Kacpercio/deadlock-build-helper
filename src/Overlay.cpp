#include "Overlay.h"

#include "SettingsMenu.h"
#include "VisionEngine.h"

#include <dwmapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <iterator>

// Windows 10 2004+; older SDK headers may not define it.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"DeadlockBuildHelperOverlay";

// Share of the screen width the panel uses (left of your team's scoreboard columns).
constexpr float kPanelWidth = 0.17f;

// A change needs a few frames to show: ImGui sizes a window from what it drew the frame before.
constexpr int kFramesPerChange = 3;

constexpr int kIconSize = 64;  // texture size; the panel never draws larger

// The part of a 280x380 hero card the scoreboard shows: its face.
const cv::Rect kHeroFace(0, 0, 280, 266);

constexpr ImU32 kRowColor = IM_COL32(255, 215, 0, 255);
constexpr ImU32 kPortraitColor = IM_COL32(0, 220, 255, 255);
constexpr ImU32 kHitColor = IM_COL32(80, 255, 120, 255);
constexpr ImU32 kHeroHitColor = IM_COL32(255, 80, 255, 255);
constexpr ImU32 kBannerBg = IM_COL32(0, 0, 0, 200);

ImFont* addFont(ImGuiIO& io, const char* file, float size)
{
    std::error_code ec;
    return std::filesystem::exists(file, ec) ? io.Fonts->AddFontFromFileTTF(file, size) : nullptr;
}

}  // namespace

Overlay::Overlay(RECT monitorRect)
    : rect_(monitorRect)
    , bounds_(monitorRect)
{
}

Overlay::~Overlay()
{
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    icons_.clear();
    preview_.Reset();
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Overlay::init()
{
    if (!createWindow() || !createDevice()) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                      // nothing to persist
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;    // the window never receives input anyway

    // Scale text and spacing with the monitor so the panel looks the same at every resolution.
    const float scale = std::max(0.7f, static_cast<float>(rect_.bottom - rect_.top) / 1080.0f);
    const float fontSize = 15.0f * scale;
    panelStyle_.scale = scale;
    panelStyle_.regular = addFont(io, "C:\\Windows\\Fonts\\segoeui.ttf", fontSize);
    panelStyle_.bold = addFont(io, "C:\\Windows\\Fonts\\segoeuib.ttf", fontSize);
    if (!panelStyle_.regular) {
        ImFontConfig font;
        font.SizePixels = fontSize;
        panelStyle_.regular = io.Fonts->AddFontDefault(&font);
    }
    if (!panelStyle_.bold) {
        panelStyle_.bold = panelStyle_.regular;
    }

    ImGui::StyleColorsDark();
    applyMenuStyle();
    ImGui::GetStyle().ScaleAllSizes(scale);

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    imguiReady_ = true;
    return true;
}

bool Overlay::createWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    const int width = rect_.right - rect_.left;
    const int height = rect_.bottom - rect_.top;

    // TOPMOST: above the game.  LAYERED + TRANSPARENT: see-through and click-through.
    // TOOLWINDOW: no taskbar button / Alt-Tab entry.  NOACTIVATE: never steals focus from the game.
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClassName, L"Deadlock Build Helper", WS_POPUP,
                            rect_.left, rect_.top, width, height,
                            nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) {
        return false;
    }

    // Opaque layered window + DWM "glass" over the whole client area: DWM then composites
    // using the back buffer's alpha channel, so pixels cleared to alpha 0 show the game.
    SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 255, LWA_ALPHA);
    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        std::cerr << "[overlay] could not exclude overlay from capture (needs Windows 10 2004+); "
                     "keep the panel outside enemy_roi\n";
    }

    // Starts hidden; render() shows it only while the panel is on screen.
    return true;
}

bool Overlay::createDevice()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // blt model: plays well with layered/DWM alpha

    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                               &sd, &swapChain_, &device_, nullptr, &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                           &sd, &swapChain_, &device_, nullptr, &context_);
    }
    if (FAILED(hr)) {
        std::cerr << "[overlay] D3D11 device creation failed, hr=0x" << std::hex << hr << std::dec << '\n';
        return false;
    }

    return createRenderTarget();
}

bool Overlay::createRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_));
}

void Overlay::setBounds(const RECT& bounds)
{
    if (EqualRect(&bounds, &bounds_)) {
        return;
    }
    bounds_ = bounds;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top, width, height, SWP_NOACTIVATE);
    swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

RECT Overlay::panelBounds(const OverlayModel& model) const
{
    const int monitorWidth = rect_.right - rect_.left;
    const int monitorHeight = rect_.bottom - rect_.top;
    const int left = rect_.left + static_cast<int>(monitorWidth * model.panelX);
    const int top = rect_.top + static_cast<int>(monitorHeight * model.panelY);
    return {left, top, std::min<LONG>(rect_.right, left + static_cast<LONG>(std::ceil(monitorWidth * kPanelWidth))),
            rect_.bottom};
}

void Overlay::setInteractive(bool interactive)
{
    if (interactive == interactive_) {
        return;
    }
    interactive_ = interactive;

    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    constexpr LONG_PTR kPassThrough = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    style = interactive ? (style & ~kPassThrough) : (style | kPassThrough);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ImGuiIO& io = ImGui::GetIO();
    if (interactive) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        previousForeground_ = GetForegroundWindow();
        setBounds(rect_);
        ShowWindow(hwnd_, SW_SHOW);
        windowShown_ = true;
        // Windows only lets the foreground thread hand over focus, and that is the game's. Sharing
        // its input state for a moment makes this allowed; nothing is sent to the game.
        const DWORD foregroundThread = GetWindowThreadProcessId(previousForeground_, nullptr);
        const DWORD thisThread = GetCurrentThreadId();
        AttachThreadInput(thisThread, foregroundThread, TRUE);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        AttachThreadInput(thisThread, foregroundThread, FALSE);
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        if (previousForeground_ && IsWindow(previousForeground_)) {
            SetForegroundWindow(previousForeground_);
        }
        previousForeground_ = nullptr;
    }
    drawnRevision_.clear();
}

bool Overlay::hasFocus() const
{
    return GetForegroundWindow() == hwnd_;
}

ImTextureID Overlay::setPreview(const cv::Mat& bgr)
{
    preview_.Reset();
    if (bgr.empty()) {
        return ImTextureID{};
    }
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(rgba.cols);
    desc.Height = static_cast<UINT>(rgba.rows);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{rgba.data, static_cast<UINT>(rgba.step), 0};
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &texture))) {
        device_->CreateShaderResourceView(texture.Get(), nullptr, &preview_);
    }
    return reinterpret_cast<ImTextureID>(preview_.Get());
}

void Overlay::setIconFolders(std::filesystem::path items, std::filesystem::path heroes)
{
    itemFolder_ = std::move(items);
    heroFolder_ = std::move(heroes);
    icons_.clear();
    drawnRevision_.clear();
}

ImTextureID Overlay::icon(const std::string& id, bool hero)
{
    const std::string key = (hero ? "hero:" : "item:") + id;
    if (const auto it = icons_.find(key); it != icons_.end()) {
        return reinterpret_cast<ImTextureID>(it->second.Get());
    }

    // Active items' shop art lives in its own folder: as a template it would only slow the search.
    const std::filesystem::path folder = hero ? heroFolder_ : itemFolder_;
    cv::Mat image = readImage(folder / (id + ".png"), cv::IMREAD_UNCHANGED);
    if (image.empty() && !hero) {
        image = readImage(folder.parent_path() / "icons" / (id + ".png"), cv::IMREAD_UNCHANGED);
    }
    if (image.empty() && !hero) {
        image = readImage(folder / (id + "@active.png"), cv::IMREAD_UNCHANGED);
    }

    ComPtr<ID3D11ShaderResourceView> view;
    if (!image.empty() && image.depth() == CV_8U && (image.channels() == 3 || image.channels() == 4)) {
        if (hero && image.size() == cv::Size(280, 380)) {
            image = image(kHeroFace);
        }
        cv::Mat rgba;
        cv::cvtColor(image, rgba, image.channels() == 4 ? cv::COLOR_BGRA2RGBA : cv::COLOR_BGR2RGBA);
        cv::resize(rgba, rgba, cv::Size(kIconSize, kIconSize), 0, 0, cv::INTER_AREA);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = kIconSize;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{rgba.data, static_cast<UINT>(rgba.step), 0};
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &texture))) {
            device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
        }
    }
    icons_[key] = view;  // also remembers a missing picture, so it is not looked for every frame
    return reinterpret_cast<ImTextureID>(view.Get());
}

bool Overlay::pumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            return false;
        }
    }
    return true;
}

void Overlay::keepOnTop()
{
    if (!windowShown_) {
        return;  // nothing to keep on top, and restacking windows is not free
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Overlay::render(const OverlayModel& model)
{
    const bool menuOpen = static_cast<bool>(model.menu);
    const bool hasContent = model.visible || model.calibrating || menuOpen || model.reminder.has_value();

    // Nothing on screen now and nothing last frame: skip the GPU work entirely.
    if (!hasContent && !lastFrameHadContent_) {
        Sleep(16);
        return;
    }
    // Same picture as already on screen: drawing it again would only make DWM recomposite the
    // desktop for nothing, once per monitor refresh.
    if (hasContent && !menuOpen && model.revision == drawnRevision_ && framesToDraw_ == 0) {
        Sleep(16);
        return;
    }
    if (hasContent && model.revision != drawnRevision_) {
        drawnRevision_ = model.revision;
        framesToDraw_ = kFramesPerChange;
    }
    if (hasContent) {
        // Only calibration needs the whole monitor. The smaller the window, the less DWM has to blend.
        setBounds(model.calibrating || menuOpen ? rect_ : panelBounds(model));
    }
    if (hasContent && !windowShown_) {
        ShowWindow(hwnd_, interactive_ ? SW_SHOW : SW_SHOWNOACTIVATE);
        keepOnTop();
        windowShown_ = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (menuOpen) {
        model.menu();
    } else if (hasContent) {
        drawPanelView(model);
    }
    if (model.calibrating && !menuOpen) {
        drawCalibration(model);
    }
    ImGui::Render();

    constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), transparent);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);  // vsync caps the loop at the refresh rate

    lastFrameHadContent_ = hasContent;
    framesToDraw_ = std::max(0, framesToDraw_ - 1);

    // A visible full-screen topmost window, even a fully transparent one, keeps the game out of
    // DWM's independent-flip path (more latency, lower FPS). Hide it once the cleared frame is up.
    if (!hasContent && windowShown_) {
        ShowWindow(hwnd_, SW_HIDE);
        windowShown_ = false;
        drawnRevision_.clear();
    }
}

void Overlay::drawPanelView(const OverlayModel& model)
{
    const float monitorWidth = static_cast<float>(rect_.right - rect_.left);
    const float monitorHeight = static_cast<float>(rect_.bottom - rect_.top);
    // In its own small window the panel starts at the window's corner.
    const ImVec2 pos = model.calibrating ? ImVec2(monitorWidth * model.panelX, monitorHeight * model.panelY) : ImVec2(0.0f, 0.0f);
    const PanelIcons icons{[this](const std::string& id) { return icon(id, false); },
                           [this](const std::string& id) { return icon(id, true); }};
    if (model.visible || model.calibrating) {
        drawPanel(model.panel, pos, monitorWidth * kPanelWidth, monitorHeight * (1.0f - model.panelY) - 2.0f, panelStyle_, icons);
    } else if (model.reminder) {
        drawReminder(*model.reminder, pos, monitorWidth * kPanelWidth, panelStyle_, icons);
    }
}

void Overlay::drawCalibration(const OverlayModel& model)
{
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    const float thickness = std::max(1.0f, ImGui::GetFontSize() / 8.0f);

    for (size_t i = 0; i < model.rows.size(); ++i) {
        const NormRect& r = model.rows[i];
        const ImVec2 a(static_cast<float>(r.x) * size.x, static_cast<float>(r.y) * size.y);
        const ImVec2 b(static_cast<float>(r.x + r.w) * size.x, static_cast<float>(r.y + r.h) * size.y);
        draw->AddRect(a, b, kRowColor, 0.0f, 0, thickness);
        const std::string label = std::format("#{}", i + 1);
        draw->AddText(ImVec2(a.x + 4.0f, a.y + 2.0f), kRowColor, label.c_str());
    }

    for (const auto& r : model.portraits) {
        const ImVec2 a(static_cast<float>(r.x) * size.x, static_cast<float>(r.y) * size.y);
        const ImVec2 b(static_cast<float>(r.x + r.w) * size.x, static_cast<float>(r.y + r.h) * size.y);
        draw->AddRect(a, b, kPortraitColor, 0.0f, 0, thickness);
    }

    for (const auto& d : model.detections) {
        const ImU32 color = d.isHero ? kHeroHitColor : kHitColor;
        const ImVec2 a(static_cast<float>(d.box.x), static_cast<float>(d.box.y));
        const ImVec2 b(static_cast<float>(d.box.x + d.box.width), static_cast<float>(d.box.y + d.box.height));
        draw->AddRect(a, b, color, 0.0f, 0, thickness);
        const std::string label = std::format("{} {:.2f}", d.itemId, d.score);
        draw->AddText(ImVec2(a.x, b.y + 1.0f), color, label.c_str());
    }

    const ImVec2 textSize = ImGui::CalcTextSize(model.banner.c_str());
    // Above the ability bar: the top of the screen holds the scoreboard columns being calibrated.
    const ImVec2 pos((size.x - textSize.x) * 0.5f, size.y * 0.76f);
    const float pad = ImGui::GetFontSize() * 0.5f;
    draw->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad), ImVec2(pos.x + textSize.x + pad, pos.y + textSize.y + pad),
                        kBannerBg, pad);
    draw->AddText(pos, kRowColor, model.banner.c_str());
}

LRESULT CALLBACK Overlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return 1;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
