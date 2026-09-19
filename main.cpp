#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace RC
{
    namespace ModShim
    {
        struct Lua;
        struct LuaVec;
    }

    class CppUserModBase
    {
    public:
        virtual ~CppUserModBase() {}
        virtual void on_update() {}
        virtual void on_unreal_init() {}
        virtual void on_ui_init() {}
        virtual void on_program_start() {}
        virtual void on_lua_start(std::wstring_view, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::LuaVec*) {}
        virtual void on_lua_start(ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::LuaVec*) {}
        virtual void on_lua_stop(std::wstring_view, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::LuaVec*) {}
        virtual void on_lua_stop(ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::LuaVec*) {}
        virtual void on_dll_load(std::wstring_view) {}
        virtual void render_tab() {}
        virtual void on_lua_start(std::wstring_view, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*) {}
        virtual void on_lua_start(ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*) {}
        virtual void on_lua_stop(std::wstring_view, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*) {}
        virtual void on_lua_stop(ModShim::Lua*, ModShim::Lua*, ModShim::Lua*, ModShim::Lua*) {}
        virtual void on_cpp_mods_loaded() {}

    protected:
        void* m_gui_tabs_storage[2];

    public:
        std::wstring ModName;
        std::wstring ModVersion;
        std::wstring ModDescription;
        std::wstring ModAuthors;
        std::wstring ModIntendedSDKVersion;
    };
}

#include <Unreal/Common.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UStruct.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/GameplayStatics.hpp>
#include <Unreal/Hooks.hpp>
// =====================================================================
// Sakura_CPP_ChainMine - DRG 连锁挖矿（host side）
//
// 玩法：镐击带资源数据的地形材质（金/硝石/墨菱石/玉石等 17 种矿）时，自动
// 探测出整条连通矿脉的体积，再按最优挖点逐帧崩空整条脉（Dig 模式），或沿
// 矿脉主轴发一条雕刻样条一次刻空（Spline 模式，实验）。只装主机即可，
// 所有地形 op 都是游戏自己的 NetMulticast，客机自动看到同样的崩落。
//
// 实现要点：
//   - 钩 APickaxeItem::Server_DigBlock（Server RPC，按 UFunction 对象精确
//     判定）；钻头等同名函数是不同 UFunction，不会误触发。检测放在全局
//     ProcessEvent pre-callback（按 UFunction 指针过滤）而不是 UFunction
//     FuncPtr hook：发出的 Server RPC 在本地不执行函数体，UFunction hook
//     永远拦不到客机自己的挥镐，全局回调在 RPC 路由前对两侧都触发。
//     触发门控用 UTerrainMaterial::ResourceData != nullptr（与游戏"挖掉出矿"
//     同一判定），探测门控用同材质射线确认每个连锁点都是矿格。
//   - 连锁挖点**逐帧**提交：DRG 地形每帧至多接受一笔刻蚀，同一帧连发多笔
//     只有最后一笔落地。pre-hook 里只探测 + 入队，由游戏线程 ProcessEvent
//     pre-callback 每 FireIntervalMs 发一笔嵌套 Server_DigBlock（重入由
//     g_inChain 挡住），像玩家快速连点镐子一样全部落地并多播同步全客户端。
//   - 探测：体内点（IsPointInsideTerrain）用出射射线材质判定，空气点只认距
//     矿面 0.85×VeinStep 内的同材质命中，BFS 沿 ±X/±Y/±Z 洪泛；每个接受格
//     再采 6 个半步长点，薄脉/斜脉尖端落在网格缝里也不会漏。
//   - Dig 挖点：满尺寸轴对齐盒子覆盖判定（half = DigSize/2，镐坑是体素化
//     方块）；细长脉沿 PCA 主轴排"一串方块"，弯折/侧枝/块状脉退回贪心覆盖；
//     判定/贪心用空间哈希桶加速。队列未崩完时再挥镐合并追加 + 去重。
//   - Spline：主轴拟合 → 一条 TerrainOp_CarveSplineSegment（Material=nullptr
//     + CarveFilter=ReplaceAll，与 BP_Pipeline_Segment 一致）→ 沿样条
//     RemoveDebrisInSphere 清表面装饰。
//
// 单位：全部 UE 厘米制（1米=100）。实测镐洞 DigSize=115.0（1.15m）。
//
// 可靠性/性能：
//   - 全部 UE 反射与配置热重载在游戏线程（ProcessEvent pre-callback）。
//   - 所有 hook 体 SEH 包裹；BFS/入队只在挥镐时执行，队列空时 tick 回调零开销。
// =====================================================================
namespace ChainMine
{
    using namespace RC::Unreal;

    // ---- raw ABI views (no FSD SDK headers needed) ----
    struct RawFVector
    {
        float X, Y, Z;
    };

    struct RawTArray
    {
        void** Data;
        int32_t Num;
        int32_t Max;
    };

    static HMODULE g_hmod = nullptr;
    static HMODULE g_ue4ss = nullptr;
    static std::wstring g_log_path;
    static std::wstring g_cfg_path;

    // DLL-exported helpers resolved at runtime (ABI changed between the SDK
    // submodule and the installed UE4SS.dll; same pattern as MeowChatMod).
    static uint8_t*& (*g_fnLocals)(void*) = nullptr;          // FFrame::Locals()
    static FField*& (*g_fnGetNext)(void*) = nullptr;          // FField::GetNext()

    static bool g_procsResolved = false;

    // ---- config (read/written on the game thread only) ----
    static bool g_cfg_enabled = true;
    static int32_t g_cfg_mode = 0;             // 0 = Dig (lattice digs), 1 = Spline (one carve op)
    static double g_cfg_vein_step = 32.0;      // probe grid step (UE cm; 32 = 0.32 m)
    static int32_t g_cfg_max_nodes = 2000;     // max probed vein cells per swing (also key MaxNodes)
    static double g_cfg_max_distance = 3000.0; // max distance from hit point (UE cm; 3000 = 30 m)
    static bool g_cfg_also_special = true;     // chain also on power attacks
    static bool g_cfg_enable_as_client = false;// allow chaining while joining someone else's game (Dig only)
    static int32_t g_cfg_raycast_filter = 3;   // ELandscapeCellFilter: 0 Any 1 Empty 2 Filled 3 Diggable ...
    static double g_cfg_probe_ray_dist = 350.0;  // probe ray length (UE cm; must exceed vein half-thickness)
    static double g_cfg_dig_radius = 115.0;      // pickaxe DigSize (UE cm; crater is a voxelized cube, half-width = DigSize/2)
    static double g_cfg_tube_margin = 60.0;      // spline: extra radius/length beyond vein extent (UE cm)
    static double g_cfg_max_spline_radius = 220.0; // spline safety cap: fitted tube radius upper bound (UE cm)
    static int32_t g_cfg_fire_interval_ms = 32;// min ms between deferred chain digs
    static ULONGLONG g_cfg_check_tick = 0;
    static bool g_cfg_had_file = false;
    static uint64_t g_cfg_last_write = 0;

    // ---- resolved hook state ----
    static UFunction* g_fnDigBlock = nullptr;
    static int32_t g_offCarvePos = -1;
    static int32_t g_offCarveDir = -1;
    static int32_t g_offTerrainMaterial = -1;
    static int32_t g_offIsSpecial = -1;

    static UFunction* g_fnTerrainOpPickAxe = nullptr;   // pre-hook tracks OperationNumber for spline numbering
    static UFunction* g_fnRaycast = nullptr;

    static UClass* g_csgClass = nullptr;             // /Script/FSD.DeepCSGWorld
    static UClass* g_collectionClass = nullptr;      // /Script/FSD.TerrainMaterialsCollection
    static UClass* g_matClass = nullptr;             // /Script/FSD.TerrainMaterial

    static UScriptStruct* g_raycastHitStruct = nullptr; // /Script/FSD.CSGRaycastHitInfo

    static int32_t g_offCsgTerrainMaterials = -1;    // ADeepCSGWorld::TerrainMaterials
    static int32_t g_offCollMaterials = -1;          // UTerrainMaterialsCollection::Materials
    static int32_t g_offMatResourceData = -1;        // UTerrainMaterial::ResourceData

    // ---- spline carve state (/Script/FSD.SplineSegmentCarveOperationData etc.) ----
    static UFunction* g_fnSplineOp = nullptr;        // ADeepCSGWorld::TerrainOp_CarveSplineSegment (NetMulticast)
    static UScriptStruct* g_splineOpStruct = nullptr;   // FSplineSegmentCarveOperationData
    static UScriptStruct* g_splineSegStruct = nullptr;  // FCarveSplineSegment
    static int32_t g_offSplineParam = -1;            // function parm 'Data' offset
    static int32_t g_offSplineOpNum = -1;            // OperationNumber
    static int32_t g_offSplineSegs = -1;             // Segments TArray
    static int32_t g_offSplineMat = -1;              // Material
    static int32_t g_offSplineFilter = -1;           // CarveFilter
    static int32_t g_offSplinePrecious = -1;         // Precious
    static int32_t g_offSplineLevelGen = -1;         // LevelGenerationComponent
    static int32_t g_offSegStart = -1;               // FCarveSplineSegment::SplineStart
    static int32_t g_offSegStartTan = -1;            // SplineStartTangent
    static int32_t g_offSegEnd = -1;                 // SplineEnd
    static int32_t g_offSegEndTan = -1;              // SplineEndTangent
    static int32_t g_offSegRadius = -1;              // Radius
    static bool g_splineReady = false;
    static bool g_triedSplineFn = false;
    static int32_t g_lastOpNumber = 0;               // last OperationNumber seen on TerrainOp_PickAxe
    static int32_t g_splineFireCount = 0;

    struct SplinePlan
    {
        RawFVector start;
        RawFVector startTan;
        RawFVector end;
        RawFVector endTan;
        float radius;
        bool valid;
    };
    struct PendingSpline
    {
        AActor* csg;
        SplinePlan plan;
        bool valid;
    };
    static PendingSpline g_pendingSpline;

    // FCSGRaycastHitInfo offsets
    static int32_t g_offHitPosition = 0x00;
    static int32_t g_offHitMaterial = 0x18;

    // DeepCSGWorld::Raycast parm offsets
    static int32_t g_offRcStart = -1;
    static int32_t g_offRcDirection = -1;
    static int32_t g_offRcMaxDist = -1;
    static int32_t g_offRcHitInfo = -1;
    static int32_t g_offRcFilter = -1;
    static int32_t g_offRcReturn = -1;

    // DeepCSGWorld::IsPointInsideTerrain (cheap "inside any solid" test)
    static UFunction* g_fnInsideTerrain = nullptr;
    static int32_t g_offInsidePos = -1;
    static int32_t g_offInsideReturn = -1;
    static bool g_insideReady = false;
    static bool g_warnedNoInside = false;
    static bool g_warnedClientDisabled = false;
    static bool g_warnedClientDigOnly = false;

    // DeepCSGWorld::RemoveDebrisInSphere (clean ore-surface decorations
    // that spline carves leave behind - the game's own dig path removes them)
    static UFunction* g_fnRemoveDebris = nullptr;
    static int32_t g_offDebrisPos = -1;
    static int32_t g_offDebrisRadius = -1;
    static int32_t g_offDebrisFragile = -1;
    static int32_t g_offDebrisDurable = -1;
    static int32_t g_offDebrisType = -1;
    static bool g_debrisReady = false;

    static CallbackId g_opNumHookId = 0;
    static bool g_ready = false;
    static bool g_raycastReady = false;
    static bool g_warnedNoCsg = false;
    static bool g_warnedRayFallback = false;
    static bool g_warnedNotOreMat = false;

    // re-entrancy guard: chain digs call Server_DigBlock again via ProcessEvent
    static bool g_inChain = false;

    // lazily cached DeepCSGWorld actor per world
    static AActor* g_csgActor = nullptr;
    static UWorld* g_cachedWorld = nullptr;

    // ---- deferred dig queue (fires ~1 dig per frame, see DrainChainQueue) ----
    struct PendingDig
    {
        AActor* pickaxe;
        RawFVector pos;
        RawFVector dir;
        int32_t mat;
        bool special;
    };
    static std::vector<PendingDig> g_pendingDigs;
    static size_t g_pendingHead = 0;
    static uint64_t g_lastFireTick = 0;
    static int32_t g_firedTotal = 0;

    static void Log(const wchar_t* fmt, ...);
    static int32_t FindPropOffset(UStruct* s, const wchar_t* name);
    // =====================================================================
    // Logging + config (same layout as MeowChatMod)
    // =====================================================================
    static void InitLogPath()
    {
        wchar_t buf[MAX_PATH] = {0};
        DWORD n = GetModuleFileNameW(g_hmod, buf, MAX_PATH);
        std::wstring p(buf, n);
        auto pos = p.rfind(L'\\');
        if (pos != std::wstring::npos) p = p.substr(0, pos);
        pos = p.rfind(L'\\');
        if (pos != std::wstring::npos) p = p.substr(0, pos);
        g_log_path = p + L"\\chainmine.log";
        g_cfg_path = p + L"\\config.txt";
        DeleteFileW(g_log_path.c_str());
    }

    static void Log(const wchar_t* fmt, ...)
    {
        if (g_log_path.empty()) return;
        wchar_t buf[1024] = {0};
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, args);
        va_end(args);
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_log_path.c_str(), L"a, ccs=UTF-8") == 0 && f)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fwprintf(f, L"[%02u:%02u:%02u.%03u] %s\n",
                     (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds, buf);
            fclose(f);
        }
    }

    static std::wstring TrimW(const std::wstring& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && iswspace(s[b])) ++b;
        while (e > b && iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    static std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0) return {};
        std::wstring w((size_t)n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s((size_t)n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    static bool ParseBoolValue(const std::wstring& v, bool def)
    {
        if (_wcsicmp(v.c_str(), L"true") == 0 || v == L"1" ||
            _wcsicmp(v.c_str(), L"yes") == 0 || _wcsicmp(v.c_str(), L"on") == 0) return true;
        if (_wcsicmp(v.c_str(), L"false") == 0 || v == L"0" ||
            _wcsicmp(v.c_str(), L"no") == 0 || _wcsicmp(v.c_str(), L"off") == 0) return false;
        return def;
    }

    static double ParseDoubleValue(const std::wstring& v, double def)
    {
        if (v.empty()) return def;
        return _wtof(v.c_str());
    }

    static void WriteDefaultConfig()
    {
        const wchar_t* txt =
            L"# Sakura_CPP_ChainMine config (UTF-8)\n"
            L"# 保存后约 1 秒生效，无需重启游戏。# 开头为注释。\n"
            L"\n"
            L"# 总开关\n"
            L"Enabled = true\n"
            L"\n"
            L"# 模式：Dig = 逐帧挖空整条矿脉（默认）；Spline = 一条样条一次刻空（实验）\n"
            L"Mode = Dig\n"
            L"\n"
            L"# 探测网格步长（厘米，32=0.32米），越小覆盖越全\n"
            L"VeinStep = 32\n"
            L"\n"
            L"# 单次挥镐最多探测的矿脉格数（0=关闭连锁）\n"
            L"MaxNodes = 2000\n"
            L"\n"
            L"# 距命中点的最大连锁距离（厘米，3000=30米）\n"
            L"MaxDistance = 3000\n"
            L"\n"
            L"# 探测射线长度（厘米），需大于矿脉半径\n"
            L"ProbeRayDist = 350\n"
            L"\n"
            L"# 镐洞尺寸（厘米），游戏默认 115\n"
            L"DigRadius = 115\n"
            L"\n"
            L"# Spline 模式：样条超出矿脉的余量（厘米）\n"
            L"TubeMargin = 60\n"
            L"\n"
            L"# Spline 模式：样条半径上限（厘米），防止挖出超大洞\n"
            L"MaxSplineRadius = 220\n"
            L"\n"
            L"# 重击（Power Attack）是否也触发连锁\n"
            L"AlsoSpecial = true\n"
            L"\n"
            L"# 进别人房（客机）是否也连锁，仅 Dig 模式\n"
            L"EnableAsClient = false\n"
            L"\n"
            L"# 射线过滤器：0=Any 1=Empty 2=Filled 3=Diggable 4=NotDiggable\n"
            L"RaycastFilter = 3\n"
            L"\n"
            L"# 连锁挖掘间隔（毫秒）：卡顿调大，太慢调小\n"
            L"FireIntervalMs = 32\n";
        std::wstring ws = txt;
        std::string utf8 = WideToUtf8(ws);
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_cfg_path.c_str(), L"wb") == 0 && f)
        {
            fwrite(utf8.data(), 1, utf8.size(), f);
            fclose(f);
        }
    }

    static void LoadConfig()
    {
        if (g_cfg_path.empty()) return;
        HANDLE h = CreateFileW(g_cfg_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return; }
        size_t size = (size_t)sz.QuadPart;
        std::string buf(size ? size : 1, 0);
        DWORD read = 0;
        BOOL ok = (size > 0) && ReadFile(h, &buf[0], (DWORD)size, &read, nullptr);
        CloseHandle(h);
        if (!ok) return;

        size_t off = (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF &&
                      (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) ? 3 : 0;
        std::wstring text = Utf8ToWide(buf.substr(off));

        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find(L'\n', pos);
            std::wstring line = TrimW(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
            pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
            if (line.empty() || line[0] == L'#') continue;
            if (line[0] == 0xFEFF) line = line.substr(1);
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = TrimW(line.substr(0, eq));
            std::wstring val = TrimW(line.substr(eq + 1));
            if (_wcsicmp(key.c_str(), L"Enabled") == 0) g_cfg_enabled = ParseBoolValue(val, g_cfg_enabled);
            else if (_wcsicmp(key.c_str(), L"Mode") == 0) g_cfg_mode = (_wcsicmp(val.c_str(), L"Spline") == 0) ? 1 : 0;
            else if (_wcsicmp(key.c_str(), L"VeinStep") == 0) g_cfg_vein_step = ParseDoubleValue(val, g_cfg_vein_step);
            else if (_wcsicmp(key.c_str(), L"MaxNodes") == 0 || _wcsicmp(key.c_str(), L"MaxCells") == 0) g_cfg_max_nodes = (int32_t)_wtoi(val.c_str());
            else if (_wcsicmp(key.c_str(), L"MaxDistance") == 0) g_cfg_max_distance = ParseDoubleValue(val, g_cfg_max_distance);
            else if (_wcsicmp(key.c_str(), L"ProbeRayDist") == 0) g_cfg_probe_ray_dist = ParseDoubleValue(val, g_cfg_probe_ray_dist);
            else if (_wcsicmp(key.c_str(), L"DigRadius") == 0) g_cfg_dig_radius = ParseDoubleValue(val, g_cfg_dig_radius);
            else if (_wcsicmp(key.c_str(), L"TubeMargin") == 0) g_cfg_tube_margin = ParseDoubleValue(val, g_cfg_tube_margin);
            else if (_wcsicmp(key.c_str(), L"MaxSplineRadius") == 0) g_cfg_max_spline_radius = ParseDoubleValue(val, g_cfg_max_spline_radius);
            else if (_wcsicmp(key.c_str(), L"AlsoSpecial") == 0) g_cfg_also_special = ParseBoolValue(val, g_cfg_also_special);
            else if (_wcsicmp(key.c_str(), L"EnableAsClient") == 0) g_cfg_enable_as_client = ParseBoolValue(val, g_cfg_enable_as_client);
            else if (_wcsicmp(key.c_str(), L"RaycastFilter") == 0) g_cfg_raycast_filter = _wtoi(val.c_str());
            else if (_wcsicmp(key.c_str(), L"FireIntervalMs") == 0) g_cfg_fire_interval_ms = _wtoi(val.c_str());
        }
        Log(L"config: mode=%ls enabled=%d step=%.1f maxCells=%d maxDist=%.1f probeRay=%.0f digRadius=%.0f margin=%.0f maxSplineR=%.0f special=%d client=%d fireMs=%d",
            g_cfg_mode == 1 ? L"Spline" : L"Dig", g_cfg_enabled ? 1 : 0, g_cfg_vein_step, g_cfg_max_nodes,
            g_cfg_max_distance, g_cfg_probe_ray_dist, g_cfg_dig_radius, g_cfg_tube_margin,
            g_cfg_max_spline_radius, g_cfg_also_special ? 1 : 0, g_cfg_enable_as_client ? 1 : 0, g_cfg_fire_interval_ms);
    }

    static void TryReloadConfig()
    {
        ULONGLONG now = GetTickCount64();
        if (now - g_cfg_check_tick < 1000) return;
        g_cfg_check_tick = now;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(g_cfg_path.c_str(), GetFileExInfoStandard, &fad))
        {
            if (g_cfg_had_file)
            {
                g_cfg_had_file = false;
                Log(L"config: file removed, keeping last values");
            }
            return;
        }
        uint64_t ft = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
        if (g_cfg_had_file && ft == g_cfg_last_write) return;
        g_cfg_had_file = true;
        g_cfg_last_write = ft;
        LoadConfig();
    }
    // =====================================================================
    // Reflection helpers
    // =====================================================================
    static void ResolveProcs()
    {
        if (g_procsResolved) return;
        g_ue4ss = GetModuleHandleW(L"UE4SS.dll");
        if (!g_ue4ss) return;
        g_fnLocals = (uint8_t*& (*)(void*))GetProcAddress(g_ue4ss, "?Locals@FFrame@Unreal@RC@@QEAAAEAPEAEXZ");
        g_fnGetNext = (FField*& (*)(void*))GetProcAddress(g_ue4ss, "?GetNext@FField@Unreal@RC@@AEAAAEAPEAV123@XZ");
        g_procsResolved = true;
    }

    static int32_t FindPropOffset(UStruct* s, const wchar_t* name)
    {
        if (!s) return -1;
        FField* child = s->GetChildProperties();
        while (child)
        {
            if (child->GetName() == name)
            {
                FProperty* prop = static_cast<FProperty*>(child);
                return prop->GetOffset_Internal();
            }
            if (!g_fnGetNext) return -1;
            child = g_fnGetNext(child);
        }
        return -1;
    }

    static void OnDigDetect(UObject* Context, UFunction* Function, void* Parms);
    static void OnTerrainOpPickaxe(UnrealScriptFunctionCallableContext& ctx, void*);
    static void ClearPendingQueue();
    static bool EnqueueDig(AActor* pickaxe, const RawFVector& pos, const RawFVector& dir, int32_t mat, bool special, float dedupR);

    // =====================================================================
    // Lazy resolution + hook registration (game thread only)
    // =====================================================================
    static bool g_triedDigFn = false;
    static bool g_triedOpFn = false;
    static bool g_triedRayFn = false;
    static bool g_triedInsideFn = false;
    static bool g_triedDebrisFn = false;
    static bool g_triedClasses = false;

    static void TryRegisterHooks()
    {
        if (g_ready) return;
        ResolveProcs();

        if (!g_triedDigFn)
        {
            g_triedDigFn = true;
            g_fnDigBlock = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.PickaxeItem:Server_DigBlock");
            if (g_fnDigBlock)
            {
                g_offCarvePos = FindPropOffset(g_fnDigBlock, L"carvePos");
                g_offCarveDir = FindPropOffset(g_fnDigBlock, L"carveDirection");
                g_offTerrainMaterial = FindPropOffset(g_fnDigBlock, L"TerrainMaterial");
                g_offIsSpecial = FindPropOffset(g_fnDigBlock, L"isSpecial");
                if (g_offCarvePos >= 0 && g_offCarveDir >= 0 && g_offTerrainMaterial >= 0)
                {
                    // Detected from the global ProcessEvent pre-callback, not a
                    // UFunction FuncPtr hook: an outgoing server RPC never
                    // executes its body locally, so a UFunction hook cannot
                    // fire for the client's own swings. The global callback
                    // runs before RPC routing on BOTH sides (host execution
                    // and client send).
                    Log(L"dig: Server_DigBlock ready (carvePos=%d carveDir=%d mat=%d special=%d)",
                        g_offCarvePos, g_offCarveDir, g_offTerrainMaterial, g_offIsSpecial);
                }
                else
                {
                    Log(L"dig: Server_DigBlock parm offsets incomplete, retrying");
                    g_triedDigFn = false;
                }
            }
        }

        if (!g_triedOpFn)
        {
            g_triedOpFn = true;
            g_fnTerrainOpPickAxe = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld:TerrainOp_PickAxe");
            if (g_fnTerrainOpPickAxe && !g_opNumHookId)
            {
                g_opNumHookId = g_fnTerrainOpPickAxe->RegisterPreHook(OnTerrainOpPickaxe);
            }
            if (!g_fnTerrainOpPickAxe) g_triedOpFn = false;
        }

        if (!g_triedRayFn)
        {
            g_triedRayFn = true;
            g_fnRaycast = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld:Raycast");
            if (g_fnRaycast)
            {
                g_offRcStart = FindPropOffset(g_fnRaycast, L"Start");
                g_offRcDirection = FindPropOffset(g_fnRaycast, L"Direction");
                g_offRcMaxDist = FindPropOffset(g_fnRaycast, L"MaxDistance");
                g_offRcHitInfo = FindPropOffset(g_fnRaycast, L"HitInfo");
                g_offRcFilter = FindPropOffset(g_fnRaycast, L"Filter");
                g_offRcReturn = FindPropOffset(g_fnRaycast, L"ReturnValue");
                g_raycastHitStruct = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, L"/Script/FSD.CSGRaycastHitInfo");
                if (g_raycastHitStruct)
                {
                    g_offHitPosition = FindPropOffset(g_raycastHitStruct, L"Position");
                    g_offHitMaterial = FindPropOffset(g_raycastHitStruct, L"Material");
                }
                g_raycastReady = g_offRcStart >= 0 && g_offRcDirection >= 0 && g_offRcMaxDist >= 0 &&
                                 g_offRcHitInfo >= 0 && g_offRcFilter >= 0 && g_offRcReturn >= 0 &&
                                 g_offHitMaterial >= 0;
                if (g_raycastReady)
                {
                    Log(L"hook: Raycast ready (start=%d dir=%d dist=%d hitInfo=%d filter=%d ret=%d hitMat=%d)",
                        g_offRcStart, g_offRcDirection, g_offRcMaxDist, g_offRcHitInfo, g_offRcFilter, g_offRcReturn, g_offHitMaterial);
                }
            }
            else
            {
                g_triedRayFn = false;
            }
        }

        if (!g_triedInsideFn)
        {
            g_triedInsideFn = true;
            g_fnInsideTerrain = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld:IsPointInsideTerrain");
            if (g_fnInsideTerrain)
            {
                g_offInsidePos = FindPropOffset(g_fnInsideTerrain, L"Pos");
                g_offInsideReturn = FindPropOffset(g_fnInsideTerrain, L"ReturnValue");
                g_insideReady = g_offInsidePos >= 0 && g_offInsideReturn >= 0;
                if (g_insideReady)
                {
                    Log(L"hook: IsPointInsideTerrain ready (pos=%d ret=%d)", g_offInsidePos, g_offInsideReturn);
                }
                else
                {
                    Log(L"hook: IsPointInsideTerrain parm offsets incomplete, retrying");
                    g_triedInsideFn = false;
                }
            }
            else
            {
                g_triedInsideFn = false;
            }
        }

        if (!g_triedDebrisFn)
        {
            g_triedDebrisFn = true;
            g_fnRemoveDebris = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld:RemoveDebrisInSphere");
            if (g_fnRemoveDebris)
            {
                g_offDebrisPos = FindPropOffset(g_fnRemoveDebris, L"Position");
                g_offDebrisRadius = FindPropOffset(g_fnRemoveDebris, L"Radius");
                g_offDebrisFragile = FindPropOffset(g_fnRemoveDebris, L"onlyFragile");
                g_offDebrisDurable = FindPropOffset(g_fnRemoveDebris, L"alsoDurable");
                g_offDebrisType = FindPropOffset(g_fnRemoveDebris, L"onlyType");
                g_debrisReady = g_offDebrisPos >= 0 && g_offDebrisRadius >= 0 &&
                                g_offDebrisFragile >= 0 && g_offDebrisDurable >= 0 &&
                                g_offDebrisType >= 0;
                if (g_debrisReady)
                {
                    Log(L"hook: RemoveDebrisInSphere ready (pos=%d radius=%d fragile=%d durable=%d type=%d)",
                        g_offDebrisPos, g_offDebrisRadius, g_offDebrisFragile,
                        g_offDebrisDurable, g_offDebrisType);
                }
                else
                {
                    g_triedDebrisFn = false;
                }
            }
            else
            {
                g_triedDebrisFn = false;
            }
        }

        if (!g_triedSplineFn)
        {
            g_triedSplineFn = true;
            g_fnSplineOp = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld:TerrainOp_CarveSplineSegment");
            g_splineOpStruct = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, L"/Script/FSD.SplineSegmentCarveOperationData");
            g_splineSegStruct = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, L"/Script/FSDEngine.CarveSplineSegment");
            if (g_fnSplineOp && g_splineOpStruct && g_splineSegStruct)
            {
                g_offSplineParam = FindPropOffset(g_fnSplineOp, L"Data");
                g_offSplineOpNum = FindPropOffset(g_splineOpStruct, L"OperationNumber");
                g_offSplineSegs = FindPropOffset(g_splineOpStruct, L"Segments");
                g_offSplineMat = FindPropOffset(g_splineOpStruct, L"Material");
                g_offSplineFilter = FindPropOffset(g_splineOpStruct, L"CarveFilter");
                g_offSplinePrecious = FindPropOffset(g_splineOpStruct, L"Precious");
                g_offSplineLevelGen = FindPropOffset(g_splineOpStruct, L"LevelGenerationComponent");
                g_offSegStart = FindPropOffset(g_splineSegStruct, L"SplineStart");
                g_offSegStartTan = FindPropOffset(g_splineSegStruct, L"SplineStartTangent");
                g_offSegEnd = FindPropOffset(g_splineSegStruct, L"SplineEnd");
                g_offSegEndTan = FindPropOffset(g_splineSegStruct, L"SplineEndTangent");
                g_offSegRadius = FindPropOffset(g_splineSegStruct, L"Radius");
                if (g_offSplineOpNum >= 0 && g_offSplineSegs >= 0 && g_offSplineMat >= 0 &&
                    g_offSplineFilter >= 0 && g_offSplinePrecious >= 0 &&
                    g_offSegStart >= 0 && g_offSegStartTan >= 0 && g_offSegEnd >= 0 &&
                    g_offSegEndTan >= 0 && g_offSegRadius >= 0)
                {
                    g_splineReady = true;
                    Log(L"hook: spline carve ready (param=%d opNum=%d segs=%d mat=%d filter=%d precious=%d)",
                        g_offSplineParam, g_offSplineOpNum, g_offSplineSegs, g_offSplineMat,
                        g_offSplineFilter, g_offSplinePrecious);
                }
                else
                {
                    Log(L"hook: spline struct offsets incomplete, retrying");
                    g_triedSplineFn = false;
                }
            }
            else
            {
                g_triedSplineFn = false;
            }
        }

        if (!g_triedClasses)
        {
            g_triedClasses = true;
            g_csgClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, L"/Script/FSD.DeepCSGWorld");
            g_collectionClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, L"/Script/FSD.TerrainMaterialsCollection");
            g_matClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, L"/Script/FSD.TerrainMaterial");
            if (!(g_csgClass && g_collectionClass && g_matClass)) g_triedClasses = false;

            if (g_csgClass)
            {
                g_offCsgTerrainMaterials = FindPropOffset(g_csgClass, L"TerrainMaterials");
            }
            if (g_collectionClass)
            {
                g_offCollMaterials = FindPropOffset(g_collectionClass, L"Materials");
            }
            if (g_matClass)
            {
                g_offMatResourceData = FindPropOffset(g_matClass, L"ResourceData");
            }
        }

        if (g_fnDigBlock &&
            g_csgClass && g_collectionClass && g_matClass &&
            g_offCsgTerrainMaterials >= 0 && g_offCollMaterials >= 0 &&
            g_offMatResourceData >= 0)
        {
            g_ready = true;
            Log(L"chain: all hooks ready (raycast=%d)", g_raycastReady ? 1 : 0);
        }
    }
    // =====================================================================
    // Chain mining core (game thread)
    // =====================================================================
    static AActor* FindDeepCSGWorld(UWorld* world)
    {
        if (!world) return nullptr;
        if (g_csgActor && g_cachedWorld == world && g_csgActor->GetWorld() == world)
        {
            return g_csgActor;
        }
        g_csgActor = nullptr;
        g_cachedWorld = world;
        if (!g_csgClass) return nullptr;
        std::vector<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(world, g_csgClass, actors);
        for (AActor* a : actors)
        {
            if (a) { g_csgActor = a; break; }
        }
        if (!g_csgActor && !g_warnedNoCsg)
        {
            g_warnedNoCsg = true;
            Log(L"chain: ADeepCSGWorld not found yet");
        }
        else if (g_csgActor)
        {
            g_warnedNoCsg = false;
        }
        return g_csgActor;
    }

    struct GridCell
    {
        int32_t X, Y, Z;
    };

    static bool CellVisited(const std::vector<GridCell>& visited, const GridCell& c)
    {
        for (size_t i = 0; i < visited.size(); ++i)
        {
            if (visited[i].X == c.X && visited[i].Y == c.Y && visited[i].Z == c.Z) return true;
        }
        return false;
    }

    // Material probe.
    //
    // Two CSG primitives:
    //  - IsPointInsideTerrain(p): true when p is inside ANY solid cell.
    //  - Raycast(p, axis, dist, Filter): first solid crossing along the ray.
    //
    // The game's CSG raycast reports the material of the cell being LEFT at
    // the first boundary. So for a point INSIDE solid, the exit material on
    // any hitting axis IS the material of the cell containing the point: the
    // first axis that hits decides (ore -> true, rock -> false), and interior
    // points of big veins pass too (no distance requirement). For a point in
    // AIR the first hit is an ENTRY into the nearest solid and the hit
    // distance is the gap to its surface, so those are only accepted within a
    // small margin of the vein - the flood hugs the surface without leaking
    // through rock or open space.
    static bool CallIsPointInsideTerrain(AActor* csg, const RawFVector& p)
    {
        if (!g_insideReady || g_offInsidePos < 0 || g_offInsideReturn < 0) return false;
        uint8_t parms[32] = {0};
        *(RawFVector*)(parms + g_offInsidePos) = p;
        csg->ProcessEvent(g_fnInsideTerrain, parms);
        return *(bool*)(parms + g_offInsideReturn) != 0;
    }

    // Raycast from 'point' along normalized axis 'd'; returns hit distance in
    // cm (>= 0) or -1 on miss, and fills outMat with the hit material index.
    static float ProbeRay(AActor* csg, const RawFVector& point, const RawFVector& d,
                          float maxDist, int32_t& outMat)
    {
        outMat = -1;
        if (!g_fnRaycast || !g_raycastReady) return -1.0f;
        uint8_t parms[96] = {0};
        if (g_offRcStart >= 0) *(RawFVector*)(parms + g_offRcStart) = point;
        if (g_offRcDirection >= 0) *(RawFVector*)(parms + g_offRcDirection) = d;
        if (g_offRcMaxDist >= 0) *(float*)(parms + g_offRcMaxDist) = maxDist;
        if (g_offRcFilter >= 0) *(uint8_t*)(parms + g_offRcFilter) = (uint8_t)g_cfg_raycast_filter;
        if (g_offRcHitInfo >= 0) memset(parms + g_offRcHitInfo, 0, 0x1C);
        csg->ProcessEvent(g_fnRaycast, parms);
        bool hit = g_offRcReturn < 0 || *(bool*)(parms + g_offRcReturn);
        if (!hit) return -1.0f;
        RawFVector hp = *(RawFVector*)(parms + g_offRcHitInfo + g_offHitPosition);
        float dx = hp.X - point.X, dy = hp.Y - point.Y, dz = hp.Z - point.Z;
        outMat = *(int32_t*)(parms + g_offRcHitInfo + g_offHitMaterial);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static bool PointIsVein(AActor* csg, const RawFVector& point, int32_t targetMat)
    {
        if (!g_fnRaycast || !g_raycastReady) return true; // fallback: carve everything
        static const float kAxes[6][3] = {
            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };
        // Air points only count as vein within ~1 cell of the ore surface.
        // Generous on purpose: rock points are filtered by the inside branch
        // (their exit material is rock), so this only widens thin-vein tips.
        float margin = (float)std::clamp(0.85 * g_cfg_vein_step, 15.0, 90.0);
        float probeDist = (float)std::clamp(g_cfg_probe_ray_dist, 80.0, 1500.0);

        if (g_insideReady)
        {
            if (CallIsPointInsideTerrain(csg, point))
            {
                // p is inside solid: the first axis that hits reveals the
                // material of the cell at p. No distance check - the hit is
                // the EXIT, so deep interior points of big veins pass too.
                for (int i = 0; i < 6; ++i)
                {
                    RawFVector d{ kAxes[i][0], kAxes[i][1], kAxes[i][2] };
                    int32_t m = -1;
                    if (ProbeRay(csg, point, d, probeDist, m) >= 0.0f) return m == targetMat;
                }
                return false; // inside solid but every exit is beyond probeDist
            }
            // p is in air: accept only within 'margin' of a same-material
            // surface so the flood hugs the vein (covers thin/irregular
            // veins) without spilling into rock or open space.
            for (int i = 0; i < 6; ++i)
            {
                RawFVector d{ kAxes[i][0], kAxes[i][1], kAxes[i][2] };
                int32_t m = -1;
                float dist = ProbeRay(csg, point, d, margin, m);
                if (dist >= 0.0f && dist <= margin && m == targetMat) return true;
            }
            return false;
        }

        if (!g_warnedNoInside)
        {
            g_warnedNoInside = true;
            Log(L"probe: IsPointInsideTerrain unavailable - using distance-limited probe (big-vein interiors may be missed)");
        }
        // Fallback (no IsPointInsideTerrain): entry/exit cannot be told apart,
        // so require a short hit. This stops the mass-flooding of air/rock.
        for (int i = 0; i < 6; ++i)
        {
            RawFVector d{ kAxes[i][0], kAxes[i][1], kAxes[i][2] };
            int32_t m = -1;
            float dist = ProbeRay(csg, point, d, probeDist, m);
            if (dist >= 0.0f && dist <= margin && m == targetMat) return true;
        }
        return false;
    }

    // Probe: flood fill the vein *volume* from the hit point (which sits on
    // the vein surface). Every accepted grid cell is a solid ore cell, so the
    // BFS grows through the whole blob and stops at the vein boundary.
    static std::vector<RawFVector> ProbeVeinCells(AActor* csg, const RawFVector& hit, const RawFVector& dir, int32_t mat)
    {
        std::vector<RawFVector> cells;
        int32_t maxCells = std::clamp<int32_t>(g_cfg_max_nodes, 1, 2000);
        float step = (float)std::clamp(g_cfg_vein_step, 25.0, 150.0);
        float maxDist = (float)std::clamp(g_cfg_max_distance, 150.0, 3000.0);

        // seed: the hit point sits ON the vein surface and usually does not
        // register as inside solid, so walk along +-swing direction in fine
        // 15cm steps to the first point inside the vein; fall back to probing
        // the 6 lattice neighbors for thin/slanted veins.
        RawFVector seed = hit;
        bool ok = false;
        float dirLen = std::sqrt((double)(dir.X*dir.X + dir.Y*dir.Y + dir.Z*dir.Z));
        if (dirLen >= 1.0f)
        {
            RawFVector dn{ dir.X / dirLen, dir.Y / dirLen, dir.Z / dirLen };
            if (PointIsVein(csg, hit, mat)) { seed = hit; ok = true; }
            for (int side = 0; side < 2 && !ok; ++side)
            {
                float s = (side == 0) ? 1.0f : -1.0f;
                for (int k = 1; k <= 9 && !ok; ++k)
                {
                    float d = s * 15.0f * (float)k;
                    RawFVector p{ hit.X + dn.X*d, hit.Y + dn.Y*d, hit.Z + dn.Z*d };
                    if (PointIsVein(csg, p, mat)) { seed = p; ok = true; }
                }
            }
        }
        if (!ok)
        {
            static const float kAxes[6][3] = {
                {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
            };
            static const float kOffs[2] = { 0.6f, 1.0f };
            for (int o = 0; o < 2 && !ok; ++o)
            for (int i = 0; i < 6 && !ok; ++i)
            {
                float off = kOffs[o] * step;
                RawFVector p{ hit.X + kAxes[i][0]*off, hit.Y + kAxes[i][1]*off, hit.Z + kAxes[i][2]*off };
                if (PointIsVein(csg, p, mat)) { seed = p; ok = true; }
            }
        }
        if (!ok)
        {
            Log(L"probe: no in-vein seed at (%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f)",
                hit.X, hit.Y, hit.Z, dir.X, dir.Y, dir.Z);
            return cells;
        }

        std::vector<GridCell> visited;
        std::vector<GridCell> queue;
        std::unordered_set<int64_t> halfSeen;
        visited.reserve(maxCells + 16);
        queue.reserve(maxCells + 16);
        queue.push_back(GridCell{0, 0, 0});
        visited.push_back(GridCell{0, 0, 0});
        cells.push_back(seed);
        size_t head = 0;
        int32_t skipped = 0;
        while (head < queue.size() && (int32_t)cells.size() < maxCells)
        {
            const GridCell cur = queue[head++];
            static const int kAxes[6][3] = {
                {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
            };
            for (int i = 0; i < 6; ++i)
            {
                if ((int32_t)cells.size() >= maxCells) break;
                GridCell nxt{ cur.X + kAxes[i][0], cur.Y + kAxes[i][1], cur.Z + kAxes[i][2] };
                if (CellVisited(visited, nxt)) continue;
                visited.push_back(nxt);
                float dist = step * (float)std::sqrt((double)(nxt.X*nxt.X + nxt.Y*nxt.Y + nxt.Z*nxt.Z));
                if (dist > maxDist) continue;
                RawFVector p;
                p.X = seed.X + (float)nxt.X * step;
                p.Y = seed.Y + (float)nxt.Y * step;
                p.Z = seed.Z + (float)nxt.Z * step;
                if (!PointIsVein(csg, p, mat)) { ++skipped; continue; }
                cells.push_back(p);
                queue.push_back(nxt);
                // Also accept half-step offset samples so thin vein tips
                // whose interior falls between lattice points are still
                // covered by dig planning. Each half point is keyed in
                // half-step grid units so it is only accepted once even when
                // two adjacent full cells both see it.
                for (int h = 0; h < 6 && (int32_t)cells.size() < maxCells; ++h)
                {
                    RawFVector hp{ p.X + kAxes[h][0]*step*0.5f, p.Y + kAxes[h][1]*step*0.5f, p.Z + kAxes[h][2]*step*0.5f };
                    int64_t hx = (int64_t)std::llround((hp.X - seed.X) / (step * 0.5f));
                    int64_t hy = (int64_t)std::llround((hp.Y - seed.Y) / (step * 0.5f));
                    int64_t hz = (int64_t)std::llround((hp.Z - seed.Z) / (step * 0.5f));
                    int64_t hkey = (hx << 42) | (hy << 21) | hz;
                    if (halfSeen.count(hkey)) continue;
                    halfSeen.insert(hkey);
                    if (PointIsVein(csg, hp, mat)) cells.push_back(hp);
                }
            }
        }
        Log(L"probe: mat=%d cells=%d visited=%d skipped=%d step=%.1f maxDist=%.1f",
            mat, (int32_t)cells.size(), (int32_t)visited.size(), skipped, step, maxDist);
        return cells;
    }

    // Dig planning: no axis-aligned lattice. Fit the vein's dominant axis
    // (PCA). For elongated veins place dig points along the axis at 2*half
    // spacing - a string of cubes that follows the vein direction. Cells the
    // tube misses (curves, side branches) plus non-elongated blobs fall back
    // to greedy coverage: repeatedly pick the uncovered cell center that
    // covers the most remaining cells. Digs are ordered outward from the hit
    // point so the crater grows naturally.
    //
    // Coverage uses an axis-aligned box of full half-extent (DigSize/2): the
    // pickaxe crater is voxelized into a cube-ish hole, so the box matches
    // the real crater shape. Cells are bucketed by half-extent
    // so greedy coverage stays fast even with 2000+ cells.
    struct CellBuckets
    {
        float cell;
        std::unordered_map<int64_t, std::vector<int>> map;
        explicit CellBuckets(const std::vector<RawFVector>& p, float c) : cell(c)
        {
            for (size_t i = 0; i < p.size(); ++i) map[keyOf(p[i])].push_back((int)i);
        }
        static int64_t key3(int x, int y, int z)
        {
            return ((int64_t)x << 42) | ((int64_t)y << 21) | (int64_t)z;
        }
        int64_t keyOf(const RawFVector& p) const
        {
            return key3((int)std::floor(p.X / cell), (int)std::floor(p.Y / cell), (int)std::floor(p.Z / cell));
        }
        // Visit every cell index whose center may lie inside the box
        // [p - cell, p + cell]. Bucket size == box half-extent, so the box
        // spans at most 3x3x3 buckets (slack avoids float boundary misses).
        template <typename Fn>
        void forEach(const RawFVector& p, Fn&& fn) const
        {
            float e = cell * 1.01f;
            int x0 = (int)std::floor((p.X - e) / cell), x1 = (int)std::floor((p.X + e) / cell);
            int y0 = (int)std::floor((p.Y - e) / cell), y1 = (int)std::floor((p.Y + e) / cell);
            int z0 = (int)std::floor((p.Z - e) / cell), z1 = (int)std::floor((p.Z + e) / cell);
            for (int bx = x0; bx <= x1; ++bx)
            for (int by = y0; by <= y1; ++by)
            for (int bz = z0; bz <= z1; ++bz)
            {
                auto it = map.find(key3(bx, by, bz));
                if (it == map.end()) continue;
                for (int i : it->second) fn(i);
            }
        }
    };

    static std::vector<RawFVector> PlanDigPoints(const std::vector<RawFVector>& cells, const RawFVector& hit)
    {
        std::vector<RawFVector> digs;
        if (cells.empty()) return digs;
        float radius = (float)std::clamp(g_cfg_dig_radius, 60.0, 300.0);
        float maxDist = (float)std::clamp(g_cfg_max_distance, 150.0, 3000.0);
        float half = radius * 0.5f; // full box half-extent (crater is cube-ish)
        size_t n = cells.size();

        RawFVector c{0, 0, 0};
        for (size_t i = 0; i < n; ++i)
        {
            c.X += cells[i].X; c.Y += cells[i].Y; c.Z += cells[i].Z;
        }
        c.X /= (float)n; c.Y /= (float)n; c.Z /= (float)n;

        double cov[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
        for (size_t i = 0; i < n; ++i)
        {
            double dx = cells[i].X - c.X, dy = cells[i].Y - c.Y, dz = cells[i].Z - c.Z;
            cov[0][0] += dx*dx; cov[0][1] += dx*dy; cov[0][2] += dx*dz;
            cov[1][0] += dy*dx; cov[1][1] += dy*dy; cov[1][2] += dy*dz;
            cov[2][0] += dz*dx; cov[2][1] += dz*dy; cov[2][2] += dz*dz;
        }
        double v[3] = {1.0, 0.0, 0.0};
        for (int iter = 0; iter < 16; ++iter)
        {
            double w[3] = {
                cov[0][0]*v[0] + cov[0][1]*v[1] + cov[0][2]*v[2],
                cov[1][0]*v[0] + cov[1][1]*v[1] + cov[1][2]*v[2],
                cov[2][0]*v[0] + cov[2][1]*v[1] + cov[2][2]*v[2]
            };
            double len = std::sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
            if (len < 1e-9) break;
            v[0] = w[0]/len; v[1] = w[1]/len; v[2] = w[2]/len;
        }
        RawFVector axis{ (float)v[0], (float)v[1], (float)v[2] };

        double tMin = 1e30, tMax = -1e30, rMax = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            double dx = cells[i].X - c.X, dy = cells[i].Y - c.Y, dz = cells[i].Z - c.Z;
            double t = dx*v[0] + dy*v[1] + dz*v[2];
            if (t < tMin) tMin = t;
            if (t > tMax) tMax = t;
            double px = dx - v[0]*t, py = dy - v[1]*t, pz = dz - v[2]*t;
            double pr = std::sqrt(px*px + py*py + pz*pz);
            if (pr > rMax) rMax = pr;
        }
        double spread = tMax - tMin;
        bool elongated = spread > 2.0 * rMax && spread > 120.0;

        std::vector<uint8_t> covered(n, 0);
        size_t coveredCount = 0;
        CellBuckets buckets(cells, half);

        auto boxCovers = [&](const RawFVector& p, int i) -> bool {
            const RawFVector& q = cells[i];
            return std::fabs(p.X - q.X) <= half &&
                   std::fabs(p.Y - q.Y) <= half &&
                   std::fabs(p.Z - q.Z) <= half;
        };
        auto coversAny = [&](const RawFVector& p) -> bool {
            bool any = false;
            buckets.forEach(p, [&](int i) {
                if (!covered[i] && boxCovers(p, i)) any = true;
            });
            return any;
        };
        auto markCovered = [&](const RawFVector& p) {
            buckets.forEach(p, [&](int i) {
                if (covered[i]) return;
                if (boxCovers(p, i)) { covered[i] = 1; ++coveredCount; }
            });
        };
        float excludeR = half * 0.85f; // manual dig already carved this cube
        auto inRange = [&](const RawFVector& p) -> bool {
            float dx = p.X - hit.X, dy = p.Y - hit.Y, dz = p.Z - hit.Z;
            if (dx*dx + dy*dy + dz*dz < excludeR*excludeR) return false;
            return std::sqrt((double)(dx*dx + dy*dy + dz*dz)) <= maxDist;
        };
        auto addDig = [&](const RawFVector& p) {
            if (!inRange(p)) return;
            digs.push_back(p);
            markCovered(p);
        };

        if (elongated)
        {
            float spacing = half * 2.0f; // adjacent AABBs tile without gaps
            int steps = (int)std::ceil(spread / (double)spacing);
            if (steps > 240) steps = 240;
            for (int s = 0; s <= steps && coveredCount < n; ++s)
            {
                double t = tMin + spread * ((double)s / (double)steps);
                RawFVector p{ c.X + axis.X*(float)t, c.Y + axis.Y*(float)t, c.Z + axis.Z*(float)t };
                if (!coversAny(p)) continue;
                addDig(p);
            }
        }

        // Greedy coverage for cells the tube missed (also the whole plan for
        // blob-shaped veins). Each dig covers the most remaining cells.
        int maxDigs = 240;
        while (coveredCount < n && (int)digs.size() < maxDigs)
        {
            size_t best = (size_t)-1;
            int bestCount = -1;
            for (size_t i = 0; i < n; ++i)
            {
                if (covered[i]) continue;
                int cnt = 0;
                const RawFVector& p = cells[i];
                buckets.forEach(p, [&](int j) {
                    if (!covered[j] && boxCovers(p, j)) ++cnt;
                });
                if (cnt > bestCount) { bestCount = cnt; best = i; }
            }
            if (best == (size_t)-1 || bestCount <= 0) break;
            const RawFVector& bp = cells[best];
            if (inRange(bp))
            {
                digs.push_back(bp);
                markCovered(bp);
            }
            else
            {
                // inside the manual dig's exclusion cube or beyond MaxDistance:
                // never diggable, drop it so the loop always progresses.
                covered[best] = 1;
                ++coveredCount;
            }
        }

        std::sort(digs.begin(), digs.end(), [&hit](const RawFVector& a, const RawFVector& b) {
            double da = (double)(a.X-hit.X)*(a.X-hit.X) + (double)(a.Y-hit.Y)*(a.Y-hit.Y) + (double)(a.Z-hit.Z)*(a.Z-hit.Z);
            double db = (double)(b.X-hit.X)*(b.X-hit.X) + (double)(b.Y-hit.Y)*(b.Y-hit.Y) + (double)(b.Z-hit.Z)*(b.Z-hit.Z);
            return da < db;
        });
        return digs;
    }

    // Spline planning: fit a straight Hermite tube along the vein's
    // dominant axis (PCA). Only "body" cells (confirmed inside solid terrain)
    // drive the radius so the tube hugs the real vein instead of the probe's
    // air-margin shell (which previously inflated it). Radius = body radius
    // + 25cm + 15% slack, never below 85% of the full shell radius; length
    // covers the whole cell extent plus a small end margin.
    static SplinePlan PlanSpline(const std::vector<RawFVector>& cells, AActor* csg)
    {
        SplinePlan plan;
        plan.valid = false;
        if (cells.size() < 3) return plan;

        std::vector<RawFVector> body;
        if (g_insideReady)
        {
            body.reserve(cells.size());
            for (size_t i = 0; i < cells.size(); ++i)
                if (CallIsPointInsideTerrain(csg, cells[i])) body.push_back(cells[i]);
        }
        const std::vector<RawFVector>& fit = body.size() >= 3 ? body : cells;

        RawFVector c{0, 0, 0};
        for (size_t i = 0; i < fit.size(); ++i)
        {
            c.X += fit[i].X; c.Y += fit[i].Y; c.Z += fit[i].Z;
        }
        c.X /= (float)fit.size(); c.Y /= (float)fit.size(); c.Z /= (float)fit.size();

        double cov[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
        for (size_t i = 0; i < fit.size(); ++i)
        {
            double dx = fit[i].X - c.X, dy = fit[i].Y - c.Y, dz = fit[i].Z - c.Z;
            cov[0][0] += dx*dx; cov[0][1] += dx*dy; cov[0][2] += dx*dz;
            cov[1][0] += dy*dx; cov[1][1] += dy*dy; cov[1][2] += dy*dz;
            cov[2][0] += dz*dx; cov[2][1] += dz*dy; cov[2][2] += dz*dz;
        }
        double v[3] = {1.0, 0.0, 0.0};
        for (int iter = 0; iter < 16; ++iter)
        {
            double w[3] = {
                cov[0][0]*v[0] + cov[0][1]*v[1] + cov[0][2]*v[2],
                cov[1][0]*v[0] + cov[1][1]*v[1] + cov[1][2]*v[2],
                cov[2][0]*v[0] + cov[2][1]*v[1] + cov[2][2]*v[2]
            };
            double len = std::sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
            if (len < 1e-9) break;
            v[0] = w[0] / len; v[1] = w[1] / len; v[2] = w[2] / len;
        }
        RawFVector axis{ (float)v[0], (float)v[1], (float)v[2] };

        double margin = std::clamp(g_cfg_tube_margin, 15.0, 300.0);
        double tMinA = 1e30, tMaxA = -1e30, rMaxAll = 0.0;
        for (size_t i = 0; i < cells.size(); ++i)
        {
            double dx = cells[i].X - c.X, dy = cells[i].Y - c.Y, dz = cells[i].Z - c.Z;
            double t = dx*v[0] + dy*v[1] + dz*v[2];
            if (t < tMinA) tMinA = t;
            if (t > tMaxA) tMaxA = t;
            double px = dx - v[0]*t, py = dy - v[1]*t, pz = dz - v[2]*t;
            double pr = std::sqrt(px*px + py*py + pz*pz);
            if (pr > rMaxAll) rMaxAll = pr;
        }
        double rMaxBody = 0.0;
        if (!body.empty())
        {
            for (size_t i = 0; i < body.size(); ++i)
            {
                double dx = body[i].X - c.X, dy = body[i].Y - c.Y, dz = body[i].Z - c.Z;
                double t = dx*v[0] + dy*v[1] + dz*v[2];
                double px = dx - v[0]*t, py = dy - v[1]*t, pz = dz - v[2]*t;
                double pr = std::sqrt(px*px + py*py + pz*pz);
                if (pr > rMaxBody) rMaxBody = pr;
            }
        }

        double halfLen = std::max(std::fabs(tMinA), std::fabs(tMaxA)) + std::min(margin, 40.0);
        double fitR = std::max(rMaxBody * 1.15 + 25.0, rMaxAll * 0.85);
        plan.radius = (float)std::clamp(fitR, 55.0, g_cfg_max_spline_radius);
        RawFVector axisHalf{ axis.X * (float)halfLen, axis.Y * (float)halfLen, axis.Z * (float)halfLen };
        plan.start.X = c.X - axisHalf.X;
        plan.start.Y = c.Y - axisHalf.Y;
        plan.start.Z = c.Z - axisHalf.Z;
        plan.end.X = c.X + axisHalf.X;
        plan.end.Y = c.Y + axisHalf.Y;
        plan.end.Z = c.Z + axisHalf.Z;
        RawFVector tan{ axisHalf.X * 2.0f, axisHalf.Y * 2.0f, axisHalf.Z * 2.0f };
        plan.startTan = tan;
        plan.endTan = tan;
        plan.valid = true;
        return plan;
    }

    // Spline carve: build FSplineSegmentCarveOperationData and call the
    // game's TerrainOp_CarveSplineSegment (NetMulticast Reliable -> host + all
    // clients). Parameters mirror the game's own pipeline carve
    // (BP_Pipeline_Segment -> CarveWithSplineSegment): Material = nullptr
    // ("hollow out"), CarveFilter = ReplaceAll(0), Precious = TurnIntoGems
    // converts ore to gems. (An EmptyTerrainMaterial + ReplaceSolid combo
    // would make the engine FILL the cells with a placeholder material,
    // leaving black "unknown material" blobs - never use that.)
    static void ClearDebrisInSphere(AActor* csg, const RawFVector& pos, float radius)
    {
        if (!g_debrisReady || !g_fnRemoveDebris) return;
        uint8_t parms[64] = {0};
        if (g_offDebrisPos >= 0) *(RawFVector*)(parms + g_offDebrisPos) = pos;
        if (g_offDebrisRadius >= 0) *(float*)(parms + g_offDebrisRadius) = radius;
        if (g_offDebrisFragile >= 0) *(bool*)(parms + g_offDebrisFragile) = false;
        if (g_offDebrisDurable >= 0) *(bool*)(parms + g_offDebrisDurable) = true;
        if (g_offDebrisType >= 0) *(uint8_t*)(parms + g_offDebrisType) = 0; // ESpecialDebrisType::None
        csg->ProcessEvent(g_fnRemoveDebris, parms);
    }

    // Spline carves do not clean ore-surface decorations (crystal/rock
    // placements) the way a pickaxe dig does, so sweep the tube afterwards.
    static void ClearSplineDebris(AActor* csg, const SplinePlan& plan)
    {
        if (!g_debrisReady) return;
        float dx = plan.end.X - plan.start.X;
        float dy = plan.end.Y - plan.start.Y;
        float dz = plan.end.Z - plan.start.Z;
        float len = std::sqrt((double)(dx*dx + dy*dy + dz*dz));
        if (len < 1.0f)
        {
            ClearDebrisInSphere(csg, plan.start, plan.radius);
            return;
        }
        int steps = (int)(len / 40.0f) + 1;
        if (steps > 64) steps = 64;
        for (int i = 0; i <= steps; ++i)
        {
            float t = (float)i / (float)steps;
            RawFVector p{ plan.start.X + dx*t, plan.start.Y + dy*t, plan.start.Z + dz*t };
            ClearDebrisInSphere(csg, p, plan.radius);
        }
    }

    static void FireSplineCarve(AActor* csg, const SplinePlan& plan)
    {
        if (!g_fnSplineOp || !g_splineOpStruct || !g_splineSegStruct) return;
        if (g_offSplineOpNum < 0 || g_offSplineSegs < 0 || g_offSplineMat < 0 ||
            g_offSplineFilter < 0 || g_offSplinePrecious < 0) return;

        alignas(16) static uint8_t segBuf[64];
        memset(segBuf, 0, sizeof(segBuf));
        float radius = std::clamp(plan.radius, 55.0f, (float)g_cfg_max_spline_radius);
        if (g_offSegStart >= 0) *(RawFVector*)(segBuf + g_offSegStart) = plan.start;
        if (g_offSegStartTan >= 0) *(RawFVector*)(segBuf + g_offSegStartTan) = plan.startTan;
        if (g_offSegEnd >= 0) *(RawFVector*)(segBuf + g_offSegEnd) = plan.end;
        if (g_offSegEndTan >= 0) *(RawFVector*)(segBuf + g_offSegEndTan) = plan.endTan;
        if (g_offSegRadius >= 0) *(float*)(segBuf + g_offSegRadius) = radius;

        uint8_t parms[0x40] = {0};
        int32_t base = g_offSplineParam >= 0 ? g_offSplineParam : 0;
        *(int32_t*)(parms + base + g_offSplineOpNum) = ++g_lastOpNumber;
        RawTArray* arr = (RawTArray*)(parms + base + g_offSplineSegs);
        arr->Data = (void**)segBuf;  // UE TArray<FCarveSplineSegment>::Data is just the element storage pointer
        arr->Num = 1;
        arr->Max = 1;
        *(void**)(parms + base + g_offSplineMat) = nullptr;   // hollow out (game pipeline carve passes NoObject())
        *(uint8_t*)(parms + base + g_offSplineFilter) = 0;    // ECarveFilterType::ReplaceAll (game pipeline carve)
        *(uint8_t*)(parms + base + g_offSplinePrecious) = 0;  // EPreciousMaterialOptions::TurnIntoGems
        if (g_offSplineLevelGen >= 0) *(void**)(parms + base + g_offSplineLevelGen) = nullptr;

        csg->ProcessEvent(g_fnSplineOp, parms);
        ClearSplineDebris(csg, plan);
        ++g_splineFireCount;
        Log(L"spline: fired #%d op=%d start=(%.1f,%.1f,%.1f) end=(%.1f,%.1f,%.1f) r=%.1f",
            g_splineFireCount, g_lastOpNumber, plan.start.X, plan.start.Y, plan.start.Z,
            plan.end.X, plan.end.Y, plan.end.Z, radius);
    }

    // Chain dig: re-enter the game's own Server_DigBlock so the game fills in
    // OperationNumber / Miner / DigSize and replicates exactly like a real
    // pickaxe swing. Our pre-hook re-entrancy is blocked by g_inChain.
    static void ChainDigAt(AActor* pickaxe, const RawFVector& pos, const RawFVector& dir, int32_t matIdx, bool special)
    {
        uint8_t parms[32] = {0};
        *(RawFVector*)(parms + g_offCarvePos) = pos;
        *(RawFVector*)(parms + g_offCarveDir) = dir;
        *(int32_t*)(parms + g_offTerrainMaterial) = matIdx;
        if (g_offIsSpecial >= 0) *(bool*)(parms + g_offIsSpecial) = special;
        pickaxe->ProcessEvent(g_fnDigBlock, parms);
    }

    // RAII re-entrancy guard (SEH exceptions are reset in the __except handler)
    struct ChainGuard
    {
        ChainGuard() { g_inChain = true; }
        ~ChainGuard() { g_inChain = false; }
    };
    static void OnDigBlockImpl(AActor* ctx, uint8_t* parms)
    {
        if (!g_ready || !g_cfg_enabled) return;
        if (g_inChain) return;   // nested chain digs
        if (!parms) return;

        if (g_offCarvePos < 0 || g_offCarveDir < 0 || g_offTerrainMaterial < 0) return;
        RawFVector carvePos = *(RawFVector*)(parms + g_offCarvePos);
        RawFVector carveDir = *(RawFVector*)(parms + g_offCarveDir);
        int32_t hitMat = *(int32_t*)(parms + g_offTerrainMaterial);
        bool isSpecial = g_offIsSpecial >= 0 ? *(bool*)(parms + g_offIsSpecial) : false;
        if (!g_cfg_also_special && isSpecial) return;

        UWorld* world = ctx->GetWorld();
        if (!world) return;

        // Client chaining (EnableAsClient, off by default) re-enters the
        // Server_DigBlock RPC from the client, Dig mode only; Spline needs
        // authority and stays host-only.
        bool isAuthority = true;
        if (void* rolePtr = ctx->GetValuePtrByPropertyNameInChain(L"Role"))
        {
            isAuthority = *(uint8_t*)rolePtr == 3; // ROLE_Authority
        }
        if (!isAuthority)
        {
            if (!g_cfg_enable_as_client)
            {
                if (!g_warnedClientDisabled)
                {
                    g_warnedClientDisabled = true;
                    Log(L"chain: client game - chain disabled (set EnableAsClient=true to enable, Dig only)");
                }
                return;
            }
            if (!g_warnedClientDigOnly)
            {
                g_warnedClientDigOnly = true;
                Log(L"chain: client mode - Dig only (Spline carve needs host authority)");
            }
        }

        AActor* csg = FindDeepCSGWorld(world);
        if (!csg) return;

        // material gate: only chain when the hit material is a resource (ore)
        if (g_offCsgTerrainMaterials < 0 || g_offCollMaterials < 0) return;
        void* collection = *(void**)((uint8_t*)csg + g_offCsgTerrainMaterials);
        if (!collection) return;
        RawTArray* mats = (RawTArray*)((uint8_t*)collection + g_offCollMaterials);
        if (!mats || hitMat < 0 || hitMat >= mats->Num) return;
        void* mat = mats->Data ? mats->Data[hitMat] : nullptr;
        if (!mat) return;
        if (g_offMatResourceData < 0 || *(void**)((uint8_t*)mat + g_offMatResourceData) == nullptr)
        {
            if (!g_warnedNotOreMat)
            {
                g_warnedNotOreMat = true;
                Log(L"chain: hit material idx=%d has no resource data, skipping", hitMat);
            }
            return;
        }
        g_warnedNotOreMat = false;

        int32_t maxNodes = std::clamp<int32_t>(g_cfg_max_nodes, 0, 2000);
        if (maxNodes <= 0) return;
        if (!g_raycastReady)
        {
            if (!g_warnedRayFallback)
            {
                g_warnedRayFallback = true;
                Log(L"chain: Raycast not ready yet, deferring chain (first swings may not chain)");
            }
            return;
        }

        AActor* pickaxe = ctx;
        ChainGuard guard;
        // A swing while the queue is still draining no longer cancels the
        // pending digs (that left corner ore behind when players swing in
        // quick succession). Merge instead: keep draining, append the new
        // plan with dedup.
        bool queueBusy = !g_pendingDigs.empty();
        if (!queueBusy) ClearPendingQueue();
        std::wstring matName = ((UObject*)mat)->GetName();

        if (g_cfg_mode == 1 && isAuthority)
        {
            // Spline mode: probe the vein, fit the tube, defer one carve op to
            // the next tick (never shares the manual dig's frame).
            if (!g_splineReady) return;
            std::vector<RawFVector> cells = ProbeVeinCells(csg, carvePos, carveDir, hitMat);
            if (cells.empty())
            {
                Log(L"spline: no vein cells found near (%.1f,%.1f,%.1f)", carvePos.X, carvePos.Y, carvePos.Z);
                return;
            }
            SplinePlan plan = PlanSpline(cells, csg);
            if (!plan.valid)
            {
                Log(L"spline: too few cells (%d) to fit a tube", (int32_t)cells.size());
                return;
            }
            g_pendingSpline.csg = csg;
            g_pendingSpline.plan = plan;
            g_pendingSpline.valid = true;
            double len = std::sqrt((double)((plan.end.X-plan.start.X)*(plan.end.X-plan.start.X) +
                                            (plan.end.Y-plan.start.Y)*(plan.end.Y-plan.start.Y) +
                                            (plan.end.Z-plan.start.Z)*(plan.end.Z-plan.start.Z)));
            Log(L"spline: enqueue ore=%ls idx=%d cells=%d radius=%.1f len=%.1f",
                matName.c_str(), hitMat, (int32_t)cells.size(), plan.radius, len);
            return;
        }

        // Dig mode: probe the whole vein, plan optimal digs, fire one per frame.
        std::vector<RawFVector> cells = ProbeVeinCells(csg, carvePos, carveDir, hitMat);
        std::vector<RawFVector> digs = PlanDigPoints(cells, carvePos);
        if (digs.empty())
        {
            Log(L"chain: probed %d cells but no dig points (spacing too large?)", (int32_t)cells.size());
            return;
        }
        float dedupR = queueBusy ? (float)std::clamp(g_cfg_dig_radius, 60.0, 300.0) * 0.5f * 0.9f : 0.0f;
        size_t added = 0;
        for (size_t i = 0; i < digs.size(); ++i)
        {
            if (EnqueueDig(pickaxe, digs[i], carveDir, hitMat, isSpecial, dedupR)) ++added;
        }
        Log(L"chain: enqueue ore=%ls idx=%d cells=%d digs=%d queue=%d%s",
            matName.c_str(), hitMat, (int32_t)cells.size(), (int32_t)added, (int32_t)g_pendingDigs.size(),
            queueBusy ? L" merged" : L"");
    }

    // Global-ProcessEvent detector for Server_DigBlock. A UFunction FuncPtr
    // hook only fires when the function body executes (on the host); this
    // also fires for the client's own outgoing RPCs, before they are sent.
    static void OnDigDetect(UObject* Context, UFunction* Function, void* Parms)
    {
        if (Function != g_fnDigBlock) return;
        if (!g_ready || !g_cfg_enabled) return;
        if (g_inChain) return;
        if (!Parms) return;
        __try { OnDigBlockImpl((AActor*)Context, (uint8_t*)Parms); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_inChain = false;
            Log(L"hook: Server_DigBlock exception 0x%08X", GetExceptionCode());
        }
    }
    // =====================================================================
    // Deferred dig queue: BFS points are enqueued on the swing, then fired
    // one per FireIntervalMs from a game-thread ProcessEvent pre-callback so
    // every dig is a fresh, non-reentrant Server_DigBlock (one CSG commit per
    // frame, like a real pickaxe swing - the game drops every op except the
    // last one when 100 digs are fired in a single frame).
    // =====================================================================
    static void ClearPendingQueue()
    {
        g_pendingDigs.clear();
        g_pendingHead = 0;
        g_pendingSpline.valid = false;
        // stamp now so the first deferred dig waits a full interval (i.e. a
        // separate frame) after the manual swing - never shares the manual
        // op frame.
        g_lastFireTick = GetTickCount64();
        g_firedTotal = 0;
    }

    static bool EnqueueDig(AActor* pickaxe, const RawFVector& pos, const RawFVector& dir, int32_t mat, bool special, float dedupR)
    {
        if (!pickaxe) return false;
        if (dedupR > 0.0f)
        {
            float r2 = dedupR * dedupR;
            for (size_t i = 0; i < g_pendingDigs.size(); ++i)
            {
                const RawFVector& q = g_pendingDigs[i].pos;
                float dx = q.X - pos.X, dy = q.Y - pos.Y, dz = q.Z - pos.Z;
                if (dx*dx + dy*dy + dz*dz < r2) return false;
            }
        }
        g_pendingDigs.push_back(PendingDig{ pickaxe, pos, dir, mat, special });
        return true;
    }

    static void DrainChainQueue()
    {
        if (g_pendingDigs.empty()) return;
        if (!g_ready || !g_cfg_enabled) { ClearPendingQueue(); return; }
        int32_t interval = std::clamp<int32_t>(g_cfg_fire_interval_ms, 5, 500);
        uint64_t now = GetTickCount64();
        if (g_lastFireTick != 0 && now - g_lastFireTick < (uint64_t)interval) return;
        if (g_pendingHead >= g_pendingDigs.size()) { ClearPendingQueue(); return; }
        PendingDig& d = g_pendingDigs[g_pendingHead++];
        ChainGuard guard;
        if (d.pickaxe && d.pickaxe->GetWorld() == g_cachedWorld)
        {
            ChainDigAt(d.pickaxe, d.pos, d.dir, d.mat, d.special);
            ++g_firedTotal;
        }
        g_lastFireTick = GetTickCount64();
        if (g_pendingHead >= g_pendingDigs.size())
        {
            Log(L"chain: done fired=%d", g_firedTotal);
            ClearPendingQueue();
        }
    }

    // Fire the deferred spline carve (one op, on its own frame like every
    // terrain commit). Called from the game-thread tick.
    static void FirePendingSpline()
    {
        if (!g_pendingSpline.valid) return;
        PendingSpline job = g_pendingSpline;
        g_pendingSpline.valid = false;
        if (!job.csg || job.csg->GetWorld() != g_cachedWorld) return;
        if (!g_ready || !g_cfg_enabled) return;
        FireSplineCarve(job.csg, job.plan);
    }

    // Game-thread tick: cheap (returns instantly when queue is empty); fires
    // one deferred dig per interval (or the single deferred spline op).
    // Skipped while g_inChain so it cannot re-enter during a manual dig or a
    // previous drain.
    static void ChainTickCallback(UObject* Context, UFunction* Function, void* Parms)
    {
        if (g_inChain) return;
        // Dig detection first: fires on the host for local and remote digs,
        // and on a client for its own outgoing dig RPCs (see OnDigDetect).
        if (Function == g_fnDigBlock)
        {
            OnDigDetect(Context, Function, Parms);
        }
        if (g_pendingDigs.empty() && !g_pendingSpline.valid) return;
        __try
        {
            if (g_pendingSpline.valid)
            {
                int32_t interval = std::clamp<int32_t>(g_cfg_fire_interval_ms, 5, 500);
                uint64_t now = GetTickCount64();
                if (g_lastFireTick == 0 || now - g_lastFireTick >= (uint64_t)interval)
                {
                    g_lastFireTick = now;
                    FirePendingSpline();
                }
            }
            DrainChainQueue();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_inChain = false;
            ClearPendingQueue();
            Log(L"chain: drain exception 0x%08X", GetExceptionCode());
        }
    }

    // =====================================================================
    // TerrainOp_PickAxe pre-hook: track the latest OperationNumber so spline
    // carves number their ops above every dig the game has committed.
    // =====================================================================
    static void OnTerrainOpPickaxeImpl(UnrealScriptFunctionCallableContext& ctx)
    {
        if (!g_fnLocals) return;
        uint8_t*& localsRef = g_fnLocals(&ctx.TheStack);
        uint8_t* parms = localsRef;
        if (!parms) return;

        int32_t opNum = *(int32_t*)(parms + 0x00);
        if (opNum > g_lastOpNumber) g_lastOpNumber = opNum;
    }

    static void OnTerrainOpPickaxe(UnrealScriptFunctionCallableContext& ctx, void*)
    {
        __try { OnTerrainOpPickaxeImpl(ctx); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log(L"hook: TerrainOp_PickAxe exception 0x%08X", GetExceptionCode());
        }
    }
    // =====================================================================
    // Game-thread setup driver (all UE reflection on the game thread)
    // =====================================================================
    static bool g_inSetup = false;
    static uint64_t g_lastSetupTick = 0;

    static void GameThreadSetupImpl()
    {
        TryReloadConfig();
        if (!g_ready) TryRegisterHooks();
    }

    static void GameThreadSetupCallback(UObject*, UFunction*, void*)
    {
        uint64_t now = GetTickCount64();
        if (now - g_lastSetupTick < 500) return;
        if (g_inSetup) return;
        g_inSetup = true;
        g_lastSetupTick = now;
        __try
        {
            GameThreadSetupImpl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log(L"setup: exception 0x%08X", GetExceptionCode());
        }
        g_inSetup = false;
    }

    // =====================================================================
    // Mod class
    // =====================================================================
    class MyMod : public RC::CppUserModBase
    {
    public:
        MyMod()
        {
            ModName = L"Sakura_CPP_ChainMine";
            ModVersion = L"1.2";
            ModDescription = L"Chain mining: probe whole ore vein, optimal digs (or spline carve), host side";
            ModAuthors = L"Sakura";
            InitLogPath();
            Log(L"=== Sakura_CPP_ChainMine loaded ===");
            if (GetFileAttributesW(g_cfg_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                WriteDefaultConfig();
            }
            TryReloadConfig();
        }

        ~MyMod() override
        {
            Log(L"mod destroyed, unregistering hooks");
            if (g_opNumHookId && g_fnTerrainOpPickAxe)
            {
                g_fnTerrainOpPickAxe->UnregisterHook(g_opNumHookId);
                g_opNumHookId = 0;
            }
            ClearPendingQueue();
        }

        void on_program_start() override
        {
            Log(L"on_program_start called");
            RC::Unreal::Hook::RegisterProcessEventPreCallback(GameThreadSetupCallback);
            Log(L"setup: game-thread ProcessEvent hook registered");
            RC::Unreal::Hook::RegisterProcessEventPreCallback(ChainTickCallback);
            Log(L"setup: chain drain tick callback registered");
        }

        void on_unreal_init() override
        {
            // All UE work runs from the game-thread ProcessEvent callback.
        }

        void on_update() override
        {
            // Intentionally empty: setup/config moved to the game thread.
        }
    };
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ChainMine::g_hmod = hModule;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new ChainMine::MyMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
