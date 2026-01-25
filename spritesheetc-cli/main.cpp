#include <iostream>

#include "spritesheetc.h"

using namespace spritesheetc;

int main(int argc, char* argv[]) {
    // for (int i = 0; i < argc; ++i) {
    //     std::string arg = argv[i];
    //     if (arg == "-f") {
    //
    //     }
    // }
    std::vector<std::string> outputFiles = buildSpritesheetsFromDirectories({
        "../../../../Suroi/client/public/img/game/shared",
        "../../../../Suroi/client/public/img/game/normal"
    }, {
        .cache = false,
        .maxOutputDirSize = 30'000'000,
        .formats = {TextureFormat::Webp},
        .resolutions = {1.0f},
        .speed = EncoderSpeed::Fast,
    });
    for (const std::string& file : outputFiles) std::cout << file << "\n";
    return 0;
}
