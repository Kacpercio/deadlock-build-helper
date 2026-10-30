#include "Overlay.h"

#include <dwmapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
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

const ImVec4 kAccent{1.00f, 0.78f, 0.35f, 1.0f};
const ImVec4 kEnemy{1.00f, 0.45f, 0.40f, 1.0f};
const ImVec4 kCounter{0.45f, 0.90f, 0.55f, 1.0f};

const ImVec4 kTip{0.70f, 0.78f, 1.00f, 1.0f};

// How many tips per enemy fit in the panel without crowding it.
constexpr size_t kMaxTipsPerPlayer = 2;

// Share of the screen width the panel may use (left of your team's scoreboard columns).
constexpr float kPanelMaxWidth = 0.17f;

constexpr ImU32 kRowColor = IM_COL32(255, 215, 0, 255);
constexpr ImU32 kPortraitColor = IM_COL32(0, 220, 255, 255);
constexpr ImU32 kHitColor = IM_COL32(80, 255, 120, 255);
constexpr ImU32 kHeroHitColor = IM_COL32(255, 80, 255, 255);
constexpr ImU32 kBannerBg = IM_COL32(0, 0, 0, 200);

std::string join(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += ", ";
        }
        out += p;
    }
    return out;
}

}  // namespace

Overlay::Overlay(RECT monitorRect)
    : rect_(monitorRect)
{
}

Overlay::~Overlay()
{
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
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

    // Scale text and spacing with the monitor so 1440p/4K stay readable.
    const float scale = std::max(1.0f, static_cast<float>(rect_.bottom - rect_.top) / 1080.0f);
    ImFontConfig font;
    font.SizePixels = 15.0f * scale;
    io.Fonts->AddFontDefault(&font);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.ScaleAllSizes(scale);

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

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_));
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
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Overlay::render(const OverlayModel& model)
{
    const bool hasContent = model.visible || model.calibrating;

    // Nothing on screen now and nothing last frame: skip the GPU work entirely.
    if (!hasContent && !lastFrameHadContent_) {
        Sleep(16);
        return;
    }
    if (hasContent && !windowShown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        keepOnTop();
        windowShown_ = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (hasContent) {
        drawPanel(model);
    }
    if (model.calibrating) {
        drawCalibration(model);
    }
    ImGui::Render();

    constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), transparent);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);  // vsync caps the loop at the refresh rate

    lastFrameHadContent_ = hasContent;

    // A visible full-screen topmost window, even a fully transparent one, keeps the game out of
    // DWM's independent-flip path (more latency, lower FPS). Hide it once the cleared frame is up.
    if (!hasContent && windowShown_) {
        ShowWindow(hwnd_, SW_HIDE);
        windowShown_ = false;
    }
}

void Overlay::drawPanel(const OverlayModel& model)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // At most 17% of the screen wide: in a 6v6 match your team's scoreboard columns start at
    // about x = 17%, and the panel must not cover them.
    const float maxWidth = viewport->Size.x * kPanelMaxWidth;
    const float wrapWidth = maxWidth - 2.0f * ImGui::GetStyle().WindowPadding.x - ImGui::GetStyle().IndentSpacing;

    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x * model.panelX, viewport->Size.y * model.panelY), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(maxWidth, viewport->Size.y * 0.85f));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##build_helper", nullptr, flags)) {
        ImGui::TextColored(kAccent, "BUILD HELPER");
        ImGui::SameLine();
        ImGui::PushTextWrapPos(maxWidth - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("%s", model.status.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        if (model.perPlayer) {
            ImGui::TextColored(kEnemy, "Enemies");
            if (model.players.empty()) {
                ImGui::TextDisabled("  none detected");
            }
            for (const auto& player : model.players) {
                // "#3 Haze - Gun build (W5 S1 V2)"
                std::string header = std::format("#{}", player.slot + 1);
                header += " " + (player.hero.empty() ? std::string("(hero unknown)") : player.hero);
                if (!player.build.empty()) {
                    header += std::format(" - {} (W{} S{} V{})", player.build, player.weaponItems,
                                          player.spiritItems, player.vitalityItems);
                }
                ImGui::TextColored(kEnemy, "%s", header.c_str());

                ImGui::Indent();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
                if (player.items.empty()) {
                    ImGui::TextDisabled("no items yet");
                } else {
                    ImGui::TextUnformatted(join(player.items).c_str());
                }
                for (size_t t = 0; t < player.tips.size() && t < kMaxTipsPerPlayer; ++t) {
                    ImGui::TextColored(kTip, "> %s", player.tips[t].c_str());
                }
                if (!player.avoid.empty()) {
                    ImGui::TextDisabled("avoid: %s", join(player.avoid).c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::Unindent();
            }
        } else {
            ImGui::TextColored(kEnemy, "Enemy items");
            if (model.enemyItems.empty()) {
                ImGui::TextDisabled("  none detected");
            }
            for (const auto& item : model.enemyItems) {
                if (item.count > 1) {
                    ImGui::BulletText("%s  x%d", item.displayName.c_str(), item.count);
                } else {
                    ImGui::BulletText("%s", item.displayName.c_str());
                }
            }
            // One search area for the whole team: show its tips together.
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            for (const auto& player : model.players) {
                for (size_t t = 0; t < player.tips.size() && t < 3 * kMaxTipsPerPlayer; ++t) {
                    ImGui::TextColored(kTip, "> %s", player.tips[t].c_str());
                }
            }
            ImGui::PopTextWrapPos();
        }

        ImGui::Spacing();
        ImGui::TextColored(kCounter, "Suggested counters");
        if (model.suggestions.empty()) {
            ImGui::TextDisabled("  nothing to counter yet");
        }
        for (const auto& s : model.suggestions) {
            ImGui::Bullet();
            ImGui::TextColored(kCounter, "%s", s.item.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("+%d  vs %s", s.score, join(s.answers).c_str());
            if (!s.reason.empty()) {
                ImGui::Indent();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
                ImGui::TextDisabled("%s", s.reason.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent();
            }
        }
    }
    ImGui::End();
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
