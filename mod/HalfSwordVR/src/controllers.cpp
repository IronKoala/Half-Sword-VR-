#include "controllers.hpp"

#include "config.hpp"
#include "log.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace hsvr
{
    namespace
    {
        XrActionSet g_set = XR_NULL_HANDLE;
        XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
        XrAction g_grip_pose = XR_NULL_HANDLE;
        XrAction g_aim_pose = XR_NULL_HANDLE;
        XrAction g_trigger = XR_NULL_HANDLE;
        XrAction g_squeeze = XR_NULL_HANDLE;
        XrAction g_stick = XR_NULL_HANDLE;
        XrAction g_stick_click = XR_NULL_HANDLE;
        XrAction g_primary = XR_NULL_HANDLE;
        XrAction g_secondary = XR_NULL_HANDLE;
        XrAction g_menu = XR_NULL_HANDLE;
        XrAction g_haptic = XR_NULL_HANDLE;
        XrSpace g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        XrSession g_attached_session = XR_NULL_HANDLE;
        bool g_created = false;

        std::mutex g_state_mutex;
        ControllerState g_state;
        bool g_trigger_was_down[2] = {false, false};

        const char* kHandName[2] = {"left", "right"};

        const char* result_name(const XrApi& xr, XrInstance instance, XrResult r)
        {
            static thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
            if (xr.ResultToString && instance != XR_NULL_HANDLE && XR_SUCCEEDED(xr.ResultToString(instance, r, buffer))) return buffer;
            snprintf(buffer, sizeof(buffer), "XrResult %d", (int)r);
            return buffer;
        }

        XrInstance g_instance = XR_NULL_HANDLE;

        bool make_action(const XrApi& xr, XrActionType type, const char* name, const char* label, XrAction& out)
        {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = type;
            strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
            strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            info.countSubactionPaths = 2;
            info.subactionPaths = g_hand_path;
            XrResult r = xr.CreateAction(g_set, &info, &out);
            if (XR_FAILED(r))
            {
                HSVR_LOG("Controllers: could not make action %s: %s", name, result_name(xr, g_instance, r));
                return false;
            }
            return true;
        }

        // One controller layout: the input paths that exist on it, per action. A path missing on a
        // layout is left out, because one wrong path makes the runtime refuse the whole layout.
        struct Binding
        {
            XrAction* action;
            const char* left;  // path below /user/hand/left, or nullptr
            const char* right; // path below /user/hand/right, or nullptr
        };

        void suggest(const XrApi& xr, const char* profile, const std::vector<Binding>& bindings)
        {
            std::vector<XrActionSuggestedBinding> list;
            for (const Binding& b : bindings)
            {
                const char* sub[2] = {b.left, b.right};
                for (int h = 0; h < 2; ++h)
                {
                    if (!sub[h]) continue;
                    std::string full = std::string("/user/hand/") + kHandName[h] + sub[h];
                    XrPath path = XR_NULL_PATH;
                    if (XR_FAILED(xr.StringToPath(g_instance, full.c_str(), &path))) continue;
                    list.push_back({*b.action, path});
                }
            }
            XrPath profile_path = XR_NULL_PATH;
            xr.StringToPath(g_instance, profile, &profile_path);
            XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = profile_path;
            suggested.countSuggestedBindings = (uint32_t)list.size();
            suggested.suggestedBindings = list.data();
            XrResult r = xr.SuggestInteractionProfileBindings(g_instance, &suggested);
            HSVR_LOG("Controllers: bindings for %s %s (%d inputs)", profile, XR_SUCCEEDED(r) ? "accepted" : result_name(xr, g_instance, r), (int)list.size());
        }

        bool read_bool(const XrApi& xr, XrSession session, XrAction action, int hand)
        {
            if (action == XR_NULL_HANDLE) return false;
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
            info.action = action;
            info.subactionPath = g_hand_path[hand];
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_FAILED(xr.GetActionStateBoolean(session, &info, &state)) || !state.isActive) return false;
            return state.currentState != XR_FALSE;
        }

        float read_float(const XrApi& xr, XrSession session, XrAction action, int hand)
        {
            if (action == XR_NULL_HANDLE) return 0.0f;
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
            info.action = action;
            info.subactionPath = g_hand_path[hand];
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_FAILED(xr.GetActionStateFloat(session, &info, &state)) || !state.isActive) return 0.0f;
            return state.currentState;
        }

        void read_vec2(const XrApi& xr, XrSession session, XrAction action, int hand, float& x, float& y)
        {
            x = y = 0.0f;
            if (action == XR_NULL_HANDLE) return;
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
            info.action = action;
            info.subactionPath = g_hand_path[hand];
            XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_FAILED(xr.GetActionStateVector2f(session, &info, &state)) || !state.isActive) return;
            x = state.currentState.x;
            y = state.currentState.y;
        }

        bool locate(const XrApi& xr, XrSpace hand_space, XrSpace base, XrTime time, XrPoseRaw& out)
        {
            if (hand_space == XR_NULL_HANDLE) return false;
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xr.LocateSpace(hand_space, base, time, &location))) return false;
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((location.locationFlags & needed) != needed) return false;
            out.px = location.pose.position.x;
            out.py = location.pose.position.y;
            out.pz = location.pose.position.z;
            out.qx = location.pose.orientation.x;
            out.qy = location.pose.orientation.y;
            out.qz = location.pose.orientation.z;
            out.qw = location.pose.orientation.w;
            return true;
        }

        void pulse(const XrApi& xr, XrSession session, int hand, float amplitude, double seconds)
        {
            if (g_haptic == XR_NULL_HANDLE) return;
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = g_haptic;
            info.subactionPath = g_hand_path[hand];
            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.amplitude = amplitude;
            vibration.duration = (XrDuration)(seconds * 1e9);
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            xr.ApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
    } // namespace

    bool controllers_create(const XrApi& xr, XrInstance instance)
    {
        g_instance = instance;
        xr.StringToPath(instance, "/user/hand/left", &g_hand_path[0]);
        xr.StringToPath(instance, "/user/hand/right", &g_hand_path[1]);

        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        XrResult r = xr.CreateActionSet(instance, &set_info, &g_set);
        if (XR_FAILED(r))
        {
            HSVR_LOG("Controllers: could not make the action set: %s", result_name(xr, instance, r));
            return false;
        }
        bool ok = make_action(xr, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand grip", g_grip_pose) &&
                  make_action(xr, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Hand pointer", g_aim_pose) &&
                  make_action(xr, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", g_trigger) &&
                  make_action(xr, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip button", g_squeeze) &&
                  make_action(xr, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick", g_stick) &&
                  make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", g_stick_click) &&
                  make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "Button A / X", g_primary) &&
                  make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Button B / Y", g_secondary) &&
                  make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", g_menu) &&
                  make_action(xr, XR_ACTION_TYPE_VIBRATION_OUTPUT, "vibrate", "Vibration", g_haptic);
        if (!ok) return false;

        const char* grip = "/input/grip/pose";
        const char* aim = "/input/aim/pose";
        const char* haptic = "/output/haptic";
        // Valve Index controllers.
        suggest(xr, "/interaction_profiles/valve/index_controller",
                {{&g_grip_pose, grip, grip},
                 {&g_aim_pose, aim, aim},
                 {&g_trigger, "/input/trigger/value", "/input/trigger/value"},
                 {&g_squeeze, "/input/squeeze/value", "/input/squeeze/value"},
                 {&g_stick, "/input/thumbstick", "/input/thumbstick"},
                 {&g_stick_click, "/input/thumbstick/click", "/input/thumbstick/click"},
                 {&g_primary, "/input/a/click", "/input/a/click"},
                 {&g_secondary, "/input/b/click", "/input/b/click"},
                 {&g_haptic, haptic, haptic}});
        // Oculus / Meta Touch layout (also what many other controllers are offered as).
        suggest(xr, "/interaction_profiles/oculus/touch_controller",
                {{&g_grip_pose, grip, grip},
                 {&g_aim_pose, aim, aim},
                 {&g_trigger, "/input/trigger/value", "/input/trigger/value"},
                 {&g_squeeze, "/input/squeeze/value", "/input/squeeze/value"},
                 {&g_stick, "/input/thumbstick", "/input/thumbstick"},
                 {&g_stick_click, "/input/thumbstick/click", "/input/thumbstick/click"},
                 {&g_primary, "/input/x/click", "/input/a/click"},
                 {&g_secondary, "/input/y/click", "/input/b/click"},
                 {&g_menu, "/input/menu/click", nullptr},
                 {&g_haptic, haptic, haptic}});
        // HTC Vive wands.
        suggest(xr, "/interaction_profiles/htc/vive_controller",
                {{&g_grip_pose, grip, grip},
                 {&g_aim_pose, aim, aim},
                 {&g_trigger, "/input/trigger/value", "/input/trigger/value"},
                 {&g_squeeze, "/input/squeeze/click", "/input/squeeze/click"},
                 {&g_stick, "/input/trackpad", "/input/trackpad"},
                 {&g_stick_click, "/input/trackpad/click", "/input/trackpad/click"},
                 {&g_menu, "/input/menu/click", "/input/menu/click"},
                 {&g_haptic, haptic, haptic}});
        // The minimal layout every runtime supports.
        suggest(xr, "/interaction_profiles/khr/simple_controller",
                {{&g_grip_pose, grip, grip},
                 {&g_aim_pose, aim, aim},
                 {&g_trigger, "/input/select/click", "/input/select/click"},
                 {&g_menu, "/input/menu/click", "/input/menu/click"},
                 {&g_haptic, haptic, haptic}});
        g_created = true;
        return true;
    }

    bool controllers_attach(const XrApi& xr, XrSession session)
    {
        if (!g_created || session == XR_NULL_HANDLE || g_attached_session == session) return g_attached_session == session;
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &g_set;
        XrResult r = xr.AttachSessionActionSets(session, &attach);
        if (XR_FAILED(r))
        {
            HSVR_LOG("Controllers: could not attach the action set: %s", result_name(xr, g_instance, r));
            return false;
        }
        for (int h = 0; h < 2; ++h)
        {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.subactionPath = g_hand_path[h];
            info.poseInActionSpace.orientation.w = 1.0f;
            info.action = g_grip_pose;
            xr.CreateActionSpace(session, &info, &g_grip_space[h]);
            info.action = g_aim_pose;
            xr.CreateActionSpace(session, &info, &g_aim_space[h]);
        }
        g_attached_session = session;
        HSVR_LOG("Controllers: action set attached");
        return true;
    }

    void controllers_update(const XrApi& xr, XrSession session, XrSpace space, int64_t time, const XrPoseRaw* head)
    {
        if (session == XR_NULL_HANDLE || session != g_attached_session) return;
        XrActiveActionSet active{g_set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        XrResult r = xr.SyncActions(session, &sync);
        if (XR_FAILED(r))
        {
            HSVR_LOG_EVERY(10.0, "Controllers: xrSyncActions failed: %s", result_name(xr, g_instance, r));
            return;
        }
        ControllerState s;
        s.active = true;
        s.focused = r != XR_SESSION_NOT_FOCUSED;
        s.predicted_time = time;
        if (head)
        {
            s.head_valid = true;
            s.head = *head;
        }
        for (int h = 0; h < 2; ++h)
        {
            HandState& hand = s.hand[h];
            hand.grip_valid = locate(xr, g_grip_space[h], space, (XrTime)time, hand.grip);
            hand.aim_valid = locate(xr, g_aim_space[h], space, (XrTime)time, hand.aim);
            hand.trigger = read_float(xr, session, g_trigger, h);
            hand.squeeze = read_float(xr, session, g_squeeze, h);
            read_vec2(xr, session, g_stick, h, hand.stick_x, hand.stick_y);
            hand.stick_click = read_bool(xr, session, g_stick_click, h);
            hand.primary = read_bool(xr, session, g_primary, h);
            hand.secondary = read_bool(xr, session, g_secondary, h);
            hand.menu = read_bool(xr, session, g_menu, h);

            // Vibration check: a short buzz each time the trigger is pulled all the way.
            bool down = hand.trigger > 0.9f;
            if (down && !g_trigger_was_down[h] && config().haptics_test)
            {
                pulse(xr, session, h, 0.5f, 0.05);
                HSVR_LOG("Controllers: test buzz on the %s hand", kHandName[h]);
            }
            g_trigger_was_down[h] = down;
        }
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_state = s;
    }

    void controllers_profile_changed(const XrApi& xr, XrInstance instance, XrSession session)
    {
        if (session == XR_NULL_HANDLE || session != g_attached_session) return;
        for (int h = 0; h < 2; ++h)
        {
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            XrResult r = xr.GetCurrentInteractionProfile(session, g_hand_path[h], &state);
            if (XR_FAILED(r))
            {
                HSVR_LOG("Controllers: %s hand layout unknown: %s", kHandName[h], result_name(xr, instance, r));
                continue;
            }
            if (state.interactionProfile == XR_NULL_PATH)
            {
                HSVR_LOG("Controllers: %s hand has no controller (or none of our layouts fit it)", kHandName[h]);
                continue;
            }
            char text[XR_MAX_PATH_LENGTH]{};
            uint32_t length = 0;
            xr.PathToString(instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &length, text);
            HSVR_LOG("Controllers: %s hand uses layout %s", kHandName[h], text);
        }
    }

    ControllerState controllers_state()
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        return g_state;
    }
} // namespace hsvr
