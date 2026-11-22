#include "controllers.hpp"

#include "config.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
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
        XrAction g_trigger_touch = XR_NULL_HANDLE;
        XrAction g_thumb_touch = XR_NULL_HANDLE;

        // SteamVR's hand skeleton (XR_EXT_hand_tracking): its estimate of each finger from the
        // controller's sensors (or real finger tracking where the controller has it).
        bool g_ext_hand_tracking = false;
        bool g_ext_motion_range = false;
        bool g_system_hand_tracking = false;
        PFN_xrCreateHandTrackerEXT g_create_hand_tracker = nullptr;
        PFN_xrDestroyHandTrackerEXT g_destroy_hand_tracker = nullptr;
        PFN_xrLocateHandJointsEXT g_locate_hand_joints = nullptr;
        XrHandTrackerEXT g_hand_tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        XrSpace g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        XrSession g_attached_session = XR_NULL_HANDLE;
        bool g_created = false;

        std::mutex g_state_mutex;
        ControllerState g_state;
        bool g_trigger_was_down[2] = {false, false};
        std::atomic<float> g_buzz[2] = {0.0f, 0.0f};
        // Hit envelopes playing on each hand (controllers_hit).
        struct HitEnvelope
        {
            std::vector<float> t, v;
            double start = 0.0, seconds = 0.1;
            float volume = 0.0f;
        };
        std::mutex g_hit_mutex;
        std::vector<HitEnvelope> g_hits[2];
        // A signature pattern (controllers_signature): while it plays, nothing else buzzes that hand.
        HitEnvelope g_sig[2];
        bool g_sig_on[2] = {false, false};

        float envelope_at(const HitEnvelope& e, double u)
        {
            if (e.t.empty()) return 0.0f;
            if (u <= e.t.front()) return e.v.front();
            for (size_t i = 1; i < e.t.size(); ++i)
                if (u <= e.t[i])
                {
                    double span = e.t[i] - e.t[i - 1];
                    double f = span > 1e-6 ? (u - e.t[i - 1]) / span : 1.0;
                    return (float)(e.v[i - 1] + (e.v[i] - e.v[i - 1]) * f);
                }
            return e.v.back();
        }

        // The sum of the hand's envelopes now (B&S Hand.UpdateHaptic), dropping finished ones.
        float hit_amplitude(int h)
        {
            double now = now_seconds();
            std::lock_guard<std::mutex> lock(g_hit_mutex);
            auto& list = g_hits[h];
            float sum = 0.0f;
            for (size_t i = 0; i < list.size();)
            {
                double u = (now - list[i].start) / list[i].seconds;
                if (u >= 1.0)
                {
                    list.erase(list.begin() + i);
                    continue;
                }
                sum += envelope_at(list[i], (std::max)(0.0, u)) * list[i].volume;
                ++i;
            }
            return (std::min)(1.0f, sum);
        }

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

        float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

        // One finger's bend: the angles between its consecutive bones, summed (degrees).
        float finger_bend(const XrHandJointLocationEXT* j, const int* chain, int n)
        {
            float total = 0.0f;
            for (int k = 0; k + 2 < n; ++k)
            {
                const XrVector3f& a = j[chain[k]].pose.position;
                const XrVector3f& b = j[chain[k + 1]].pose.position;
                const XrVector3f& c = j[chain[k + 2]].pose.position;
                float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                float vx = c.x - b.x, vy = c.y - b.y, vz = c.z - b.z;
                float lu = std::sqrt(ux * ux + uy * uy + uz * uz), lv = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (lu < 1e-5f || lv < 1e-5f) continue;
                float d = (ux * vx + uy * vy + uz * vz) / (lu * lv);
                d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
                total += std::acos(d) * 57.29578f;
            }
            return total;
        }

        // The skeleton's finger curls. Full bend (a fist): about 250 deg over a finger's three joints,
        // about 110 over the thumb's two (normal human ranges; logged in degrees to calibrate).
        bool read_joints(XrSpace space, XrTime time, int h, HandState& hand)
        {
            if (g_hand_tracker[h] == XR_NULL_HANDLE || !g_locate_hand_joints) return false;
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locations.jointLocations = joints;
            XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            info.baseSpace = space;
            info.time = time;
            XrHandJointsMotionRangeInfoEXT range{XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT};
            range.handJointsMotionRange = XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT; // a closed hand reads as a fist, not wrapped on the controller
            if (g_ext_motion_range) info.next = &range;
            if (XR_FAILED(g_locate_hand_joints(g_hand_tracker[h], &info, &locations)) || !locations.isActive) return false;
            for (int k = 0; k < XR_HAND_JOINT_COUNT_EXT; ++k)
                if (!(joints[k].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
            static const int thumb[] = {XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_THUMB_PROXIMAL_EXT, XR_HAND_JOINT_THUMB_DISTAL_EXT, XR_HAND_JOINT_THUMB_TIP_EXT};
            static const int index[] = {XR_HAND_JOINT_INDEX_METACARPAL_EXT, XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
                                        XR_HAND_JOINT_INDEX_DISTAL_EXT, XR_HAND_JOINT_INDEX_TIP_EXT};
            static const int middle[] = {XR_HAND_JOINT_MIDDLE_METACARPAL_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT,
                                         XR_HAND_JOINT_MIDDLE_DISTAL_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT};
            static const int ring[] = {XR_HAND_JOINT_RING_METACARPAL_EXT, XR_HAND_JOINT_RING_PROXIMAL_EXT, XR_HAND_JOINT_RING_INTERMEDIATE_EXT,
                                       XR_HAND_JOINT_RING_DISTAL_EXT, XR_HAND_JOINT_RING_TIP_EXT};
            static const int little[] = {XR_HAND_JOINT_LITTLE_METACARPAL_EXT, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,
                                         XR_HAND_JOINT_LITTLE_DISTAL_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT};
            hand.joint_deg[0] = finger_bend(joints, thumb, 4);
            hand.joint_deg[1] = finger_bend(joints, index, 5);
            hand.joint_deg[2] = finger_bend(joints, middle, 5);
            hand.joint_deg[3] = finger_bend(joints, ring, 5);
            hand.joint_deg[4] = finger_bend(joints, little, 5);
            // Each joint on its own (Matthew 2026-11-21: "fingers have joints"): the bend at the knuckle
            // (metacarpal -> proximal -> intermediate), the middle joint and the end joint, over the
            // rig's full bend there (the framework's CURL table, degrees).
            static const float full_deg[5][3] = {{25.8f, 34.4f, 45.8f}, {69.9f, 95.1f, 60.2f}, {77.9f, 100.3f, 64.7f}, {81.9f, 100.3f, 61.9f}, {88.2f, 95.1f, 60.2f}};
            const int* chains[5] = {thumb, index, middle, ring, little};
            for (int f = 1; f < 5; ++f)
                for (int k = 0; k < 3; ++k)
                    hand.joint_frac[f][k] = clamp01(finger_bend(joints, chains[f] + k, 3) / full_deg[f][k]);
            hand.joint_curl[0] = clamp01(hand.joint_deg[0] / 110.0f);
            for (int f = 1; f < 5; ++f) hand.joint_curl[f] = clamp01(hand.joint_deg[f] / 250.0f);
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
        // Touch sensors (finger curls for fists); not fatal if a runtime refuses them.
        make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "Finger on trigger", g_trigger_touch);
        make_action(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_touch", "Thumb resting", g_thumb_touch);

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
                 {&g_trigger_touch, "/input/trigger/touch", "/input/trigger/touch"},
                 {&g_thumb_touch, "/input/thumbstick/touch", "/input/thumbstick/touch"},
                 {&g_thumb_touch, "/input/a/touch", "/input/a/touch"},
                 {&g_thumb_touch, "/input/b/touch", "/input/b/touch"},
                 {&g_thumb_touch, "/input/trackpad/touch", "/input/trackpad/touch"},
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
                 {&g_trigger_touch, "/input/trigger/touch", "/input/trigger/touch"},
                 {&g_thumb_touch, "/input/thumbstick/touch", "/input/thumbstick/touch"},
                 {&g_thumb_touch, "/input/x/touch", "/input/a/touch"},
                 {&g_thumb_touch, "/input/y/touch", "/input/b/touch"},
                 {&g_thumb_touch, "/input/thumbrest/touch", "/input/thumbrest/touch"},
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
        if (g_ext_hand_tracking && g_system_hand_tracking && config().input_hand_tracking && xr.GetInstanceProcAddr)
        {
            xr.GetInstanceProcAddr(g_instance, "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&g_create_hand_tracker));
            xr.GetInstanceProcAddr(g_instance, "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&g_destroy_hand_tracker));
            xr.GetInstanceProcAddr(g_instance, "xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction*>(&g_locate_hand_joints));
            for (int h = 0; h < 2 && g_create_hand_tracker && g_locate_hand_joints; ++h)
            {
                XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                info.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                XrResult hr = g_create_hand_tracker(session, &info, &g_hand_tracker[h]);
                HSVR_LOG("Controllers: %s hand skeleton %s", kHandName[h], XR_SUCCEEDED(hr) ? "tracked (XR_EXT_hand_tracking)" : result_name(xr, g_instance, hr));
                if (XR_FAILED(hr)) g_hand_tracker[h] = XR_NULL_HANDLE;
            }
        }
        else
            HSVR_LOG("Controllers: no hand skeleton (extension %d, system %d, input_hand_tracking %d): finger curls from the trigger, grip and touch sensors",
                     (int)g_ext_hand_tracking, (int)g_system_hand_tracking, (int)config().input_hand_tracking);
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
            hand.trigger_touch = read_bool(xr, session, g_trigger_touch, h);
            hand.thumb_touch = read_bool(xr, session, g_thumb_touch, h);
            // Sensor curls (as Matthew's framework, ik/fingers.rs): the thumb resting on a control is
            // closed, pressing one is fully closed; the index follows the trigger (a finger resting on
            // it is a little curled); the other three follow the grip.
            bool thumb_press = hand.stick_click || hand.primary || hand.secondary;
            hand.sensor_curl[0] = thumb_press ? 1.0f : (hand.thumb_touch ? 0.8f : 0.1f);
            hand.sensor_curl[1] = (std::max)(hand.trigger, hand.trigger_touch ? 0.25f : 0.0f);
            hand.sensor_curl[2] = hand.sensor_curl[3] = hand.sensor_curl[4] = hand.squeeze;
            hand.joints_valid = read_joints(space, (XrTime)time, h, hand);
            for (int f = 0; f < 5; ++f) hand.curl[f] = hand.joints_valid ? (std::max)(hand.joint_curl[f], hand.sensor_curl[f]) : hand.sensor_curl[f];

            // Vibration check: a short buzz each time the trigger is pulled all the way.
            bool down = hand.trigger > 0.9f;
            if (down && !g_trigger_was_down[h] && config().haptics_test)
            {
                pulse(xr, session, h, 0.5f, 0.05);
                HSVR_LOG("Controllers: test buzz on the %s hand", kHandName[h]);
            }
            g_trigger_was_down[h] = down;
            float buzz = g_buzz[h].exchange(0.0f);
            float hit = hit_amplitude(h);
            {
                std::lock_guard<std::mutex> lock(g_hit_mutex);
                if (g_sig_on[h])
                {
                    double u = (now_seconds() - g_sig[h].start) / g_sig[h].seconds;
                    if (u >= 1.0) g_sig_on[h] = false;
                    else
                    {
                        // Only the pattern: other buzzes and hits are dropped, so its gaps stay silent.
                        float v = envelope_at(g_sig[h], (std::max)(0.0, u)) * g_sig[h].volume;
                        if (v > 0.01f) pulse(xr, session, h, v, 1.0 / 60.0);
                        continue;
                    }
                }
            }
            // A hit plays as B&S sends it: the summed envelope, 1/30 s at a time, every frame.
            if (hit > 0.01f && hit >= buzz) pulse(xr, session, h, hit, 1.0 / 30.0);
            else if (buzz > 0.01f) pulse(xr, session, h, (std::min)(1.0f, buzz), 0.02);
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

    std::vector<const char*> controllers_wanted_extensions(const std::vector<std::string>& offered)
    {
        std::vector<const char*> want;
        g_ext_hand_tracking = g_ext_motion_range = false;
        if (!config().input_hand_tracking) return want;
        for (const std::string& e : offered)
        {
            if (e == XR_EXT_HAND_TRACKING_EXTENSION_NAME) g_ext_hand_tracking = true;
            if (e == XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME) g_ext_motion_range = true;
        }
        if (g_ext_hand_tracking) want.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        if (g_ext_hand_tracking && g_ext_motion_range) want.push_back(XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME);
        else g_ext_motion_range = false;
        HSVR_LOG("Controllers: runtime offers hand skeleton %d, motion range %d", (int)g_ext_hand_tracking, (int)g_ext_motion_range);
        return want;
    }

    void controllers_system_hand_tracking(bool supported)
    {
        g_system_hand_tracking = supported;
        HSVR_LOG("Controllers: headset system %s hand tracking", supported ? "supports" : "does not support");
    }

    ControllerState controllers_state()
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        return g_state;
    }

    void controllers_hit(int hand, const float* key_t, const float* key_v, int keys, double seconds, float volume)
    {
        if (hand < 0 || hand > 1 || keys <= 0 || seconds <= 0.0 || volume <= 0.0f) return;
        HitEnvelope e;
        e.t.assign(key_t, key_t + keys);
        e.v.assign(key_v, key_v + keys);
        e.start = now_seconds();
        e.seconds = seconds;
        e.volume = volume;
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        if (g_hits[hand].size() >= 16) g_hits[hand].erase(g_hits[hand].begin());
        g_hits[hand].push_back(std::move(e));
    }

    void controllers_signature(int hand, const float* key_t, const float* key_v, int keys, double seconds, float volume)
    {
        if (hand < 0 || hand > 1 || keys <= 0 || seconds <= 0.0) return;
        std::lock_guard<std::mutex> lock(g_hit_mutex);
        g_sig[hand].t.assign(key_t, key_t + keys);
        g_sig[hand].v.assign(key_v, key_v + keys);
        g_sig[hand].start = now_seconds();
        g_sig[hand].seconds = seconds;
        g_sig[hand].volume = volume;
        g_sig_on[hand] = true;
    }

    void controllers_buzz(int hand, float amplitude)
    {
        if (hand < 0 || hand > 1) return;
        float old = g_buzz[hand].load();
        while (amplitude > old && !g_buzz[hand].compare_exchange_weak(old, amplitude)) {}
    }
} // namespace hsvr
