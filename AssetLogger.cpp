#include "AssetLogger.h"
#include "../sdk_offsets.h"
#include <cstdio>

// ============================================================================
// AssetScanner — GObjects walker running on its own thread.
// Populates the shared gAssets vector for the GUI to display.
// ============================================================================

// Shared state definitions
std::mutex                           AssetScanner::gMutex;
std::vector<TrackedAsset>            AssetScanner::gAssets;
std::unordered_set<uintptr_t>        AssetScanner::gKnownObjs;
std::unordered_map<int, int>         AssetScanner::gCategoryStats;
volatile bool                        AssetScanner::gScanEnabled = true;
volatile bool                        AssetScanner::gRunning = true;
volatile int                         AssetScanner::gScanInterval = 100;
volatile bool                        AssetScanner::gForceScan = false;
int                                  AssetScanner::gTotalGObjects = 0;
int                                  AssetScanner::gBaselineCount = 0;
uint64_t                             AssetScanner::gStartTime = 0;
bool                                 AssetScanner::gCalibrated = false;

static uintptr_t gBase = 0;
static int gStride = 0, gLenShift = 1, gLenOffset = 0, gHdrSize = 2;

// --- Safe memory ---
static bool CanRead(uintptr_t addr, size_t sz = 8) {
    if (addr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}
static uintptr_t RPtr(uintptr_t a) { return CanRead(a) ? *(uintptr_t*)a : 0; }
static int32_t   RI32(uintptr_t a) { return CanRead(a, 4) ? *(int32_t*)a : 0; }

// --- FName resolution ---
static std::string ResolveFName(int32_t idx) {
    if (idx <= 0 || gStride == 0) return {};
    int block = idx >> 16, off = idx & 0xFFFF;
    if (block < 0 || block >= 8192) return {};
    uintptr_t pool = gBase + OFFSET_GNAMES;
    uintptr_t blk = RPtr(pool + 0x10 + block * 8);
    if (!blk) return {};
    uintptr_t entry = blk + (uintptr_t)off * gStride;
    if (!CanRead(entry, gHdrSize)) return {};
    uint16_t hdr = *(uint16_t*)(entry + gLenOffset);
    bool wide = hdr & 1;
    int len = hdr >> gLenShift;
    if (len <= 0 || len > 1024 || wide) return {};
    const char* s = (const char*)(entry + gHdrSize);
    if (!CanRead((uintptr_t)s, len)) return {};
    return std::string(s, len);
}

static std::string ObjectFName(uintptr_t obj) {
    if (!obj) return {};
    return ResolveFName(RI32(obj + OFF_UObject_FName));
}

static std::string ClassName(uintptr_t obj) {
    if (!obj) return {};
    uintptr_t cls = RPtr(obj + OFF_UObject_Class);
    return cls ? ResolveFName(RI32(cls + OFF_UObject_FName)) : std::string{};
}

static std::string GetObjectPath(uintptr_t obj) {
    if (!obj) return {};
    std::vector<std::string> parts;
    uintptr_t cur = obj;
    int depth = 0;
    while (cur && depth < 32) {
        std::string name = ObjectFName(cur);
        if (name.empty()) break;
        parts.push_back(name);
        cur = RPtr(cur + OFF_UObject_Outer);
        depth++;
    }
    std::string path;
    for (int i = (int)parts.size() - 1; i >= 0; i--) {
        if (path.empty() && parts[i] == "None") continue;
        if (!path.empty()) path += "/";
        path += parts[i];
    }
    
    // Clean up common UE prefixes
    if (path.compare(0, 6, "/Game/") == 0) {
        path = path.substr(6);
    } else if (path.compare(0, 5, "Game/") == 0) {
        path = path.substr(5);
    }
    return path;
}

// --- FName calibration ---
static bool CalibrateStride(uintptr_t knownObj) {
    int32_t clsFNameId = RI32(RPtr(knownObj + OFF_UObject_Class) + OFF_UObject_FName);
    int block = clsFNameId >> 16, offset = clsFNameId & 0xFFFF;
    uintptr_t pool = gBase + OFFSET_GNAMES;
    uintptr_t blk = RPtr(pool + 0x10 + block * 8);
    if (!blk) return false;
    struct Cfg { int stride, lenOff, strOff, lenShift; };
    Cfg configs[] = {
        {4,4,6,1},{2,0,2,1},{2,0,2,6},{4,4,6,6},{2,4,6,1},{4,0,2,1},{1,0,2,1},
    };
    for (auto& c : configs) {
        uintptr_t entry = blk + (uintptr_t)offset * c.stride;
        if (!CanRead(entry, c.strOff + 40)) continue;
        uint16_t hdr = *(uint16_t*)(entry + c.lenOff);
        int len = hdr >> c.lenShift;
        if ((hdr & 1) || len <= 0 || len > 200) continue;
        const char* str = (const char*)(entry + c.strOff);
        if (!CanRead((uintptr_t)str, len)) continue;
        bool ok = true;
        for (int i = 0; i < len; i++) {
            char ch = str[i];
            if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_')) { ok=false; break; }
        }
        if (ok && len >= 3) {
            gStride = c.stride; gLenShift = c.lenShift; gLenOffset = c.lenOff; gHdrSize = c.strOff;
            return true;
        }
    }
    return false;
}

// --- Asset classification ---
static AssetCategory ClassifyAsset(const std::string& cls, const std::string& obj, const std::string& path) {
    auto ic = [](const std::string& s, const char* sub) -> bool {
        std::string lo = s; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        std::string ls(sub); std::transform(ls.begin(), ls.end(), ls.begin(), ::tolower);
        return lo.find(ls) != std::string::npos;
    };
    if (ic(cls,"MaterialInstance")||ic(cls,"MaterialParameterCollection")||(ic(cls,"Material")&&!ic(cls,"PhysMat"))) return AssetCategory::Material;
    if (ic(cls,"Niagara")) return AssetCategory::Niagara;
    if (ic(cls,"StaticMesh")||ic(cls,"SkeletalMesh")||ic(cls,"MeshComponent")) return AssetCategory::Mesh;
    if (ic(cls,"Texture")||ic(cls,"RenderTarget")) return AssetCategory::Texture;
    if (ic(cls,"AnimSequence")||ic(cls,"AnimMontage")||ic(cls,"AnimBlueprint")||ic(cls,"AnimInstance")||ic(cls,"BlendSpace")) return AssetCategory::Animation;
    if (ic(cls,"AkAudio")||ic(cls,"AkMedia")||ic(cls,"AkComponent")) return AssetCategory::Wwise;
    if (ic(cls,"SoundWave")||ic(cls,"SoundCue")||ic(cls,"AudioComponent")) return AssetCategory::Sound;
    if (ic(cls,"BlueprintGeneratedClass")||(ic(cls,"Blueprint")&&!ic(cls,"Anim"))) return AssetCategory::Blueprint;
    if (ic(cls,"Widget")||ic(cls,"UserWidget")) return AssetCategory::Widget;
    if (ic(cls,"World")||ic(cls,"Level")||ic(cls,"MapBuildData")) return AssetCategory::Level;
    if (ic(cls,"DataAsset")||ic(cls,"DataTable")||ic(cls,"CurveFloat")) return AssetCategory::DataAsset;
    if (ic(obj,"MarvelBase")||ic(cls,"MarvelBase")||ic(cls,"MarvelCharacter")) return AssetCategory::Hero;
    if (ic(cls,"GameplayAbility")||ic(cls,"GameplayEffect")||ic(path,"Ability")) return AssetCategory::Ability;
    return AssetCategory::Other;
}

// --- Noise filter ---
static bool IsReflectionNoise(const std::string& cls) {
    static const char* skip[] = {
        "Function","ScriptStruct","Enum","Package","Class",
        "ObjectProperty","StructProperty","FloatProperty","IntProperty",
        "BoolProperty","ByteProperty","StrProperty","NameProperty",
        "TextProperty","ArrayProperty","MapProperty","SetProperty",
        "DelegateProperty","MulticastDelegateProperty",
        "MulticastInlineDelegateProperty","MulticastSparseDelegateProperty",
        "SoftObjectProperty","SoftClassProperty","ObjectRedirector",
        "ClassProperty","InterfaceProperty","EnumProperty",
        "Int64Property","UInt32Property","UInt64Property",
        "Int16Property","UInt16Property","Int8Property",
        "FieldPathProperty","LazyObjectProperty","WeakObjectProperty",
        "FunctionProperty",nullptr
    };
    for (int i = 0; skip[i]; i++) if (cls == skip[i]) return true;
    return false;
}

// --- GObjects access ---
static constexpr int OBJ_PER_CHUNK = 65536;
static constexpr int FUOBJ_SIZE = 0x18;

static int GetNumUObjects() {
    uintptr_t go = gBase + OFFSET_GOBJECTS;
    return CanRead(go + 0x14, 4) ? RI32(go + 0x14) : 0;
}

static uintptr_t GetUObjectByIndex(int index) {
    uintptr_t go = gBase + OFFSET_GOBJECTS;
    uintptr_t ca = RPtr(go);
    if (!ca) return 0;
    int ci = index / OBJ_PER_CHUNK, wi = index % OBJ_PER_CHUNK;
    uintptr_t chunk = RPtr(ca + ci * 8);
    if (!chunk) return 0;
    return RPtr(chunk + (uintptr_t)wi * FUOBJ_SIZE);
}

static void ScanForNewAssets() {
    int num = GetNumUObjects();
    if (num <= 0 || num > 10000000) return;
    AssetScanner::gTotalGObjects = num;
    uint64_t now = GetTickCount64() - AssetScanner::gStartTime;
    int added = 0;

    std::vector<TrackedAsset> localAdditions;
    std::unordered_map<int, int> localStats;

    for (int i = 0; i < num; i++) {
        uintptr_t obj = GetUObjectByIndex(i);
        if (!obj) continue;
        if (AssetScanner::gKnownObjs.count(obj)) continue;
        AssetScanner::gKnownObjs.insert(obj);

        std::string cls = ClassName(obj);
        if (cls.empty() || IsReflectionNoise(cls)) continue;
        std::string objName = ObjectFName(obj);
        if (objName.empty()) continue;

        std::string fullPath = GetObjectPath(obj);
        AssetCategory cat = ClassifyAsset(cls, objName, fullPath);

        TrackedAsset asset;
        asset.address = obj;
        asset.className = cls;
        asset.objectName = objName;
        asset.fullPath = fullPath;
        asset.category = cat;
        asset.discoveryTimeMs = now;
        asset.bookmarked = false;

        localAdditions.push_back(std::move(asset));
        localStats[(int)cat]++;

        // Cap at 5000 per pass to prevent complete thread stall on massive map load spikes
        if (++added > 5000) break;
    }

    if (!localAdditions.empty()) {
        std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
        for (auto& st : localStats) {
            AssetScanner::gCategoryStats[st.first] += st.second;
        }
        AssetScanner::gAssets.insert(AssetScanner::gAssets.end(),
            std::make_move_iterator(localAdditions.begin()),
            std::make_move_iterator(localAdditions.end()));
    }
}

// --- Main scanning thread ---
static DWORD WINAPI ScanThread(LPVOID) {
    gBase = (uintptr_t)GetModuleHandle(nullptr);
    AssetScanner::gStartTime = GetTickCount64();

    // Wait for GWorld
    uintptr_t gworldAddr = gBase + OFFSET_GWORLD;
    for (int i = 0; i < 120 && AssetScanner::gRunning; i++) {
        if (RPtr(gworldAddr)) break;
        Sleep(500);
    }
    if (!RPtr(gworldAddr)) return 1;
    Sleep(3000);

    uintptr_t world = RPtr(gworldAddr);
    if (!CalibrateStride(world)) return 1;
    AssetScanner::gCalibrated = true;

    // Baseline scan
    int num = GetNumUObjects();
    for (int i = 0; i < num; i++) {
        uintptr_t obj = GetUObjectByIndex(i);
        if (obj) AssetScanner::gKnownObjs.insert(obj);
    }
    AssetScanner::gBaselineCount = (int)AssetScanner::gKnownObjs.size();

    // Main loop
    while (AssetScanner::gRunning) {
        if (AssetScanner::gScanEnabled || AssetScanner::gForceScan) {
            ScanForNewAssets();
            AssetScanner::gForceScan = false;
        }
        for (int s = 0; s < AssetScanner::gScanInterval && AssetScanner::gRunning && !AssetScanner::gForceScan; s += 20) {
            Sleep(20);
        }
    }
    return 0;
}

void AssetScanner::ResetBaseline() {
    std::lock_guard<std::mutex> lk(gMutex);
    gKnownObjs.clear();
    gAssets.clear();
    gCategoryStats.clear();
    int num = GetNumUObjects();
    for (int i = 0; i < num; i++) {
        uintptr_t obj = GetUObjectByIndex(i);
        if (obj) gKnownObjs.insert(obj);
    }
    gBaselineCount = (int)gKnownObjs.size();
}

void AssetScanner::Start() {
    CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
}

void AssetScanner::Stop() {
    gRunning = false;
}

// --- Inspector: Heuristic Pointer Scanning ---
std::vector<TrackedAsset> AssetScanner::GetHeuristicDependencies(uintptr_t objAddr) {
    std::vector<TrackedAsset> deps;
    if (!objAddr) return deps;

    // Scan up to ~2KB of the object's instance memory
    // (most properties fit here; we avoid overscanning to prevent huge pauses)
    const int SCAN_BYTES = 2048;
    
    std::unordered_set<uintptr_t> foundPointers;
    for (int offset = 0x28; offset < SCAN_BYTES; offset += 8) {
        uintptr_t val = RPtr(objAddr + offset);
        if (val && val != objAddr && foundPointers.find(val) == foundPointers.end()) {
            foundPointers.insert(val);
        }
    }

    if (foundPointers.empty()) return deps;

    // Match found pointers against tracked assets safely
    std::lock_guard<std::mutex> lk(gMutex);
    for (auto& asset : gAssets) {
        if (foundPointers.find(asset.address) != foundPointers.end()) {
            // Ignore common noise unless it's specifically useful
            if (asset.category != AssetCategory::Other || asset.className.find("Property") == std::string::npos) {
                deps.push_back(asset); // copy
            }
        }
    }

    return deps;
}
