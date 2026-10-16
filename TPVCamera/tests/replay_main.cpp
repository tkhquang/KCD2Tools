/**
 * @file replay_main.cpp
 * @brief Replays a camera frame recording through the core (core/collision/probe_record.hpp) and fails at the first
 *        frame whose probe or port calls, output or state after differ from what was recorded.
 *
 * Usage: tpvcamera_replay <KCD2_TPVCamera_frames_genNNNN.tpvrec>
 */

#include "core/collision/probe_record.hpp"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: tpvcamera_replay <recording.tpvrec>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto *bytes = reinterpret_cast<const std::byte *>(raw.data());
    const TPVCamera::ProbeRecord::ReplayReport report =
        TPVCamera::ProbeRecord::replay(std::span<const std::byte>(bytes, raw.size()));
    std::printf("%s: %d frame(s) (%d arm, %d shoulder, %d lens, %d rig), %lld probe and port call(s)\n", argv[1],
                report.frames, report.arm_frames, report.shoulder_frames, report.lens_frames, report.rig_frames,
                report.calls);
    if (!report.ok)
    {
        std::printf("DIVERGED: %s\n", report.failure.c_str());
        return 1;
    }
    std::printf("bit-exact\n");
    return 0;
}
