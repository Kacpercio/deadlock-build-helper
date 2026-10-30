#pragma once

#include "Config.h"
#include "PanelView.h"
#include "Types.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct OverlayModel {
    bool visible = false;      // the suggestion panel
    bool calibrating = false;  // row outlines + live detections + banner (implies the panel)
    std::function<void()> menu;  // set: the settings menu is open and takes the mouse and keyboard
    std::string revision;      // changes whenever the content below does; nothing is redrawn otherwise
    PanelModel panel;
    float panelX = 0.02f;  // panel top-left corner, normalized to the monitor
    float panelY = 0.30f;

    // Calibration only.
    std::vector<NormRect> rows;
    std::vector<NormRect> portraits;
    std::vector<Detection> detections;  // monitor pixels, same space as the overlay window
    std::string banner;
};

// Transparent, click-through, always-on-top window drawn with Dear ImGui on D3D11: just big enough
// for the panel, or the whole monitor while calibrating. It is hidden when there is nothing to show.
// It is excluded from screen capture, so it never shows up in its own scans (or in OBS/Discord).
class Overlay {
public:
    explicit Overlay(RECT monitorRect);
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init();

    // Returns false once the window has been closed.
    bool pumpMessages();

    void render(const OverlayModel& model);

    // Games sometimes push themselves above other topmost windows; call this periodically.
    void keepOnTop();

    // Where the panel finds its item and hero pictures.
    void setIconFolders(std::filesystem::path items, std::filesystem::path heroes);

    // Normally the window lets everything through to the game. For the settings menu it takes over
    // the mouse and keyboard, and gives them back afterwards.
    void setInteractive(bool interactive);
    bool hasFocus() const;

    // Uploads a BGR picture for the menu; replaces the previous one.
    ImTextureID setPreview(const cv::Mat& bgr);

private:
    bool createWindow();
    bool createDevice();
    bool createRenderTarget();
    void setBounds(const RECT& bounds);
    RECT panelBounds(const OverlayModel& model) const;
    ImTextureID icon(const std::string& id, bool hero);
    void drawPanelView(const OverlayModel& model);
    void drawCalibration(const OverlayModel& model);
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    RECT rect_;    // the monitor
    RECT bounds_;  // the window, in the same coordinates
    HWND hwnd_ = nullptr;
    HWND previousForeground_ = nullptr;
    bool interactive_ = false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview_;
    PanelStyle panelStyle_;
    std::filesystem::path itemFolder_;
    std::filesystem::path heroFolder_;
    std::map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> icons_;
    std::string drawnRevision_;
    int framesToDraw_ = 0;
    bool imguiReady_ = false;
    bool lastFrameHadContent_ = true;  // true so the very first frame clears the window
    bool windowShown_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
};
