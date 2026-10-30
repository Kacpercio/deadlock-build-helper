#pragma once

#include "Config.h"
#include "Types.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct OverlayModel {
    bool visible = false;      // the suggestion panel
    bool calibrating = false;  // row outlines + live detections + banner (implies the panel)
    std::string status;
    bool perPlayer = false;    // more than one enemy row configured: list items per enemy
    std::vector<EnemyItem> enemyItems;
    std::vector<EnemyPlayer> players;
    std::vector<Suggestion> suggestions;
    float panelX = 0.02f;  // panel top-left corner, normalized to the monitor
    float panelY = 0.30f;

    // Calibration only.
    std::vector<NormRect> rows;
    std::vector<NormRect> portraits;
    std::vector<Detection> detections;  // monitor pixels, same space as the overlay window
    std::string banner;
};

// Full-monitor, transparent, click-through, always-on-top window drawn with Dear ImGui on D3D11.
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

private:
    bool createWindow();
    bool createDevice();
    void drawPanel(const OverlayModel& model);
    void drawCalibration(const OverlayModel& model);
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    RECT rect_;
    HWND hwnd_ = nullptr;
    bool imguiReady_ = false;
    bool lastFrameHadContent_ = true;  // true so the very first frame clears the window
    bool windowShown_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
};
