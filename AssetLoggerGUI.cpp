#include "AssetLogger.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")

// ============================================================================
// AssetLoggerGUI — Standalone ImGui window with D3D11 renderer
// ============================================================================

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// D3D11 resources
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static HWND                     g_hWnd = nullptr;
static WNDCLASSEXW              g_wc = {};
static bool                     g_guiRunning = false;

// GUI state
static char                     g_searchBuf[256] = {};
static bool                     g_categoryEnabled[(int)AssetCategory::COUNT] = {}; // per-category toggle
static bool                     g_filtersInitialized = false;
static bool                     g_autoScroll = true;
static bool                     g_showBookmarks = false;
static bool                     g_showStats = true;
static bool                     g_showInspector = false;
static TrackedAsset             g_inspectedAsset;
static std::vector<TrackedAsset> g_inspectedDeps;
static std::vector<TrackedAsset> g_displayAssets;  // local snapshot for rendering

static void InitFilters() {
    if (!g_filtersInitialized) {
        for (int i = 0; i < (int)AssetCategory::COUNT; i++) g_categoryEnabled[i] = true;
        g_filtersInitialized = true;
    }
}

// --- D3D11 helpers ---
static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)  { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)         { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        if (g_pd3dDevice) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Category colors for ImGui ---
static ImVec4 CategoryColor(AssetCategory cat) {
    switch (cat) {
    case AssetCategory::Material:   return ImVec4(1.00f, 0.85f, 0.20f, 1.0f); // Gold
    case AssetCategory::Niagara:    return ImVec4(0.30f, 0.90f, 1.00f, 1.0f); // Cyan
    case AssetCategory::Mesh:       return ImVec4(0.40f, 1.00f, 0.40f, 1.0f); // Green
    case AssetCategory::Texture:    return ImVec4(0.90f, 0.45f, 0.90f, 1.0f); // Magenta
    case AssetCategory::Animation:  return ImVec4(0.45f, 0.55f, 1.00f, 1.0f); // Blue
    case AssetCategory::Sound:      return ImVec4(1.00f, 0.60f, 0.20f, 1.0f); // Orange
    case AssetCategory::Wwise:      return ImVec4(1.00f, 0.30f, 0.70f, 1.0f); // Hot Pink
    case AssetCategory::Blueprint:  return ImVec4(1.00f, 0.35f, 0.35f, 1.0f); // Red
    case AssetCategory::Widget:     return ImVec4(0.50f, 0.80f, 0.80f, 1.0f); // Teal
    case AssetCategory::Level:      return ImVec4(0.85f, 0.40f, 0.40f, 1.0f); // Salmon
    case AssetCategory::DataAsset:  return ImVec4(0.35f, 0.75f, 0.35f, 1.0f); // Dark Green
    case AssetCategory::Hero:       return ImVec4(1.00f, 1.00f, 1.00f, 1.0f); // Bright White
    case AssetCategory::Ability:    return ImVec4(0.80f, 0.40f, 0.80f, 1.0f); // Purple
    default:                        return ImVec4(0.70f, 0.70f, 0.70f, 1.0f); // Gray
    }
}

// --- Category badge with colored background ---
static void DrawCategoryBadge(AssetCategory cat) {
    ImVec4 col = CategoryColor(cat);
    ImVec4 bgCol = ImVec4(col.x * 0.20f, col.y * 0.20f, col.z * 0.20f, 0.80f);
    const char* name = CategoryNames[(int)cat];

    ImVec2 textSize = ImGui::CalcTextSize(name);
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float pad = 4.0f;
    ImVec2 rectMin = ImVec2(cursorPos.x, cursorPos.y + 1);
    ImVec2 rectMax = ImVec2(cursorPos.x + textSize.x + pad * 2, cursorPos.y + textSize.y + 3);
    dl->AddRectFilled(rectMin, rectMax, ImGui::ColorConvertFloat4ToU32(bgCol), 3.0f);
    dl->AddRect(rectMin, rectMax, ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, 0.4f)), 3.0f);

    ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + pad, cursorPos.y + 2));
    ImGui::TextColored(col, "%s", name);
}

// --- Export to CSV ---
static void ExportCSV() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string csvPath(path);
    size_t sl = csvPath.find_last_of("\\/");
    if (sl != std::string::npos) csvPath = csvPath.substr(0, sl + 1);
    SYSTEMTIME st; GetLocalTime(&st);
    char fname[128];
    snprintf(fname, sizeof(fname), "AssetLog_%04d%02d%02d_%02d%02d%02d.csv",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    csvPath += fname;
    std::ofstream f(csvPath);
    if (!f.is_open()) return;
    f << "Timestamp_ms,Category,ClassName,ObjectName,FullPath,Address,Bookmarked\n";
    for (auto& a : g_displayAssets) {
        f << a.discoveryTimeMs << "," << CategoryNames[(int)a.category] << ","
          << a.className << "," << a.objectName << "," << a.fullPath << ","
          << "0x" << std::hex << a.address << std::dec << ","
          << (a.bookmarked ? "yes" : "no") << "\n";
    }
    f.close();
}

// --- Apply custom dark theme ---
static void SetupTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base dark colors with subtle blue tint
    colors[ImGuiCol_WindowBg]            = ImVec4(0.06f, 0.06f, 0.09f, 1.00f);
    colors[ImGuiCol_ChildBg]             = ImVec4(0.07f, 0.07f, 0.11f, 1.00f);
    colors[ImGuiCol_PopupBg]             = ImVec4(0.08f, 0.08f, 0.12f, 0.96f);
    colors[ImGuiCol_Border]              = ImVec4(0.20f, 0.20f, 0.28f, 0.60f);
    colors[ImGuiCol_BorderShadow]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]             = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]      = ImVec4(0.15f, 0.15f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive]       = ImVec4(0.18f, 0.18f, 0.28f, 1.00f);
    colors[ImGuiCol_TitleBg]             = ImVec4(0.05f, 0.05f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive]       = ImVec4(0.08f, 0.08f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]    = ImVec4(0.04f, 0.04f, 0.06f, 1.00f);
    colors[ImGuiCol_MenuBarBg]           = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]         = ImVec4(0.05f, 0.05f, 0.08f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]       = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.35f, 0.35f, 0.50f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.45f, 0.65f, 1.00f);
    colors[ImGuiCol_CheckMark]           = ImVec4(0.40f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]          = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]    = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]              = ImVec4(0.14f, 0.14f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered]       = ImVec4(0.20f, 0.20f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonActive]        = ImVec4(0.25f, 0.25f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]              = ImVec4(0.12f, 0.12f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered]       = ImVec4(0.18f, 0.18f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive]        = ImVec4(0.22f, 0.22f, 0.38f, 1.00f);
    colors[ImGuiCol_Separator]           = ImVec4(0.20f, 0.20f, 0.28f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]    = ImVec4(0.30f, 0.50f, 0.80f, 0.80f);
    colors[ImGuiCol_SeparatorActive]     = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
    colors[ImGuiCol_Tab]                 = ImVec4(0.10f, 0.10f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered]          = ImVec4(0.22f, 0.22f, 0.35f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]       = ImVec4(0.10f, 0.10f, 0.16f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]   = ImVec4(0.18f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_TableBorderLight]    = ImVec4(0.14f, 0.14f, 0.20f, 1.00f);
    colors[ImGuiCol_TableRowBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]       = ImVec4(0.08f, 0.08f, 0.12f, 0.40f);
    colors[ImGuiCol_TextSelectedBg]      = ImVec4(0.25f, 0.45f, 0.80f, 0.50f);

    // Rounding
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 4.0f;

    // Padding
    style.WindowPadding  = ImVec2(10, 10);
    style.FramePadding   = ImVec2(8, 4);
    style.ItemSpacing    = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.ScrollbarSize  = 14.0f;
}

// --- Render one frame ---
static void RenderFrame() {
    ImGuiIO& io = ImGui::GetIO();

    // Snapshot assets from scanner (minimize lock time)
    {
        std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
        g_displayAssets = AssetScanner::gAssets;
    }

    // --- Menu bar ---
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Export CSV", "Ctrl+E")) ExportCSV();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                g_guiRunning = false;
                PostMessage(g_hWnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats Panel", nullptr, &g_showStats);
            ImGui::MenuItem("Bookmarks Panel", nullptr, &g_showBookmarks);
            ImGui::MenuItem("Inspector Panel", nullptr, &g_showInspector);
            ImGui::MenuItem("Auto-scroll", nullptr, &g_autoScroll);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Full-window ImGui panel
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - ImGui::GetFrameHeight()));
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ===================== TOOLBAR =====================
    {
        // Scan toggle button
        bool scanning = AssetScanner::gScanEnabled;
        if (scanning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.20f, 0.20f, 1.0f));
        }
        if (ImGui::Button(scanning ? "  Scanning  " : "  Paused  ", ImVec2(100, 28))) {
            AssetScanner::gScanEnabled = !AssetScanner::gScanEnabled;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("Force Refresh", ImVec2(120, 28))) {
            AssetScanner::gForceScan = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset Baseline", ImVec2(140, 28))) {
            AssetScanner::ResetBaseline();
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear Log", ImVec2(100, 28))) {
            std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
            AssetScanner::gAssets.clear();
            AssetScanner::gCategoryStats.clear();
        }

        ImGui::SameLine();
        if (ImGui::Button("Export CSV", ImVec2(100, 28))) {
            ExportCSV();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        int interval = AssetScanner::gScanInterval;
        if (ImGui::SliderInt("##interval", &interval, 10, 5000, "%d ms")) {
            AssetScanner::gScanInterval = interval;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scan Interval (ms)");

        // Status indicator
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 330);
        if (!AssetScanner::gCalibrated) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Waiting for game...");
        }
        else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f),
                "GObjects: %d | Baseline: %d",
                AssetScanner::gTotalGObjects.load(std::memory_order_relaxed),
                AssetScanner::gBaselineCount);
        }
    }

    ImGui::Spacing();

    // ===================== FILTER BAR =====================
    {
        InitFilters();

        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##search", "Search assets (name, class, path)...", g_searchBuf, sizeof(g_searchBuf));

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "  %d assets", (int)g_displayAssets.size());

        // Category checkboxes row — colored, compact
        ImGui::Spacing();

        // All / None quick toggles
        if (ImGui::SmallButton("All")) {
            for (int i = 1; i < (int)AssetCategory::COUNT; i++) g_categoryEnabled[i] = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) {
            for (int i = 1; i < (int)AssetCategory::COUNT; i++) g_categoryEnabled[i] = false;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.38f, 1.0f), "|");
        ImGui::SameLine();

        // Individual category checkboxes with category colors
        for (int i = 1; i < (int)AssetCategory::COUNT; i++) {
            ImVec4 col = CategoryColor((AssetCategory)i);
            ImVec4 dimCol = ImVec4(col.x * 0.5f, col.y * 0.5f, col.z * 0.5f, 1.0f);

            // Color the checkmark to match category
            ImGui::PushStyleColor(ImGuiCol_CheckMark, col);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(col.x * 0.10f, col.y * 0.10f, col.z * 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(col.x * 0.20f, col.y * 0.20f, col.z * 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(col.x * 0.25f, col.y * 0.25f, col.z * 0.25f, 1.0f));

            // Label color: bright if enabled, dim if disabled
            ImVec4 labelCol = g_categoryEnabled[i] ? col : dimCol;
            ImGui::PushStyleColor(ImGuiCol_Text, labelCol);

            ImGui::Checkbox(CategoryNames[i], &g_categoryEnabled[i]);

            ImGui::PopStyleColor(5);

            if (i < (int)AssetCategory::COUNT - 1) {
                if (i == 9) {
                    ImGui::NewLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 24.0f);
                } else {
                    ImGui::SameLine();
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ===================== STATS BAR =====================
    if (g_showStats && AssetScanner::gCalibrated) {
        // Horizontal stats with colored category counts
        ImGui::BeginChild("##stats", ImVec2(0, 30), false);
        for (int i = 1; i < (int)AssetCategory::COUNT; i++) {
            int count = 0;
            {
                std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
                auto it = AssetScanner::gCategoryStats.find(i);
                if (it != AssetScanner::gCategoryStats.end()) count = it->second;
            }
            if (count > 0) {
                ImVec4 col = CategoryColor((AssetCategory)i);
                ImGui::TextColored(col, "%s:%d", CategoryNames[i], count);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.30f, 1.0f), "|");
                ImGui::SameLine();
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    // Build filtered list
    std::string searchStr(g_searchBuf);
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

    std::vector<int> filteredIdx;
    filteredIdx.reserve(g_displayAssets.size());
    for (int i = 0; i < (int)g_displayAssets.size(); i++) {
        auto& a = g_displayAssets[i];
        // Category filter — check if this category's checkbox is enabled
        int catIdx = (int)a.category;
        if (catIdx > 0 && catIdx < (int)AssetCategory::COUNT && !g_categoryEnabled[catIdx]) continue;
        // Search filter
        if (!searchStr.empty()) {
            std::string combined = a.className + " " + a.objectName + " " + a.fullPath;
            std::transform(combined.begin(), combined.end(), combined.begin(), ::tolower);
            if (combined.find(searchStr) == std::string::npos) continue;
        }
        filteredIdx.push_back(i);
    }

    // ===================== BOOKMARKS PANEL (optional) =====================
    if (g_showBookmarks) {
        ImGui::BeginChild("##bookmarks", ImVec2(0, 120), true);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Bookmarks");
        ImGui::Separator();
        for (auto& a : g_displayAssets) {
            if (!a.bookmarked) continue;
            ImGui::PushID((int)(uintptr_t)&a);
            ImVec4 col = CategoryColor(a.category);
            ImGui::TextColored(col, "[%s]", CategoryNames[(int)a.category]);
            ImGui::SameLine();
            ImGui::TextUnformatted(a.fullPath.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                ImGui::OpenPopup("bm_ctx");
            }
            if (ImGui::BeginPopup("bm_ctx")) {
                if (ImGui::MenuItem("Copy Path")) {
                    ImGui::SetClipboardText(a.fullPath.c_str());
                }
                if (ImGui::MenuItem("Remove Bookmark")) {
                    // Find and un-bookmark in shared state
                    std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
                    for (auto& sa : AssetScanner::gAssets) {
                        if (sa.address == a.address) { sa.bookmarked = false; break; }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    // ===================== MAIN ASSET TABLE =====================
    float tableHeight = ImGui::GetContentRegionAvail().y;
    
    // If Inspector is active, give it 350px on the right
    float inspectorWidth = 380.0f;
    if (g_showInspector) {
        ImGui::BeginChild("##table_child", ImVec2(ImGui::GetContentRegionAvail().x - inspectorWidth - 10, tableHeight), false);
    }

    // ===================== MAIN ASSET TABLE =====================
    ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##assets", 5, tableFlags, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Full Path",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Use clipper for performance with large lists
        ImGuiListClipper clipper;
        clipper.Begin((int)filteredIdx.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                auto& asset = g_displayAssets[filteredIdx[row]];
                ImGui::TableNextRow();
                
                if (asset.bookmarked) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.35f, 0.30f, 0.10f, 0.6f)));
                }
                
                ImGui::PushID(row);

                // Time column
                ImGui::TableNextColumn();
                uint64_t ms = asset.discoveryTimeMs;
                uint64_t sec = ms / 1000;
                uint64_t msr = ms % 1000;
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f),
                    "%02llu:%02llu.%03llu", (unsigned long long)(sec / 60),
                    (unsigned long long)(sec % 60), (unsigned long long)msr);

                // Category column (colored badge)
                ImGui::TableNextColumn();
                DrawCategoryBadge(asset.category);

                // Class column
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.80f, 1.0f), "%s", asset.className.c_str());

                // Name column
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(asset.objectName.c_str());

                // Full Path column
                ImGui::TableNextColumn();
                // Selectable row support for inspector double clicks
                if (ImGui::Selectable(asset.fullPath.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        g_inspectedAsset = asset;
                        g_inspectedDeps = AssetScanner::GetHeuristicDependencies(asset.address);
                        g_showInspector = true;
                    }
                }

                // Right-click context menu on path
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                    ImGui::OpenPopup("asset_ctx");
                }
                if (ImGui::BeginPopup("asset_ctx")) {
                    if (ImGui::MenuItem("Open In Asset Inspector")) {
                        g_inspectedAsset = asset;
                        g_inspectedDeps = AssetScanner::GetHeuristicDependencies(asset.address);
                        g_showInspector = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy Full Path")) {
                        ImGui::SetClipboardText(asset.fullPath.c_str());
                    }
                    if (ImGui::MenuItem("Copy Class Name")) {
                        ImGui::SetClipboardText(asset.className.c_str());
                    }
                    if (ImGui::MenuItem("Copy Object Name")) {
                        ImGui::SetClipboardText(asset.objectName.c_str());
                    }
                    ImGui::Separator();
                    bool bm = asset.bookmarked;
                    if (ImGui::MenuItem(bm ? "Remove Bookmark" : "Bookmark")) {
                        std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
                        for (auto& sa : AssetScanner::gAssets) {
                            if (sa.address == asset.address) {
                                sa.bookmarked = !sa.bookmarked;
                                break;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }
        clipper.End();

        // Auto-scroll to bottom
        if (g_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndTable();
    }
    
    if (g_showInspector) {
        ImGui::EndChild(); // end table child
        ImGui::SameLine();
        
        // ===================== ASSET INSPECTOR =====================
        ImGui::BeginChild("##inspector", ImVec2(inspectorWidth, tableHeight), true);
        
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.8f, 1.0f), "Asset Inspector");
        ImGui::SameLine(ImGui::GetWindowWidth() - 30);
        if (ImGui::Button("X")) {
            g_showInspector = false;
        }
        ImGui::Separator();
        
        if (g_inspectedAsset.address == 0) {
            ImGui::TextDisabled("Double-click an asset to inspect.");
        } else {
            ImVec4 catColor = CategoryColor(g_inspectedAsset.category);
            ImGui::TextColored(catColor, "[%s] %s", CategoryNames[(int)g_inspectedAsset.category], g_inspectedAsset.className.c_str());
            ImGui::TextWrapped("%s", g_inspectedAsset.objectName.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Address:"); ImGui::SameLine();
            ImGui::Text("0x%llX", (unsigned long long)g_inspectedAsset.address);
            ImGui::TextDisabled("Path:"); 
            ImGui::TextWrapped("%s", g_inspectedAsset.fullPath.c_str());
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Heuristic Dependencies");
            ImGui::TextDisabled("(Scanned from memory pointers)");
            ImGui::Spacing();
            
            if (g_inspectedDeps.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No tracked dependencies found.");
            } else {
                for (auto& dep : g_inspectedDeps) {
                    ImVec4 depCol = CategoryColor(dep.category);
                    ImGui::PushID((int)dep.address); // Add unique ID inside the loop
                    if (ImGui::Selectable(dep.objectName.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        // Recursively inspect
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            TrackedAsset copy = dep;
                            g_inspectedAsset = copy;
                            g_inspectedDeps = AssetScanner::GetHeuristicDependencies(copy.address);
                            ImGui::PopID(); // Pop ID before breaking to prevent ImGui assert crash
                            break; // iterator invalidated, break out of loop
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextColored(depCol, "[%s] %s", CategoryNames[(int)dep.category], dep.className.c_str());
                        ImGui::Text("Path: %s", dep.fullPath.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
        }
        
        ImGui::EndChild();
    }

    ImGui::End();
}

// --- GUI Thread ---
static DWORD WINAPI GUIThread(LPVOID) {
    // Register window class
    g_wc = {};
    g_wc.cbSize = sizeof(WNDCLASSEXW);
    g_wc.style = CS_CLASSDC;
    g_wc.lpfnWndProc = WndProc;
    g_wc.hInstance = GetModuleHandle(nullptr);
    g_wc.lpszClassName = L"AssetLoggerGUI";
    g_wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&g_wc);

    // Create window
    g_hWnd = CreateWindowExW(
        0, g_wc.lpszClassName,
        L"MR Asset Logger",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1400, 800,
        nullptr, nullptr, g_wc.hInstance, nullptr);

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
        return 1;
    }

    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Scale font size for clarity
    io.Fonts->AddFontDefault();
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 16.0f;
    io.Fonts->AddFontDefault(&fontCfg);

    SetupTheme();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Use the larger font
    io.FontDefault = io.Fonts->Fonts[1];

    g_guiRunning = true;
    ImVec4 clearColor = ImVec4(0.04f, 0.04f, 0.06f, 1.00f);

    // Main loop
    MSG msg;
    while (g_guiRunning) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_guiRunning = false;
            }
        }
        if (!g_guiRunning) break;

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderFrame();

        // Render
        ImGui::Render();
        const float clear_color_with_alpha[4] = {
            clearColor.x * clearColor.w, clearColor.y * clearColor.w,
            clearColor.z * clearColor.w, clearColor.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);

    return 0;
}

void AssetLoggerGUI::Start() {
    CreateThread(nullptr, 0, GUIThread, nullptr, 0, nullptr);
}

void AssetLoggerGUI::Stop() {
    g_guiRunning = false;
    if (g_hWnd) PostMessage(g_hWnd, WM_CLOSE, 0, 0);
}

bool AssetLoggerGUI::IsRunning() {
    return g_guiRunning;
}
