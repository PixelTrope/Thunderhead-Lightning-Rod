#pragma once

#include <string>

namespace tlr {

// What Thunderhead reads out of a Bryce scene file (.br7, .br6) for a render
// job. The file is a "Bryce_6.0_File" container of zlib streams; one of the
// last streams inflates to a 416-byte render settings record whose first
// four big-endian 32-bit words are document width and height, then render
// width and height (equal unless the render size is a multiple), with the
// animation length in seconds as a big-endian float at 0x3c and the frame
// rate at 0x40. Verified on 604 scenes of the farm's RENDER.BIN: 15, 24 and
// 30 fps throughout (10 and 1 fps on a few), length 0 for a still; the last
// frame Bryce shows is length times rate (30 s at 30 fps shows frames 0 to
// 900). The record lies near the end of the file, so the file is read from
// the end in one-megabyte steps and the scan stops at the first record.
struct BryceSceneInfo {
    int width, height;      // the render size: what the frames and Bryce's own .bmp have
    int docWidth, docHeight;   // the document size; the render size is a multiple of it (200 % renders twice it)
    double seconds;         // animation length; 0 for a still image
    double fps;
    int lastFrame;          // seconds * fps rounded; 0 for a still
    BryceSceneInfo() : width(0), height(0), docWidth(0), docHeight(0), seconds(0), fps(0), lastFrame(0) {}
    bool animation() const { return lastFrame > 0; }
};

bool readBryceScene(const std::wstring& path, BryceSceneInfo& out, std::string& error);

// A 24-bit uncompressed BMP of one color, the format Bryce writes beside a
// scene when "Image with Scene Open/Save" is on; a neutral 50 % gray one in
// the job folder keeps Bryce from rendering when it opens the scene.
bool writeSolidBmp(const std::wstring& path, int width, int height, unsigned char gray, std::string& error);

} // namespace tlr
