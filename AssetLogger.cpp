#include "AssetLogger.h"
#include "rivals_offsets.h"
#include "sdk_offsets.h"

// ============================================================================
// AssetScanner - GObjects walker running on its own thread.
// Populates the shared gAssets vector for the GUI to display.
// ============================================================================

// Shared state definitions
static std::mutex gKnownMutex; // guards gKnownObjs independently
std::mutex                           AssetScanner::gMutex;
std::vector<TrackedAsset>            AssetScanner::gAssets;
std::unordered_set<uintptr_t>        AssetScanner::gKnownObjs;
std::unordered_map<int, int>         AssetScanner::gCategoryStats;
std::atomic<bool>                    AssetScanner::gScanEnabled{ true };
std::atomic<bool>                    AssetScanner::gRunning{ true };
std::atomic<bool>                    AssetScanner::gForceScan{ false };
std::atomic<int>                     AssetScanner::gScanInterval{ 100 };
std::atomic<int>                     AssetScanner::gTotalGObjects{ 0 };
int                                  AssetScanner::gBaselineCount = 0;
uint64_t                             AssetScanner::gStartTime = 0;
bool                                 AssetScanner::gCalibrated = false;

static uintptr_t gBase = 0;
static uintptr_t gOffsetGWorld = 0;
static uintptr_t gOffsetFNamePool = 0;
static uintptr_t gOffsetGObjects = 0;
static uintptr_t gFNameAppend = 0;
static uintptr_t gFNamePool = 0;
static int gNameMul = 0;
static int gLenShift = 0;
static int gLenOffset = 0;
static int gStrOffset = 0;

static uintptr_t RPtr(uintptr_t a);

// --- Safe memory ---
static bool CanRead(uintptr_t addr, size_t sz = 8) {
    if (addr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (addr + sz < addr || addr + sz > regionEnd) return false;
    return true;
}
static uintptr_t RPtr(uintptr_t a) { return CanRead(a) ? *(uintptr_t*)a : 0; }
static int32_t   RI32(uintptr_t a) { return CanRead(a, 4) ? *(int32_t*)a : 0; }

// --- FName resolution ---
struct FNameValue {
    int32_t ComparisonIndex = 0;
    int32_t Number = 0;
};

struct FStringValue {
    wchar_t* Data = nullptr;
    int32_t Count = 0;
    int32_t Max = 0;
};

using FNameAppendStringFn = void(__fastcall*)(const FNameValue*, FStringValue*);

struct FNameCfg { int mul, lenOff, strOff, lenShift; };

static FNameCfg gFNameConfigs[] = {
    {2, 0, 2, 6},
    {1, 0, 2, 6},
    {2, 0, 2, 1},
    {4, 4, 6, 1},
    {4, 4, 6, 6},
    {2, 4, 6, 1},
    {4, 0, 2, 1},
    {1, 0, 2, 1},
};

static std::string NarrowFromWide(const wchar_t* s, int len) {
    if (!s || len <= 0) return {};

    int needed = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out;
    out.resize((size_t)needed);
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), needed, nullptr, nullptr);
    return out;
}

static uintptr_t GetFNameBlock(uintptr_t pool, int block) {
    if (!pool || block < 0 || block >= 8192) return 0;
    return RPtr(pool + 0x10 + block * 8);
}

static bool TryDecodeFNamePool(const FNameValue& name, const FNameCfg& cfg, std::string* out = nullptr) {
    if (name.ComparisonIndex <= 0 || !gFNamePool) return false;

    int block = name.ComparisonIndex >> 16;
    int off = name.ComparisonIndex & 0xFFFF;
    uintptr_t blk = GetFNameBlock(gFNamePool, block);
    if (!blk) return false;

    uintptr_t entry = blk + (uintptr_t)off * cfg.mul;
    if (!CanRead(entry, cfg.strOff + 256)) return false;

    uint16_t hdr = *(uint16_t*)(entry + cfg.lenOff);
    if (hdr & 1) return false;

    int len = hdr >> cfg.lenShift;
    if (len <= 0 || len > 200) return false;

    const char* s = (const char*)(entry + cfg.strOff);
    if (!CanRead((uintptr_t)s, (size_t)len)) return false;

    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            return false;
        }
    }

    if (out) {
        *out = std::string(s, len);
        if (name.Number > 0) {
            *out += "_";
            *out += std::to_string(name.Number - 1);
        }
    }
    return len >= 3;
}

static std::string ResolveFNamePool(const FNameValue& name) {
    if (!gFNamePool || !gNameMul) return {};

    FNameCfg cfg{ gNameMul, gLenOffset, gStrOffset, gLenShift };
    std::string out;
    return TryDecodeFNamePool(name, cfg, &out) ? out : std::string{};
}

static std::string ResolveFName(const FNameValue& name) {
    if (name.ComparisonIndex <= 0) return {};

    std::string poolName = ResolveFNamePool(name);
    if (!poolName.empty()) return poolName;

    if (!gFNameAppend) return {};

    wchar_t buffer[512]{};
    FStringValue out{};
    out.Data = buffer;
    out.Count = 0;
    out.Max = (int32_t)(sizeof(buffer) / sizeof(buffer[0]));

    auto appendString = (FNameAppendStringFn)gFNameAppend;
    appendString(&name, &out);

    if (!out.Data || out.Count <= 0) return {};
    int len = out.Count;
    if (out.Data[len - 1] == L'\0') len--;
    if (len <= 0 || len >= out.Max) return {};

    return NarrowFromWide(out.Data, len);
}

static std::string ObjectFName(uintptr_t obj) {
    if (!obj) return {};
    if (!CanRead(obj + OFF_UObject_FName, sizeof(FNameValue))) return {};
    return ResolveFName(*(FNameValue*)(obj + OFF_UObject_FName));
}

static std::string ClassName(uintptr_t obj) {
    if (!obj) return {};
    uintptr_t cls = RPtr(obj + OFF_UObject_Class);
    if (!cls || !CanRead(cls + OFF_UObject_FName, sizeof(FNameValue))) return {};
    return ResolveFName(*(FNameValue*)(cls + OFF_UObject_FName));
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
static bool TryCalibrateFNameConfig(const FNameValue& name) {
    for (const auto& cfg : gFNameConfigs) {
        if (TryDecodeFNamePool(name, cfg)) {
            gNameMul = cfg.mul;
            gLenOffset = cfg.lenOff;
            gStrOffset = cfg.strOff;
            gLenShift = cfg.lenShift;
            return true;
        }
    }
    return false;
}

static bool CalibrateFNameResolver(uintptr_t knownObj) {
    gFNamePool = gBase + (gOffsetFNamePool ? gOffsetFNamePool : ue_offsets_gnames());
    gFNameAppend = gBase + ;

    if (CanRead(gFNamePool + 0x10, 8)) {
        if (CanRead(knownObj + OFF_UObject_FName, sizeof(FNameValue))) {
            if (TryCalibrateFNameConfig(*(FNameValue*)(knownObj + OFF_UObject_FName))) return true;
        }

        uintptr_t cls = RPtr(knownObj + OFF_UObject_Class);
        if (cls && CanRead(cls + OFF_UObject_FName, sizeof(FNameValue))) {
            if (TryCalibrateFNameConfig(*(FNameValue*)(cls + OFF_UObject_FName))) return true;
        }
    }

    if (!CanRead(gFNameAppend, 16)) return false;
    return !ObjectFName(knownObj).empty() || !ClassName(knownObj).empty();
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
    uintptr_t go = gBase + gOffsetGObjects;
    return (go && CanRead(go + 0x14, 4)) ? RI32(go + 0x14) : 0;
}

static uintptr_t GetUObjectByIndex(int index) {
    uintptr_t go = gBase + gOffsetGObjects;
    if (!go) return 0;
    uintptr_t ca = RPtr(go);
    if (!ca) return 0;
    int ci = index / OBJ_PER_CHUNK, wi = index % OBJ_PER_CHUNK;
    uintptr_t chunk = RPtr(ca + ci * 8);
    if (!chunk) return 0;
    return RPtr(chunk + (uintptr_t)wi * FUOBJ_SIZE);
}

static void ScanForNewAssets() {
    int num = GetNumUObjects();
    if (num <= 0 || num > 10'000'000) return;
    AssetScanner::gTotalGObjects.store(num, std::memory_order_relaxed);

    uint64_t now = GetTickCount64() - AssetScanner::gStartTime;
    int added = 0;

    std::vector<TrackedAsset>        localAdditions;
    std::unordered_map<int, int>     localStats;
    for (int i = 0; i < num; i++) {
        uintptr_t obj = GetUObjectByIndex(i);
        if (!obj) continue;

        // Check and mark known under its own lock.
        {
            std::lock_guard<std::mutex> kl(gKnownMutex);
            if (AssetScanner::gKnownObjs.count(obj)) continue;
            AssetScanner::gKnownObjs.insert(obj);
        }

        // All memory reads happen outside any lock.
        std::string cls = ClassName(obj);
        if (cls.empty() || IsReflectionNoise(cls)) continue;
        std::string objName = ObjectFName(obj);
        if (objName.empty()) continue;
        std::string fullPath = GetObjectPath(obj);

        AssetCategory cat = ClassifyAsset(cls, objName, fullPath);

        TrackedAsset asset;
        asset.address = obj;
        asset.className = std::move(cls);
        asset.objectName = std::move(objName);
        asset.fullPath = std::move(fullPath);
        asset.category = cat;
        asset.discoveryTimeMs = now;
        asset.bookmarked = false;

        localAdditions.push_back(std::move(asset));
        localStats[(int)cat]++;

        if (++added >= 5000) break;
    }

    if (!localAdditions.empty()) {
        std::lock_guard<std::mutex> lk(AssetScanner::gMutex);
        for (auto& [k, v] : localStats)
            AssetScanner::gCategoryStats[k] += v;
        AssetScanner::gAssets.insert(
            AssetScanner::gAssets.end(),
            std::make_move_iterator(localAdditions.begin()),
            std::make_move_iterator(localAdditions.end()));
    }
}

// --- Main scanning thread ---
static DWORD WINAPI ScanThread(LPVOID) {
    gBase = (uintptr_t)GetModuleHandle(nullptr);
    AssetScanner::gStartTime = GetTickCount64();

    ue_offsets_start();

    for (int i = 0; i < 240 && AssetScanner::gRunning.load(); i++) {
        gOffsetGWorld = (uintptr_t)ue_offsets_gworld_storage();
        gOffsetFNamePool = (uintptr_t)ue_offsets_gnames();
        gOffsetGObjects = (uintptr_t)ue_offsets_gobjects_storage();

        if (gOffsetGWorld && gOffsetGObjects) break;
        Sleep(500);
    }

    if (!gOffsetGWorld || !gOffsetGObjects) return 1;

    // Match the old header path: GWorld global address stores the live world pointer.
    uintptr_t gworldAddr = gBase + gOffsetGWorld;
    for (int i = 0; i < 240 && AssetScanner::gRunning.load(); i++) {
        if (RPtr(gworldAddr)) break;
        Sleep(500);
    }

    if (!RPtr(gworldAddr)) return 1;

    Sleep(1000); // small settle delay

    uintptr_t world = RPtr(gworldAddr);

    bool calibrated = CalibrateFNameResolver(world);
    if (!calibrated) {
        int num = GetNumUObjects();
        int limit = num < 4096 ? num : 4096;
        for (int i = 0; i < limit; i++) {
            uintptr_t obj = GetUObjectByIndex(i);
            if (obj && CanRead(obj + OFF_UObject_Class, 8) && CalibrateFNameResolver(obj)) {
                calibrated = true;
                break;
            }
        }
    }

    if (!calibrated) return 1;
    AssetScanner::gCalibrated = true;

    {
        std::lock_guard<std::mutex> kl(gKnownMutex);
        int num = GetNumUObjects();
        for (int i = 0; i < num; i++) {
            uintptr_t obj = GetUObjectByIndex(i);
            if (obj) AssetScanner::gKnownObjs.insert(obj);
        }
        AssetScanner::gBaselineCount = (int)AssetScanner::gKnownObjs.size();
    }

    // Main loop
    while (AssetScanner::gRunning.load()) {
        if (AssetScanner::gScanEnabled.load() || AssetScanner::gForceScan.load()) {
            ScanForNewAssets();
            AssetScanner::gForceScan.store(false);
        }
        int interval = AssetScanner::gScanInterval.load();
        for (int s = 0; s < interval && AssetScanner::gRunning.load()
            && !AssetScanner::gForceScan.load(); s += 20) {
            Sleep(20);
        }
    }
    return 0;
}


void AssetScanner::ResetBaseline() {
    std::lock_guard<std::mutex> lk(gMutex);
    std::lock_guard<std::mutex> kl(gKnownMutex);

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

    // Snapshot the asset list while locked
    std::vector<TrackedAsset> snapshot;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        snapshot = gAssets; // copy
    }

    // Scan up to ~2KB of the object's instance memory
    // (most properties fit here; we avoid overscanning to prevent huge pauses)
    const int SCAN_BYTES = 2048;
    std::unordered_set<uintptr_t> foundPointers;
    for (int offset = 0x28; offset < SCAN_BYTES; offset += 8) {
        uintptr_t val = RPtr(objAddr + offset);
        if (val && val != objAddr)
            foundPointers.insert(val);
    }


    if (foundPointers.empty()) return deps;

    // Match found pointers against snapshot instead
    for (auto& asset : snapshot) {
        if (foundPointers.count(asset.address)) {
            if (asset.category != AssetCategory::Other ||
                asset.className.find("Property") == std::string::npos)
                deps.push_back(asset);
        }
    }

    return deps;
}
