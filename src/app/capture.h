// Frame capture, for verifying what the renderer actually drew.
//
// This exists because the thing being built cannot be looked at. A screen saver takes over the
// display, and on a machine where another screen saver already owns the input desktop a test run
// is invisible to everyone including the person who started it. Reading the presented image back
// out of the swapchain is the only honest way to check that a frame contains what it should.
//
// Off unless NUKE_SAVER_CAPTURE names a path, the same way logging is off unless asked for.
#ifndef NUKE_SAVER_CAPTURE_H
#define NUKE_SAVER_CAPTURE_H

#include <cstdint>
#include <string>

namespace app {

struct CaptureRequest {
    bool        enabled = false;
    std::string path;        // where to write; a frame number is inserted before the extension
    int         atFrame = 0; // which frame to grab, so a capture can catch a specific moment
};

// Reads NUKE_SAVER_CAPTURE. The value is a path, optionally followed by "@<frame>":
//   C:\tmp\shot.png          grabs frame 0
//   C:\tmp\shot.png@300      grabs frame 300
CaptureRequest CaptureRequestFromEnvironment();

// Writes 8-bit RGB as a PNG. `pixels` is tightly packed BGRA, which is what the swapchain format
// hands back, and the swizzle happens here so no caller has to remember it.
//
// The deflate stream uses stored blocks only — no compression. That keeps the writer to a page of
// code with no dependency, and these files are written by hand, once, by someone looking at them.
bool WritePngFromBgra(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
                      uint32_t rowPitchBytes);

}  // namespace app

#endif
