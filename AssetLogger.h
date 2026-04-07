#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <algorithm>

// ============================================================================
// Asset Category
// ============================================================================
enum class AssetCategory : int {
    All = 0,
    Material,
    Niagara,
    Mesh,
    Texture,
    Animation,
    Sound,
    Wwise,
    Blueprint,
    Widget,
    Level,
    DataAsset,
    Hero,
    Ability,
    Other,
    COUNT
};

static const char* CategoryNames[] = {
    "ALL", "Material", "Niagara", "Mesh", "Texture",
    "Animation", "Sound", "Wwise", "Blueprint", "Widget",
    "Level", "DataAsset", "Hero", "Ability", "Other"
};

// ============================================================================
// Tracked Asset — shared between scanner and GUI
// ============================================================================
struct TrackedAsset {
    uintptr_t     address;
    std::string   className;
    std::string   objectName;
    std::string   fullPath;
    AssetCategory category;
    uint64_t      discoveryTimeMs;
    bool          bookmarked = false;
};

// ============================================================================
// Scanner — runs on its own thread, feeds assets to shared state
// ============================================================================
namespace AssetScanner {
    void Start();
    void Stop();

    // --- Shared state (lock gMutex before access) ---
    extern std::mutex                           gMutex;
    extern std::vector<TrackedAsset>            gAssets;
    extern std::unordered_set<uintptr_t>        gKnownObjs;
    extern std::unordered_map<int, int>         gCategoryStats;
    extern volatile bool                        gScanEnabled;
    extern volatile bool                        gRunning;
    extern volatile int                         gScanInterval;
    extern volatile bool                        gForceScan;
    extern int                                  gTotalGObjects;
    extern int                                  gBaselineCount;
    extern uint64_t                             gStartTime;
    extern bool                                 gCalibrated;

    void ResetBaseline();
    std::vector<TrackedAsset> GetHeuristicDependencies(uintptr_t objAddr);
}

// ============================================================================
// GUI — standalone ImGui window with D3D11
// ============================================================================
namespace AssetLoggerGUI {
    void Start();
    void Stop();
    bool IsRunning();
}
