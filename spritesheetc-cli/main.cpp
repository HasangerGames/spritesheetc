#include "spritesheetc.h"

using namespace spritesheetc;

int main(int argc, char* argv[]) {
    // for (int i = 0; i < argc; ++i) {
    //     std::string arg = argv[i];
    //     if (arg == "-f") {
    //
    //     }
    // }
    buildSpritesheetsFromDirectories({
        "../../../Suroi/client/public/img/game/shared",
        "../../../Suroi/client/public/img/game/normal"
    }, {
        .formats = {TextureFormat::Ktx2Uastc, TextureFormat::Webp},
        .speed = EncoderSpeed::Fast,
    });
    return 0;
}
