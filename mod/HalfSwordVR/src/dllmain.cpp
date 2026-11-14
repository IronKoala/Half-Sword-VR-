// HalfSwordVR: Phase 0 "hello" mod.
// Proves our code loads inside Half Sword through UE4SS and can find game objects.
// It only reads and logs; it changes nothing in the game.

#include <chrono>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

class HalfSwordVR : public CppUserModBase
{
  public:
    HalfSwordVR() : CppUserModBase()
    {
        ModName = STR("HalfSwordVR");
        ModVersion = STR("0.0.1");
        ModDescription = STR("Native VR for Half Sword (Phase 0 hello build)");
        ModAuthors = STR("Half-Sword-VR contributors");
        Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] DLL loaded\n"));
    }

    ~HalfSwordVR() override = default;

    auto on_unreal_init() -> void override
    {
        m_unreal_ready = true;
        Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] Unreal initialised\n"));

        if (auto* engine = UObjectGlobals::FindFirstOf(STR("GameEngine")))
        {
            Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] Engine object: {}\n"), engine->GetFullName());
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[HalfSwordVR] GameEngine instance not found yet\n"));
        }
    }

    auto on_update() -> void override
    {
        if (!m_unreal_ready || m_found_player)
        {
            return;
        }
        // Look for the player character a couple of times per second until it appears.
        auto now = std::chrono::steady_clock::now();
        if (now - m_last_check < std::chrono::milliseconds(500))
        {
            return;
        }
        m_last_check = now;

        if (auto* player = UObjectGlobals::FindFirstOf(STR("Willie_BP_C")))
        {
            m_found_player = true;
            Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] Found player character: {}\n"), player->GetFullName());
        }
    }

  private:
    bool m_unreal_ready{false};
    bool m_found_player{false};
    std::chrono::steady_clock::time_point m_last_check{};
};

#define HALFSWORDVR_API __declspec(dllexport)
extern "C"
{
    HALFSWORDVR_API RC::CppUserModBase* start_mod()
    {
        return new HalfSwordVR();
    }

    HALFSWORDVR_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
