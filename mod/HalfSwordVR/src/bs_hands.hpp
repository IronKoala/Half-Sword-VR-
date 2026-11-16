#pragma once
// Hands that hold what they grip, the Blade & Sorcery way (build 3): squeezing a grip on
// something takes hold of it, letting go drops it. These are the game-side actions; which one a
// grip press or release means is decided in controller_input.cpp (gestures).

namespace RC::Unreal
{
    class UObject;
}

namespace hsvr
{
    // Takes hold of whatever the game itself would let a hand grab (another character's body part,
    // a loose prop) within radius_cm of the hand's palm, with the game's own grab and the joint
    // moved onto the exact bone touched. True when something was grabbed.
    bool bs_grab(RC::Unreal::UObject* pawn, int hand, double radius_cm);
    // The hand holds something by a grab (not a weapon in the hand): the game's "Grabbed R/L".
    bool bs_grabbing(RC::Unreal::UObject* pawn, int hand);
    // Lets go of a grab (the game's "Release Grab R/L").
    void bs_release(RC::Unreal::UObject* pawn, int hand);
    // Drops the weapon in the hand: the game's own drop (what its grab key does to a full hand),
    // without the game's "do not drop" switch in the way. True when the game was asked.
    bool bs_drop(RC::Unreal::UObject* pawn, int hand);
    // What the hand grabbed last ("<actor> by <bone>"), for the self-test and the log.
    const char* bs_last_grab();
} // namespace hsvr
