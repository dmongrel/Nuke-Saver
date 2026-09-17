#include "app/capture.h"

#include "app/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace app {
namespace {

uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc = 0xFFFFFFFFu) {
    // Table built on first use. A screen saver has no business carrying a 1 KB constant table for
    // a diagnostic that usually never runs.
    static uint32_t table[256];
    static bool     ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t Adler32(const uint8_t* data, size_t length) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < length; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void PushBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PushChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    PushBe32(out, static_cast<uint32_t>(payload.size()));

    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());

    const uint32_t crc = Crc32(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu;
    PushBe32(out, crc);
}

}  // namespace

CaptureRequest CaptureRequestFromEnvironment() {
    CaptureRequest request;

    const char* value = std::getenv("NUKE_SAVER_CAPTURE");
    if (!value || !*value) return request;

    std::string spec(value);

    // "@<frame>" is looked for after the last path separator, so a drive letter's colon and any
    // directory name containing an @ cannot be mistaken for the separator.
    const size_t slash = spec.find_last_of("\\/");
    const size_t at    = spec.find('@', slash == std::string::npos ? 0 : slash);

    if (at != std::string::npos) {
        request.atFrame = std::atoi(spec.c_str() + at + 1);
        spec.resize(at);
    }

    request.path    = spec;
    request.enabled = !request.path.empty();
    return request;
}

bool WritePngFromBgra(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
                      uint32_t rowPitchBytes) {
    if (!path || !pixels || width == 0 || height == 0) return false;

    // Raw PNG scanlines: one filter byte per row, then RGB. Filter 0 (None) throughout, because
    // nothing is compressed anyway and a filter would only make the bytes less predictable.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));

    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = pixels + static_cast<size_t>(y) * rowPitchBytes;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            raw.push_back(px[2]);  // B8G8R8A8 -> R
            raw.push_back(px[1]);  // G
            raw.push_back(px[0]);  // B
        }
    }

    // A zlib stream of stored (uncompressed) deflate blocks. Legal, trivially correct, and about
    // a page of code instead of a dependency.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);  // CMF: deflate, 32K window
    zlib.push_back(0x01);  // FLG: no preset dictionary, fastest level

    const size_t kMaxBlock = 65535;
    size_t       offset    = 0;
    while (offset < raw.size()) {
        const size_t   chunk = raw.size() - offset < kMaxBlock ? raw.size() - offset : kMaxBlock;
        const bool     last  = (offset + chunk) >= raw.size();
        const uint16_t len   = static_cast<uint16_t>(chunk);

        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(len & 0xFF));
        zlib.push_back(static_cast<uint8_t>(len >> 8));
        zlib.push_back(static_cast<uint8_t>(~len & 0xFF));
        zlib.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + chunk);

        offset += chunk;
    }
    PushBe32(zlib, Adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    PushBe32(ihdr, width);
    PushBe32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // colour type: truecolour RGB
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", zlib);
    PushChunk(png, "IEND", {});

    std::FILE* file = std::fopen(path, "wb");
    if (!file) {
        Log("capture: cannot open %s", path);
        return false;
    }
    const size_t written = std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);

    if (written != png.size()) {
        Log("capture: short write to %s", path);
        return false;
    }

    Log("capture: wrote %s (%ux%u, %zu bytes)", path, width, height, png.size());
    return true;
}

}  // namespace app
