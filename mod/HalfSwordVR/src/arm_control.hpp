// Motion controls: the controllers move the character's arms.
//
// Half Sword animates an invisible "driver" skeleton (DriverSkeleton, ABP_Willie) and the visible,
// physically simulated body (CharacterMesh0) copies that pose each frame with a CopyPoseFromMesh
// node; the body's muscles then pull the real arm bodies towards it. This file bends the driver
// skeleton's arm bones so that each hand sits where the player's controller is, just before the
// body copies the pose. Strength, stamina, injuries and weapon weight stay the game's own.
// See docs/phase3-motion-controls.md.
#pragma once

namespace RC::Unreal
{
    class UObject;
}

namespace hsvr
{
    // Once per engine tick on the game thread, before the world ticks. `pawn` is the player's
    // character (may be null), `menu_up` true while a menu or pause screen is showing.
    void arm_game_tick(RC::Unreal::UObject* pawn, bool menu_up);

    // Flips motion arms on or off while playing (a key).
    void arm_toggle();

    // A hand closes on a thing to pick it up (hand 1 = right): where its controller's grip is on
    // that thing now, before the game's pick-up moves it (the hold layer holds it there).
    void arm_note_touch(int hand, RC::Unreal::UObject* component);
} // namespace hsvr
