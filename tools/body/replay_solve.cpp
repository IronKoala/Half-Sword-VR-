// Offline check of the body solver (mod/HalfSwordVR/src/body_solver.cpp) against a bug-marker
// replay: feeds the replay's headset, controllers and capsule through the solver frame by frame and
// reports what the legs do (knee bend, knee jumps a tick, planted feet sliding, steps).
//
// Build (Developer Command Prompt / any cl):
//   cl /O2 /EHsc /std:c++17 /I ..\..\mod\HalfSwordVR\src replay_solve.cpp ..\..\mod\HalfSwordVR\src\body_solver.cpp
// Run:
//   replay_solve <replay.csv> <lopoly_refskeleton.csv> [--frames out.csv]
// The reference skeleton CSV is Willie_Lopoly's (HalfSwordDecomp willie-skeleton export; private, not in
// the repo): columns name, cs_x, cs_y, cs_z in his component space (x his left, y forward, z up).
#include "body_solver.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace hsvr;

static std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') cur += c;
    }
    out.push_back(cur);
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("replay_solve <replay.csv> <lopoly.csv> [--frames out.csv]\n"); return 1; }
    std::string frames_out;
    for (int i = 3; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--frames") frames_out = argv[i + 1];

    // Reference skeleton.
    std::map<std::string, Vec3> ref;
    {
        std::ifstream f(argv[2]);
        std::string line;
        std::getline(f, line);
        auto h = split(line);
        int in = -1, ix = -1, iy = -1, iz = -1;
        for (int i = 0; i < (int)h.size(); ++i)
        {
            if (h[i] == "name") in = i;
            if (h[i] == "cs_x") ix = i;
            if (h[i] == "cs_y") iy = i;
            if (h[i] == "cs_z") iz = i;
        }
        while (std::getline(f, line))
        {
            auto c = split(line);
            if ((int)c.size() <= iz) continue;
            // component (x left, y forward, z up) -> body (X forward, Y right, Z up)
            ref[c[in]] = Vec3{std::atof(c[iy].c_str()), -std::atof(c[ix].c_str()), std::atof(c[iz].c_str())};
        }
    }
    body::Solver solver;
    solver.skel.scale = 1.0;
    for (int j = 0; j < body::J_COUNT; ++j)
    {
        auto it = ref.find(body::joint_bone(j));
        if (it == ref.end()) { std::printf("no %s in the skeleton\n", body::joint_bone(j)); return 1; }
        solver.skel.joint[j] = it->second;
    }
    solver.skel.build(9.0, 7.0);
    std::printf("skeleton: thigh %.1f shin %.1f ankle %.1f eyes %.1f\n", solver.skel.thigh, solver.skel.shin, solver.skel.ankle_height, solver.skel.eye_height);

    std::ifstream f(argv[1]);
    std::string line;
    std::vector<std::string> header;
    double mark = 0.0;
    std::map<std::string, int> col;
    FILE* fo = frames_out.empty() ? nullptr : std::fopen(frames_out.c_str(), "w");
    if (fo) std::fprintf(fo, "t,knee_l,knee_r,dknee_l,dknee_r,dkneepos_l,dkneepos_r,pitch,hips_z,floor,short,pulled,step_l,step_r,lift_l,lift_r,walk,torso_yaw,head_yaw\n");
    double worst_dknee = 0, worst_kneepos = 0, worst_knee = 0, slide_sum = 0, slide_worst = 0;
    int slide_n = 0, frames = 0;
    double prev_knee[2] = {0, 0};
    Vec3 prev_kneepos[2], prev_ankle[2], prev2_kneepos[2];
    Vec3 old_prev[2], old_prev2[2];
    int nprev = 0;
    double acc_worst = 0, acc_sum = 0, old_acc_worst = 0, old_acc_sum = 0;
    int acc_n = 0, twitch = 0, old_twitch = 0;
    double prev_dk[2] = {0, 0}, old_prev_dk[2] = {0, 0}, old_prev_knee[2] = {0, 0};
    bool prev_step[2] = {false, false};
    bool have_prev = false;
    double t0 = -1, t_last = 0, skip_until = -1e9;
    int frames_skipped = 0;
    while (std::getline(f, line))
    {
        if (line.rfind("# mark_t:", 0) == 0) { mark = std::atof(line.c_str() + 9); continue; }
        if (line.empty() || line[0] == '#') continue;
        auto c = split(line);
        if (header.empty())
        {
            header = c;
            for (int i = 0; i < (int)c.size(); ++i) col[c[i]] = i;
            continue;
        }
        auto num = [&](const char* name) -> double {
            auto it = col.find(name);
            if (it == col.end() || it->second >= (int)c.size() || c[it->second].empty()) return std::nan("");
            return std::atof(c[it->second].c_str());
        };
        double t = num("t");
        if (std::isnan(num("head_x"))) continue;
        body::Input in;
        in.dt = t0 < 0 ? 1.0 / 60.0 : t - t_last;
        t_last = t;
        if (t0 < 0) t0 = t;
        in.eyes = {num("head_x"), num("head_y"), num("head_z")};
        in.head_rot = Quat{num("head_qx"), num("head_qy"), num("head_qz"), num("head_qw")}.normalized();
        in.eye_offset = {9.0, 0.0, 7.0};
        const char* side[2] = {"l", "r"};
        for (int s = 0; s < 2; ++s)
        {
            std::string p = side[s];
            double x = num((p + "_ctl_x").c_str());
            in.hand_valid[s] = !std::isnan(x) && num((p + "_valid").c_str()) != 0.0;
            in.hand[s] = {x, num((p + "_ctl_y").c_str()), num((p + "_ctl_z").c_str())};
        }
        in.capsule = {num("actor_x"), num("actor_y"), num("actor_z")};
        // The floor: where the recorded (traced) feet stood, the lower foot, less his ankle height, held
        // at the lowest of the last 0.3 s (a lifted foot is not the floor). The capsule's own height jumps
        // with the game's step-ups and stairs.
        double floor;
        {
            double fz = (std::min)(num("bone_foot_l_z"), num("bone_foot_r_z")) - solver.skel.ankle_height;
            if (std::isnan(fz)) fz = in.capsule.z - 87.0;
            static std::vector<std::pair<double, double>> hist;
            hist.push_back({t, fz});
            while (!hist.empty() && hist.front().first < t - 0.3) hist.erase(hist.begin());
            floor = fz;
            for (auto& h : hist) floor = (std::min)(floor, h.second);
        }
        in.floor_z[0] = in.floor_z[1] = floor;
        body::Output o = solver.solve(in);
        if (!o.valid) continue;
        // The view itself jumped (the capsule stepping onto a body or a stair and back): the body follows
        // the eyes, so those frames say nothing about the solver; counted apart.
        static Vec3 last_eyes;
        static bool have_eyes = false;
        bool view_jump = have_eyes && length(in.eyes - last_eyes) > 8.0 + 600.0 * in.dt;
        last_eyes = in.eyes, have_eyes = true;
        static int view_jumps = 0;
        if (view_jump) { ++view_jumps; skip_until = t + 0.1; }
        if (t < skip_until) { have_prev = false; nprev = 0; frames_skipped++; }
        ++frames;
        Vec3 kp[2] = {o.pos[body::J_CALF_L], o.pos[body::J_CALF_R]};
        Vec3 an[2] = {o.pos[body::J_FOOT_L], o.pos[body::J_FOOT_R]};
        double dk[2] = {0, 0}, dkp[2] = {0, 0};
        for (int s = 0; s < 2; ++s)
        {
            worst_knee = (std::max)(worst_knee, o.knee_deg[s]);
            if (have_prev)
            {
                dk[s] = o.knee_deg[s] - prev_knee[s];
                dkp[s] = length(kp[s] - prev_kneepos[s]);
                worst_dknee = (std::max)(worst_dknee, std::fabs(dk[s]));
                worst_kneepos = (std::max)(worst_kneepos, dkp[s]);
                if (!o.stepping[s] && !prev_step[s] && in.dt > 0)
                {
                    Vec3 d = an[s] - prev_ankle[s];
                    d.z = 0;
                    double v = length(d) / in.dt;
                    slide_sum += v, ++slide_n;
                    slide_worst = (std::max)(slide_worst, v);
                }
            }
            prev_knee[s] = o.knee_deg[s];
            prev2_kneepos[s] = prev_kneepos[s];
            prev_kneepos[s] = kp[s];
            prev_ankle[s] = an[s];
            prev_step[s] = o.stepping[s];
        }
        // Jerkiness: the knee's second difference a frame (cm), ours and the recorded (old) pose's, and
        // twitches (the knee's bend reversing by more than 10 deg a tick both ways).
        {
            const char* cb[2] = {"bone_calf_l", "bone_calf_r"};
            const char* tb[2] = {"bone_thigh_l", "bone_thigh_r"};
            const char* fb[2] = {"bone_foot_l", "bone_foot_r"};
            for (int s2 = 0; s2 < 2; ++s2)
            {
                std::string c0 = cb[s2], t0s = tb[s2], f0 = fb[s2];
                Vec3 ok{num((c0 + "_x").c_str()), num((c0 + "_y").c_str()), num((c0 + "_z").c_str())};
                Vec3 ot{num((t0s + "_x").c_str()), num((t0s + "_y").c_str()), num((t0s + "_z").c_str())};
                Vec3 of{num((f0 + "_x").c_str()), num((f0 + "_y").c_str()), num((f0 + "_z").c_str())};
                Vec3 a = normalized(ot - ok), b = normalized(of - ok);
                double old_knee = 180.0 - std::acos((std::max)(-1.0, (std::min)(1.0, dot(a, b)))) * 57.29578;
                if (nprev >= 2)
                {
                    double acc = length(kp[s2] - prev_kneepos[s2] * 2.0 + prev2_kneepos[s2]);
                    double oacc = length(ok - old_prev[s2] * 2.0 + old_prev2[s2]);
                    acc_worst = (std::max)(acc_worst, acc), acc_sum += acc;
                    old_acc_worst = (std::max)(old_acc_worst, oacc), old_acc_sum += oacc;
                    ++acc_n;
                    double odk = old_knee - old_prev_knee[s2];
                    if (dk[s2] * prev_dk[s2] < 0 && std::fabs(dk[s2]) > 10 && std::fabs(prev_dk[s2]) > 10) ++twitch;
                    if (odk * old_prev_dk[s2] < 0 && std::fabs(odk) > 10 && std::fabs(old_prev_dk[s2]) > 10) ++old_twitch;
                    old_prev_dk[s2] = odk;
                }
                else if (nprev == 1) old_prev_dk[s2] = old_knee - old_prev_knee[s2];
                prev_dk[s2] = dk[s2];
                old_prev_knee[s2] = old_knee;
                old_prev2[s2] = old_prev[s2];
                old_prev[s2] = ok;
            }
        }
        ++nprev;
        have_prev = true;
        if (fo)
        {
            Vec3 fw = in.head_rot.rotate({1, 0, 0});
            std::fprintf(fo, "%.3f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%.1f,%.1f,%d,%.0f,%.0f\n", t - mark, o.knee_deg[0], o.knee_deg[1], dk[0], dk[1], dkp[0], dkp[1],
                         o.pitch_deg, o.pos[body::J_PELVIS].z - floor, floor, o.hips_short_cm, o.hips_pulled_cm, (int)o.stepping[0], (int)o.stepping[1], o.lift_cm[0], o.lift_cm[1],
                         (int)o.walking, o.torso_yaw_deg, std::atan2(fw.y, fw.x) * 57.2958);
        }
    }
    if (fo) std::fclose(fo);
    body::Output last;
    std::printf("frames %d: knee worst %.0f deg; dk %.1f deg; dpos %.1f cm; slide mean %.2f cm/s worst %.0f; jerk mean %.2f worst %.1f cm (old %.2f / %.1f); twitches %d (old %d)\n",
                frames, worst_knee, worst_dknee, worst_kneepos, slide_n ? slide_sum / slide_n : 0.0, slide_worst,
                acc_n ? acc_sum / acc_n : 0.0, acc_worst, acc_n ? old_acc_sum / acc_n : 0.0, old_acc_worst, twitch, old_twitch, frames_skipped);
    return 0;
}
