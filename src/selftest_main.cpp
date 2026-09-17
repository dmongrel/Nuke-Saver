// Console harness for the M0 toolchain probe.
//
// The shipped .scr is a Windows-subsystem binary with no stdout and, per spec 9.1, exits 0 on
// any argument it does not recognise — so it is the wrong place to put a diagnostic mode. This
// builds as a separate console executable (`make selftest`) that links the same objects.

#include "render/vk_probe.h"

#include <cstdio>

int main() {
    probe::Result r = probe::Run();

    std::printf("nuke-saver M0 selftest\n");
    std::printf("----------------------\n");
    std::printf("shaders embedded : %u blob(s), %u bytes\n", r.shaderCount, r.shaderBytes);
    std::printf("stage reached    : %s\n", r.stage.c_str());
    std::printf("physical devices : %u\n", r.deviceCount);
    if (!r.apiVersion.empty()) std::printf("device api       : %s\n", r.apiVersion.c_str());
    std::printf("detail           : %s\n", r.detail.c_str());
    std::printf("result           : %s\n", r.ok ? "PASS" : "FAIL");

    return r.ok ? 0 : 1;
}
