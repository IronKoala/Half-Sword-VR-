#pragma once
#include <string>
namespace RC::Unreal { class UObject; }
namespace hsvr
{
    // Writes the game's Blueprint script and the player's live settings to Mods\HalfSwordVR\study
    // (ini dev_study=1). Reads only.
    void study_tick(RC::Unreal::UObject* pawn, const std::wstring& level_name);
}
