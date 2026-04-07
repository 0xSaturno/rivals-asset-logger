#include <windows.h>
#include "AssetLogger.h"

// ============================================================================
// MR Asset Logger — DLL Entry Point
// Launches the GObjects scanner thread and the ImGui GUI window.
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        AssetScanner::Start();
        AssetLoggerGUI::Start();
        return TRUE;

    case DLL_PROCESS_DETACH:
        AssetLoggerGUI::Stop();
        AssetScanner::Stop();
        break;
    }
    return TRUE;
}
