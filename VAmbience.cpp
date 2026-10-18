// VAmbience for GTA IV: Complete Edition (1.2.0.59) - plugin-sdk port
//
// Original mod: VAmbience 1.0 by ItsClonkAndre (https://github.com/ClonkAndre/VAmbience)
// This is a port of the original ScriptHook-style source to a native .asi built on
// plugin-sdk (GTAIV / PLUGIN_SGV_CE / RAGE). It runs from Events::gameProcessEvent
// (once per frame, main thread) and calls the game's script natives through the
// SDK's Command<> wrapper.
//
// Build defines (set by the plugin-sdk project generator for Plugin_IV):  GTAIV;PLUGIN_SGV_CE;RAGE
// Language: C++17, 32-bit (Win32).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "plugin.h"
#include "extensions/ScriptCommands.h"
#include "CMenuManager.h" // aPrefs[]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace plugin;
namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------
// Minimal BASS binding (bass.dll is loaded at runtime, so no bass.lib / bass.h is required).
// Constants and signatures are those of BASS 2.4 (32-bit, stdcall).
// ------------------------------------------------------------------------------------------
namespace bass {
    typedef DWORD HSTREAM;
    struct VEC3 { float x, y, z; };

    constexpr DWORD DEVICE_3D       = 4;
    constexpr DWORD SAMPLE_MONO     = 2;
    constexpr DWORD SAMPLE_3D       = 8;
    constexpr DWORD STREAM_PRESCAN  = 0x20000;
    constexpr DWORD STREAM_AUTOFREE = 0x40000;
    constexpr DWORD UNICODE_NAMES   = 0x80000000; // BASS_UNICODE
    constexpr DWORD ATTRIB_VOL      = 2;
    constexpr DWORD ACTIVE_STOPPED  = 0;
    constexpr DWORD ACTIVE_PLAYING  = 1;
    constexpr int   ERROR_ALREADY   = 14;

    static HMODULE dll = nullptr;

#define BASS_FN(ret, name, args) typedef ret (WINAPI *name##_t) args; static name##_t name = nullptr;
    BASS_FN(BOOL,    BASS_Init,                  (int, DWORD, DWORD, HWND, const void*))
    BASS_FN(BOOL,    BASS_Free,                  (void))
    BASS_FN(int,     BASS_ErrorGetCode,          (void))
    BASS_FN(HSTREAM, BASS_StreamCreateFile,      (BOOL, const void*, unsigned long long, unsigned long long, DWORD))
    BASS_FN(BOOL,    BASS_StreamFree,            (HSTREAM))
    BASS_FN(BOOL,    BASS_ChannelPlay,           (DWORD, BOOL))
    BASS_FN(BOOL,    BASS_ChannelPause,          (DWORD))
    BASS_FN(BOOL,    BASS_ChannelStop,           (DWORD))
    BASS_FN(DWORD,   BASS_ChannelIsActive,       (DWORD))
    BASS_FN(BOOL,    BASS_ChannelSetAttribute,   (DWORD, DWORD, float))
    BASS_FN(BOOL,    BASS_ChannelSlideAttribute, (DWORD, DWORD, float, DWORD))
    BASS_FN(BOOL,    BASS_ChannelIsSliding,      (DWORD, DWORD))
    BASS_FN(BOOL,    BASS_ChannelSet3DPosition,  (DWORD, const VEC3*, const VEC3*, const VEC3*))
    BASS_FN(BOOL,    BASS_Set3DPosition,         (const VEC3*, const VEC3*, const VEC3*, const VEC3*))
    BASS_FN(BOOL,    BASS_Set3DFactors,          (float, float, float))
    BASS_FN(void,    BASS_Apply3D,               (void))
#undef BASS_FN

    static bool Load(const fs::path& dllPath) {
        dll = LoadLibraryW(dllPath.wstring().c_str());
        if (!dll) dll = LoadLibraryW(L"bass.dll");
        if (!dll) return false;
        bool ok = true;
#define BASS_GET(name) name = reinterpret_cast<name##_t>(GetProcAddress(dll, #name)); ok = ok && name != nullptr;
        BASS_GET(BASS_Init) BASS_GET(BASS_Free) BASS_GET(BASS_ErrorGetCode)
        BASS_GET(BASS_StreamCreateFile) BASS_GET(BASS_StreamFree)
        BASS_GET(BASS_ChannelPlay) BASS_GET(BASS_ChannelPause) BASS_GET(BASS_ChannelStop)
        BASS_GET(BASS_ChannelIsActive) BASS_GET(BASS_ChannelSetAttribute)
        BASS_GET(BASS_ChannelSlideAttribute) BASS_GET(BASS_ChannelIsSliding)
        BASS_GET(BASS_ChannelSet3DPosition) BASS_GET(BASS_Set3DPosition)
        BASS_GET(BASS_Set3DFactors) BASS_GET(BASS_Apply3D)
#undef BASS_GET
        return ok;
    }
}

// ------------------------------------------------------------------------------------------
// Game-side helpers
// ------------------------------------------------------------------------------------------
namespace {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    bool IsZero() const { return x == 0.0f && y == 0.0f && z == 0.0f; }
};

enum class Zone { Alderney, HappinessIsland, Algonquin, ChargeIsland, ColonyIsland, Bohan, BrokerDukes, LibertyCity };

// Index of STAT_DERRICK_MISSION_PROGRESS in the float stat list (plugin_IV/game_IV/CStats.h).
constexpr int STAT_DERRICK_MISSION_PROGRESS = 25;
// Index of the SFX volume slider in the game's menu preferences (CMenuManager.h: PREF_SFX_VOLUME).
// In the 1.2.0.59 exe the slider defaults to 9, i.e. the range is 0..10.
constexpr int PREF_SFX_VOLUME_INDEX = 25;
// Model hash of the "Aiden" secret ped (same value as the original mod).
constexpr uint32_t AIDEN_MODEL_HASH = 215190023;

const std::unordered_map<std::string, Zone> kZones = {
    {"WESDY", Zone::Alderney}, {"LEFWO", Zone::Alderney}, {"ALDCI", Zone::Alderney}, {"BERCH", Zone::Alderney},
    {"NORMY", Zone::Alderney}, {"ACTRR", Zone::Alderney}, {"PORTU", Zone::Alderney}, {"TUDOR", Zone::Alderney},
    {"ACTIP", Zone::Alderney}, {"ALSCF", Zone::Alderney},
    {"HAPIN", Zone::HappinessIsland},
    {"CASGR", Zone::Algonquin}, {"THXCH", Zone::Algonquin}, {"FISSO", Zone::Algonquin}, {"CHITO", Zone::Algonquin},
    {"CITH", Zone::Algonquin}, {"CASGC", Zone::Algonquin}, {"SUFFO", Zone::Algonquin}, {"LITAL", Zone::Algonquin},
    {"LOWEA", Zone::Algonquin}, {"FISSN", Zone::Algonquin}, {"THPRES", Zone::Algonquin}, {"EASON", Zone::Algonquin},
    {"THTRI", Zone::Algonquin}, {"TMEQU", Zone::Algonquin}, {"WESMI", Zone::Algonquin}, {"STARJ", Zone::Algonquin},
    {"LANCE", Zone::Algonquin}, {"HATGA", Zone::Algonquin}, {"PUGAT", Zone::Algonquin}, {"MIDPW", Zone::Algonquin},
    {"MIDPA", Zone::Algonquin}, {"MIDPE", Zone::Algonquin}, {"LANCA", Zone::Algonquin}, {"VASIH", Zone::Algonquin},
    {"NOHOL", Zone::Algonquin}, {"EAHOL", Zone::Algonquin}, {"NORWO", Zone::Algonquin},
    {"CHISL", Zone::ChargeIsland},
    {"COISL", Zone::ColonyIsland},
    {"STHBO", Zone::Bohan}, {"CHAPO", Zone::Bohan}, {"FORSI", Zone::Bohan}, {"BOULE", Zone::Bohan},
    {"NRTGA", Zone::Bohan}, {"INSTI", Zone::Bohan}, {"LTBAY", Zone::Bohan},
    {"STEIN", Zone::BrokerDukes}, {"MEADP", Zone::BrokerDukes}, {"FRANI", Zone::BrokerDukes}, {"WILLI", Zone::BrokerDukes},
    {"MEADH", Zone::BrokerDukes}, {"EISLC", Zone::BrokerDukes}, {"BOAB", Zone::BrokerDukes}, {"CERHE", Zone::BrokerDukes},
    {"BEECW", Zone::BrokerDukes}, {"SCHOL", Zone::BrokerDukes}, {"DOWTW", Zone::BrokerDukes}, {"ROTTH", Zone::BrokerDukes},
    {"ESHOO", Zone::BrokerDukes}, {"OUTL", Zone::BrokerDukes}, {"SUTHS", Zone::BrokerDukes}, {"HOBEH", Zone::BrokerDukes},
    {"FIREP", Zone::BrokerDukes}, {"FIISL", Zone::BrokerDukes}, {"BEGGA", Zone::BrokerDukes},
    {"BRALG", Zone::LibertyCity}, {"BRBRO", Zone::LibertyCity}, {"BREBB", Zone::LibertyCity}, {"BRDBB", Zone::LibertyCity},
    {"NOWOB", Zone::LibertyCity}, {"HIBRG", Zone::LibertyCity}, {"LEAPE", Zone::LibertyCity}, {"BOTU", Zone::LibertyCity},
    {"LIBERTY", Zone::LibertyCity},
};

// Script natives return their result in a 4 byte slot that shares memory with the parameters,
// so a "bool" result must only be read from its low byte.
inline bool AsBool(int v) { return (v & 0xFF) != 0; }

// ---- thin, typed wrappers over the natives we use ----
namespace nat {
    inline bool  IsPlayerPlaying(int playerIdx)   { return AsBool(Command<int, Commands::IS_PLAYER_PLAYING>(playerIdx)); }
    inline int   GetPlayerId()                    { return Command<int, Commands::GET_PLAYER_ID>(); }
    inline int   ConvertIntToPlayerIndex(int id)  { return Command<int, Commands::CONVERT_INT_TO_PLAYERINDEX>(id); }
    inline bool  DoesCharExist(uint32_t ped)      { return ped != 0 && AsBool(Command<int, Commands::DOES_CHAR_EXIST>(ped)); }
    inline bool  IsPauseMenuActive()              { return AsBool(Command<int, Commands::IS_PAUSE_MENU_ACTIVE>()); }
    inline bool  IsInteriorScene()                { return AsBool(Command<int, Commands::IS_INTERIOR_SCENE>()); }
    inline bool  HasCutsceneFinished()            { return AsBool(Command<int, Commands::HAS_CUTSCENE_FINISHED>()); }
    inline int   GetHoursOfDay()                  { return Command<int, Commands::GET_HOURS_OF_DAY>(); }
    inline float GetFloatStat(int stat)           { return Command<float, Commands::GET_FLOAT_STAT>(stat); }
    inline const char* GetNameOfZone(float x, float y, float z) { return Command<const char*, Commands::GET_NAME_OF_ZONE>(x, y, z); }

    inline uint32_t GetPlayerChar(int playerIdx) {
        uint32_t ped = 0;
        Command<void, Commands::GET_PLAYER_CHAR>(playerIdx, &ped);
        return ped;
    }
    inline Vec3 GetCharCoordinates(uint32_t ped) {
        float x = 0, y = 0, z = 0;
        Command<void, Commands::GET_CHAR_COORDINATES>(ped, &x, &y, &z);
        return { x, y, z };
    }
    inline Vec3 GetCharVelocity(uint32_t ped) {
        float x = 0, y = 0, z = 0;
        Command<void, Commands::GET_CHAR_VELOCITY>(ped, &x, &y, &z);
        return { x, y, z };
    }
    inline uint32_t GetRootCam() {
        uint32_t cam = 0;
        Command<void, Commands::GET_ROOT_CAM>(&cam);
        return cam;
    }
    inline Vec3 GetCamPos(uint32_t cam) {
        float x = 0, y = 0, z = 0;
        Command<void, Commands::GET_CAM_POS>(cam, &x, &y, &z);
        return { x, y, z };
    }
    inline Vec3 GetCamRot(uint32_t cam) {
        float x = 0, y = 0, z = 0;
        Command<void, Commands::GET_CAM_ROT>(cam, &x, &y, &z);
        return { x, y, z };
    }
    // Returns false if no node was found.
    inline bool GetNextPositionOnStreet(const Vec3& pos, Vec3& out) {
        uint32_t area = (uint32_t)Command<int, Commands::GET_MAP_AREA_FROM_COORDS>(pos.x, pos.y, pos.z);
        float x = 0, y = 0, z = 0, heading = 0;
        uint32_t unk = 0;
        int r = Command<int, Commands::GET_NTH_CLOSEST_CAR_NODE_WITH_HEADING_ON_ISLAND>(
            pos.x, pos.y, pos.z, 1, area, &x, &y, &z, &heading, &unk);
        if (!AsBool(r)) return false;
        out = { x, y, z };
        return true;
    }
    inline void LoadModel(uint32_t model) {
        if (!AsBool(Command<int, Commands::HAS_MODEL_LOADED>(model))) {
            Command<void, Commands::REQUEST_MODEL>(model);
            Command<void, Commands::LOAD_ALL_OBJECTS_NOW>();
        }
    }
    inline void UnloadModel(uint32_t model) {
        if (AsBool(Command<int, Commands::HAS_MODEL_LOADED>(model)))
            Command<void, Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(model);
    }
    inline void DeleteChar(uint32_t& ped) {
        if (DoesCharExist(ped)) Command<void, Commands::DELETE_CHAR>(&ped);
        ped = 0;
    }
}

// ------------------------------------------------------------------------------------------
// Settings (VAmbience.ini)
// ------------------------------------------------------------------------------------------
struct ZoneChances { int alderney, algonquin, bohan, brokerDukes, chargeIsland, colonyIsland, happinessIsland; };

struct Settings {
    int  globalMultiplier = 1;
    bool enable3D = true;
    bool playInCutscenes = true;

    bool dvEnabled = true;  int dvGlobal = 100; int dvSound = 2; int dvDistance = 120;
    ZoneChances dvChance{ 4, 5, 2, 2, 1, 1, 0 };

    bool dgsEnabled = true; int dgsGlobal = 100; int dgsSound = 2; int dgsDistance = 300;
    ZoneChances dgsChance{ 4, 1, 3, 4, 0, 0, 0 };

    bool seEnabled = true;  int seSound = 2;
};

std::string ReadIni(const std::string& file, const char* section, const char* key, const char* def) {
    char buf[64] = {};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), file.c_str());
    return buf;
}
int ReadIniInt(const std::string& file, const char* section, const char* key, int def) {
    // Parse ourselves so that bad values fall back to the default instead of throwing.
    std::string s = ReadIni(file, section, key, "");
    if (s.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return (end == s.c_str()) ? def : (int)v;
}

void LoadSettings(const std::string& ini, Settings& s) {
    s.globalMultiplier = ReadIniInt(ini, "Sound", "GlobalMultiplier", 1);
    s.enable3D         = ReadIniInt(ini, "Sound", "Enable3D", 1) != 0;
    s.playInCutscenes  = ReadIniInt(ini, "Sound", "PlayInCutscenes", 1) != 0;

    auto zones = [&](const char* sec, ZoneChances& z, const ZoneChances& d) {
        z.alderney        = ReadIniInt(ini, sec, "Alderney",        d.alderney);
        z.algonquin       = ReadIniInt(ini, sec, "Algonquin",       d.algonquin);
        z.bohan           = ReadIniInt(ini, sec, "Bohan",           d.bohan);
        z.brokerDukes     = ReadIniInt(ini, sec, "BrokerAndDukes",  d.brokerDukes);
        z.chargeIsland    = ReadIniInt(ini, sec, "ChargeIsland",    d.chargeIsland);
        z.colonyIsland    = ReadIniInt(ini, sec, "ColonyIsland",    d.colonyIsland);
        z.happinessIsland = ReadIniInt(ini, sec, "HappinessIsland", d.happinessIsland);
    };

    // The shipped ini spells the key "GlobalMuliplier" (sic); accept the correct spelling as well.
    auto global = [&](const char* sec) {
        int v = ReadIniInt(ini, sec, "GlobalMuliplier", 100);
        return ReadIniInt(ini, sec, "GlobalMultiplier", v);
    };

    s.dvEnabled  = ReadIniInt(ini, "DistantVehicles", "Enabled", 1) != 0;
    s.dvGlobal   = global("DistantVehicles");
    s.dvSound    = ReadIniInt(ini, "DistantVehicles", "SoundMultiplier", 2);
    s.dvDistance = ReadIniInt(ini, "DistantVehicles", "SoundDistance", 120);
    zones("DistantVehicles", s.dvChance, ZoneChances{ 4, 5, 2, 2, 1, 1, 0 });

    s.dgsEnabled  = ReadIniInt(ini, "DistantGunSounds", "Enabled", 1) != 0;
    s.dgsGlobal   = global("DistantGunSounds");
    s.dgsSound    = ReadIniInt(ini, "DistantGunSounds", "SoundMultiplier", 2);
    s.dgsDistance = ReadIniInt(ini, "DistantGunSounds", "SoundDistance", 300);
    zones("DistantGunSounds", s.dgsChance, ZoneChances{ 4, 1, 3, 4, 0, 0, 0 });

    s.seEnabled = ReadIniInt(ini, "Secrets", "Enabled", 1) != 0;
    s.seSound   = ReadIniInt(ini, "Secrets", "SoundMultiplier", 2);
}

int ChanceFor(const ZoneChances& c, Zone z) {
    switch (z) {
        case Zone::Alderney:        return c.alderney;
        case Zone::Algonquin:       return c.algonquin;
        case Zone::Bohan:           return c.bohan;
        case Zone::BrokerDukes:     return c.brokerDukes;
        case Zone::ChargeIsland:    return c.chargeIsland;
        case Zone::ColonyIsland:    return c.colonyIsland;
        case Zone::HappinessIsland: return c.happinessIsland;
        default:                    return 0; // LibertyCity (bridges/tunnels) has no entry in the original either
    }
}

// ------------------------------------------------------------------------------------------
// Sound library: which files exist (scanned once at startup, files are named 1.mp3 .. N.mp3)
// ------------------------------------------------------------------------------------------
struct SoundLibrary {
    fs::path root;                              // ...\VAmbiance
    int gunGroups = 0;     std::vector<int> gunFiles;      // [group-1] -> file count
    int vehicleGroups = 0; std::vector<int> vehicleFiles;
    int aidenFiles = 0;

    static int CountMp3(const fs::path& dir) {
        std::error_code ec;
        int n = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".mp3") n++;
        }
        return n;
    }
    static int CountGroups(const fs::path& dir, std::vector<int>& files) {
        std::error_code ec;
        int groups = 0;
        for (;; groups++) { // groups are numbered 1..N
            fs::path g = dir / std::to_string(groups + 1);
            if (!fs::is_directory(g, ec)) break;
            files.push_back(CountMp3(g));
        }
        return groups;
    }
    void Scan(const fs::path& r) {
        root = r;
        gunGroups     = CountGroups(root / "gun", gunFiles);
        vehicleGroups = CountGroups(root / "vehicle", vehicleFiles);
        aidenFiles    = CountMp3(root / "secret" / "aiden");
    }
};

// ------------------------------------------------------------------------------------------
// The mod
// ------------------------------------------------------------------------------------------
class VAmbience {
public:
    VAmbience() {
        Events::gameProcessEvent += [this] { OnTick(); };
        Events::shutdownEngineEvent += [this] { Shutdown(); };
    }

private:
    // ---- state ----
    Settings cfg;
    SoundLibrary lib;
    std::mt19937 rng{ std::random_device{}() };
    bool initTried = false, ready = false;

    Vec3 playerPos, playerVel, aidenPos{ -1561.919f, 1220.616f, 11.463f };
    uint32_t aidenPed = 0;
    float sfxVolume = 0.0f;

    bass::HSTREAM vehicleStream = 0, aidenStream = 0;
    bool wasPaused = false;
    bool vehiclePausedByUs = false, aidenPausedByUs = false;

    // distant gun burst (replaces the original's worker thread; runs on the game thread)
    struct GunBurst {
        bool active = false;
        int gun = 0, fired = 0, total = 0;
        Vec3 pos;
        ULONGLONG nextShot = 0;
    } burst;

    // ---- random helpers ----
    int RandInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }          // inclusive
    float RandFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

    // ---- init / shutdown ----
    static fs::path ModuleDir() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        return fs::path(path).parent_path();
    }
    static fs::path ExeDir() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    void Init() {
        initTried = true;

        // Works with Ultimate ASI Loader (dinput8.dll), which loads *.asi from the game folder and
        // from "scripts", "plugins" and "update". Resources are looked up next to the .asi first,
        // then in its parent folder, then in the game folder - the first folder that has them wins.
        fs::path asiDir = ModuleDir();
        std::vector<fs::path> dirs{ asiDir, asiDir.parent_path(), ExeDir() };
        std::error_code ec;
        auto firstWith = [&](const char* name, bool wantDir) {
            for (auto& d : dirs) {
                fs::path p = d / name;
                if (wantDir ? fs::is_directory(p, ec) : fs::is_regular_file(p, ec)) return d;
            }
            return ExeDir();
        };
        fs::path soundDir = firstWith("VAmbiance", true);
        fs::path iniDir   = firstWith("VAmbience.ini", false);
        fs::path bassDir  = firstWith("bass.dll", false);

        LoadSettings((iniDir / "VAmbience.ini").string(), cfg);
        lib.Scan(soundDir / "VAmbiance");

        if (!bass::Load(bassDir / "bass.dll")) return;
        if (!bass::BASS_Init(-1, 44100, bass::DEVICE_3D, nullptr, nullptr)) {
            if (bass::BASS_ErrorGetCode() != bass::ERROR_ALREADY) return;
        }
        ready = true;
    }

    void Shutdown() {
        if (!ready) return;
        ready = false;
        bass::BASS_Free();
    }

    // ---- BASS helpers ----
    static bass::HSTREAM LoadAudio(const fs::path& file, bool as3D) {
        DWORD flags = bass::STREAM_PRESCAN | bass::STREAM_AUTOFREE | bass::UNICODE_NAMES;
        if (as3D) flags |= bass::SAMPLE_MONO | bass::SAMPLE_3D;
        return bass::BASS_StreamCreateFile(FALSE, file.c_str(), 0, 0, flags);
    }
    static DWORD ActiveState(bass::HSTREAM s) { return s ? bass::BASS_ChannelIsActive(s) : bass::ACTIVE_STOPPED; }
    static bool IsPlaying(bass::HSTREAM s) { return ActiveState(s) == bass::ACTIVE_PLAYING; }

    // GTA (x east, y north, z up, right handed)  ->  BASS (x right, y up, z forward, left handed)
    static bass::VEC3 ToBass(const Vec3& v) { return { v.x, v.z, v.y }; }

    bass::HSTREAM PlaySound(const fs::path& file, const Vec3& pos, int multiplier) {
        bass::HSTREAM s = LoadAudio(file, cfg.enable3D);
        if (!s) return 0;
        float vol = std::max(0.0f, sfxVolume * (float)multiplier) / 100.0f;
        bass::BASS_ChannelSetAttribute(s, bass::ATTRIB_VOL, vol);
        if (cfg.enable3D) {
            bass::VEC3 p = ToBass(pos);
            bass::BASS_ChannelSet3DPosition(s, &p, nullptr, nullptr);
            bass::BASS_Apply3D();
        }
        bass::BASS_ChannelPlay(s, FALSE);
        return s;
    }

    bass::HSTREAM PlayAidenSound() {
        if (lib.aidenFiles <= 0) return 0;
        fs::path f = lib.root / "secret" / "aiden" / (std::to_string(RandInt(1, lib.aidenFiles)) + ".mp3");
        return PlaySound(f, aidenPos, cfg.seSound);
    }
    bass::HSTREAM PlayVehicleSound(const Vec3& pos) {
        if (lib.vehicleGroups <= 0) return 0;
        int g = RandInt(1, lib.vehicleGroups);
        int n = lib.vehicleFiles[g - 1];
        if (n <= 0) return 0;
        fs::path f = lib.root / "vehicle" / std::to_string(g) / (std::to_string(RandInt(1, n)) + ".mp3");
        return PlaySound(f, pos, cfg.dvSound);
    }
    void PlayGunSound(int gun, const Vec3& pos) {
        int n = lib.gunFiles[gun - 1];
        if (n <= 0) return;
        fs::path f = lib.root / "gun" / std::to_string(gun) / (std::to_string(RandInt(1, n)) + ".mp3");
        PlaySound(f, pos, cfg.dgsSound); // fire and forget (BASS_STREAM_AUTOFREE)
    }

    // ---- geometry helpers ----
    // Random point within +-distance on x/y of the position.
    Vec3 PositionAround(const Vec3& p, float distance) {
        return { p.x + RandFloat(-1.0f, 1.0f) * distance, p.y + RandFloat(-1.0f, 1.0f) * distance, p.z };
    }
    static Vec3 RotationToDirection(const Vec3& rot) {
        const float d2r = 3.14159265358979f / 180.0f;
        float rz = rot.z * d2r, rx = rot.x * d2r;
        float c = std::fabs(std::cos(rx));
        return { -std::sin(rz) * c, std::cos(rz) * c, std::sin(rx) };
    }
    static float Distance(const Vec3& a, const Vec3& b) {
        Vec3 d = b - a;
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    void UpdateListener(const Vec3& camPos, const Vec3& camRot) {
        Vec3 front = RotationToDirection(camRot);
        // up vector made perpendicular to front (BASS expects orthogonal front/top)
        float dot = front.z; // front . (0,0,1)
        Vec3 up{ -front.x * dot, -front.y * dot, 1.0f - front.z * dot };
        float len = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
        if (len < 1e-4f) up = { 0, 1, 0 }; else up = { up.x / len, up.y / len, up.z / len };

        bass::VEC3 p = ToBass(camPos), v = ToBass(playerVel), f = ToBass(front), t = ToBass(up);
        bass::BASS_Set3DPosition(&p, &v, &f, &t);
        bass::BASS_Set3DFactors(1.0f, 0.1f, 1.0f);
        bass::BASS_Apply3D();
    }

    // ---- per-frame ----
    void OnTick() {
        if (!initTried) Init();
        if (!ready) return;

        // The hook also runs on the main menu / while loading: do nothing until a player exists.
        int playerIdx = nat::ConvertIntToPlayerIndex(nat::GetPlayerId());
        if (!nat::IsPlayerPlaying(playerIdx)) { burst.active = false; return; }
        uint32_t playerPed = nat::GetPlayerChar(playerIdx);
        if (!nat::DoesCharExist(playerPed)) return;

        playerPos = nat::GetCharCoordinates(playerPed);
        playerVel = nat::GetCharVelocity(playerPed);

        uint32_t cam = nat::GetRootCam();
        Vec3 camPos = nat::GetCamPos(cam);
        Vec3 camRot = nat::GetCamRot(cam);

        Zone zone = Zone::LibertyCity;
        bool zoneKnown = false;
        if (const char* zn = nat::GetNameOfZone(playerPos.x, playerPos.y, playerPos.z)) {
            auto it = kZones.find(zn);
            if (it != kZones.end()) { zone = it->second; zoneKnown = true; }
        }

        // SFX slider (0..10) scaled by the ini's GlobalMultiplier, like the original mod
        int pref = aPrefs ? aPrefs[PREF_SFX_VOLUME_INDEX] : 0;
        sfxVolume = (float)std::max(pref, 0) * (float)cfg.globalMultiplier;

        UpdateListener(camPos, camRot);

        // Pause menu: abort gun bursts and pause running sounds, resume them afterwards
        if (nat::IsPauseMenuActive()) {
            burst.active = false;
            if (!wasPaused) {
                if (IsPlaying(vehicleStream)) { bass::BASS_ChannelPause(vehicleStream); vehiclePausedByUs = true; }
                if (IsPlaying(aidenStream))   { bass::BASS_ChannelPause(aidenStream);   aidenPausedByUs = true; }
                wasPaused = true;
            }
            return;
        }
        if (wasPaused) {
            if (vehiclePausedByUs) { bass::BASS_ChannelPlay(vehicleStream, FALSE); vehiclePausedByUs = false; }
            if (aidenPausedByUs)   { bass::BASS_ChannelPlay(aidenStream, FALSE);   aidenPausedByUs = false; }
            wasPaused = false;
        }

        if (nat::IsInteriorScene()) return;
        if (!nat::HasCutsceneFinished() && !cfg.playInCutscenes) return;

        if (cfg.seEnabled) TickSecrets();
        if (zoneKnown) {
            if (cfg.dvEnabled) TickDistantVehicles(zone);
            if (cfg.dgsEnabled) TickDistantGuns(zone);
        }
    }

    void TickSecrets() {
        // Aiden: only between 01:00 and 02:59 and once the Derrick mission line is complete
        int hour = nat::GetHoursOfDay();
        if (!(hour >= 1 && hour <= 2)) { nat::DeleteChar(aidenPed); return; }
        if (nat::GetFloatStat(STAT_DERRICK_MISSION_PROGRESS) < 100.0f) return;

        float dist = Distance(playerPos, aidenPos);

        if (nat::DoesCharExist(aidenPed)) Command<void, Commands::SET_PED_ALPHA>(aidenPed, 125);
        else aidenPed = 0;

        if (dist <= 450.0f) {
            if (aidenPed == 0 && dist > 80.0f) SpawnAiden();
        } else {
            nat::DeleteChar(aidenPed);
        }
        if (dist <= 80.0f) nat::DeleteChar(aidenPed);

        if (dist <= 85.0f) {
            float r = RandFloat(0.0f, 101.0f) * 90.0f;
            if (r < 5.0f && !IsPlaying(aidenStream)) aidenStream = PlayAidenSound();
        }
    }

    void SpawnAiden() {
        nat::LoadModel(AIDEN_MODEL_HASH);
        uint32_t ped = 0;
        Command<void, Commands::CREATE_CHAR>(22, AIDEN_MODEL_HASH, aidenPos.x, aidenPos.y, aidenPos.z, &ped, 1);
        if (nat::DoesCharExist(ped)) {
            aidenPed = ped;
            Command<void, Commands::SET_CHAR_AS_MISSION_CHAR>(ped);
            Command<void, Commands::BLOCK_CHAR_AMBIENT_ANIMS>(ped, 1);
            Command<void, Commands::BLOCK_CHAR_GESTURE_ANIMS>(ped, 1);
            Command<void, Commands::BLOCK_CHAR_VISEME_ANIMS>(ped, 1);
            Command<void, Commands::BLOCK_COWERING_IN_COVER>(ped, 1);
            Command<void, Commands::BLOCK_CHAR_HEAD_IK>(ped, 1);
            Command<void, Commands::SET_CHAR_MOVEMENT_ANIMS_BLOCKED>(ped, 1);
            Command<void, Commands::SET_CHAR_HEADING>(ped, 273.157f);
            Command<void, Commands::SET_CHAR_PROOFS>(ped, 1, 1, 1, 1, 1);
            Command<void, Commands::SET_CHAR_COLLISION>(ped, 0);
        }
        nat::UnloadModel(AIDEN_MODEL_HASH);
    }

    // Same odds formula as the original: random(0..101) * global <= zone percentage
    bool Roll(int global, int percentage) {
        return RandFloat(0.0f, 101.0f) * (float)global <= (float)percentage;
    }

    void TickDistantVehicles(Zone zone) {
        if (!Roll(cfg.dvGlobal, ChanceFor(cfg.dvChance, zone))) return;
        if (IsPlaying(vehicleStream)) return;
        Vec3 node;
        if (!nat::GetNextPositionOnStreet(PositionAround(playerPos, (float)cfg.dvDistance), node)) return;
        vehicleStream = PlayVehicleSound(node);
    }

    void TickDistantGuns(Zone zone) {
        ULONGLONG now = GetTickCount64();

        if (!burst.active) {
            if (lib.gunGroups <= 0) return;
            if (!Roll(cfg.dgsGlobal, ChanceFor(cfg.dgsChance, zone))) return;
            burst.active = true;
            burst.gun = RandInt(1, lib.gunGroups);
            burst.total = RandInt(3, 6);   // 3-6 shots, like the original
            burst.fired = 0;
            burst.pos = PositionAround(playerPos, (float)cfg.dgsDistance);
            burst.nextShot = now;
        }

        if (now >= burst.nextShot) {
            PlayGunSound(burst.gun, burst.pos);
            burst.fired++;
            burst.nextShot = now + (ULONGLONG)RandInt(290, 449);
            if (burst.fired >= burst.total) burst.active = false;
        }
    }
} vambience;

} // namespace
