#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "spritesheetc.h"

#include <latch>
#include <queue>

#include "basisu_comp.h"

#include "resvg.h"

#include "rectpack2D/finders_interface.h"

#include "webp/encode.h"

using namespace spritesheetc;

std::unique_ptr<SpriteData> renderSprite(
    const std::string& filePath,
    resvg_options* opt,
    const SpritesheetBuilderConfig& config
) {
    resvg_render_tree* tree;
    auto err = (resvg_error) resvg_parse_tree_from_file(filePath.c_str(), opt, &tree);
    if (err != RESVG_OK) {
        std::string errorMessage;
        switch (err) {
            case RESVG_ERROR_NOT_AN_UTF8_STR:
                errorMessage = "File is not UTF-8";
                break;
            case RESVG_ERROR_FILE_OPEN_FAILED:
                errorMessage = "Failed to open file";
                break;
            case RESVG_ERROR_MALFORMED_GZIP:
                errorMessage = "Compressed SVG must use the GZip algorithm";
                break;
            case RESVG_ERROR_ELEMENTS_LIMIT_REACHED:
                errorMessage = "SVGs with more than 1,000,000 elements are unsupported";
                break;
            case RESVG_ERROR_INVALID_SIZE:
                errorMessage = "Invalid size. width, height and viewBox must be set";
                break;
            case RESVG_ERROR_PARSING_FAILED:
                errorMessage = "Failed to parse SVG data";
                break;
            default:
                break;
        }
        throw SpritesheetBuilderException(filePath + ": Failed to parse: " + errorMessage);
    }

    resvg_size size = resvg_get_image_size(tree);

    int width      = std::ceil(size.width),
        height     = std::ceil(size.height),
        rectWidth  = width  + config.padding * 2,
        rectHeight = height + config.padding * 2;

    if (rectWidth > config.maxAtlasSize || rectHeight > config.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width + padding {}, height + padding {}, maximum {}\n",
            filePath, rectWidth, rectHeight, config.maxAtlasSize
        ));
    }

    std::string filename = filePath.substr(filePath.find_last_of("/\\") + 1);
    std::string name = filename.substr(0, filename.find_last_of('.'));

    std::unique_ptr<SpriteData> sprite = std::make_unique<SpriteData>(
        name,
        rectpack2D::rect_xywh(-1, -1, rectWidth, rectHeight),
        std::vector<uint8_t>(width * height * 4)
    );

    resvg_render(
        tree,
        resvg_transform_identity(),
        width,
        height,
        reinterpret_cast<char*>(sprite->pixels.data())
    );
    resvg_tree_destroy(tree);

    return sprite;
}

std::vector<Atlas> packAtlases(
    std::vector<Sprite>& sprites,
    const SpritesheetBuilderConfig& config
) {
    std::vector<Atlas> atlases;
    constexpr auto insertCallback = [](auto&) {
        return rectpack2D::callback_result::CONTINUE_PACKING;
    };
    auto finderInput = rectpack2D::make_finder_input(
        config.maxAtlasSize,
        config.packingQuality,
        insertCallback,
        insertCallback,
        rectpack2D::flipping_option::DISABLED
    );
    while (!sprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(sprites, finderInput);

        Atlas atlas = {
            .w = (uint16_t) result.w,
            .h = (uint16_t) result.h,
        };

        for (size_t i = 0; i < sprites.size(); ) {
            Sprite& sprite = sprites[i];
            const Rect& rect = sprite.get_rect();
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            atlas.spritesheet.frames[sprite.name()] = {
                .frame = {
                    .x = (uint16_t) rect.x,
                    .y = (uint16_t) rect.y,
                    .w = (uint16_t) rect.w,
                    .h = (uint16_t) rect.h
                }
            };

            atlas.sprites.push_back(std::move(sprite));
            sprites[i] = std::move(sprites.back());
            sprites.pop_back();
        }

        atlases.push_back(std::move(atlas));
    }
    return atlases;
}

void encodeWebp(const std::vector<uint8_t>& data, const Atlas& atlas, const WebPConfig& config, uint16_t i) {
    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        throw SpritesheetBuilderException("WebP picture init failed");
    }
    picture.width = atlas.w;
    picture.height = atlas.h;
    picture.use_argb = 1;

    if (WebPPictureImportRGBA(
        &picture,
        data.data(),
        atlas.w * 4
    ) == 0) {
        throw SpritesheetBuilderException("WebP data import failed");
    }

    std::string filename = std::format("output/atlas{}.webp", i + 1);
    std::ofstream output{filename};
    if (!output.good()) {
        throw SpritesheetBuilderException("Unable to write to file: " + filename);
    }

    picture.custom_ptr = &output;
    picture.writer = [](const uint8_t* data, size_t size, const WebPPicture* picture) -> int {
        auto* output = static_cast<std::ofstream*>(picture->custom_ptr);
        output->write(reinterpret_cast<const char*>(data), (std::streamsize) size);
        return 1;
    };

    if (WebPEncode(&config, &picture) == 0) {
        throw SpritesheetBuilderException("WebP encode failed");
    }

    WebPPictureFree(&picture);

    output.close();
}

void encodeUastc(const std::vector<uint8_t>& data, const Atlas& atlas, basist::basis_tex_format format, uint16_t i) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::image image;
    image.init(data.data(), atlas.w, atlas.h, 4);
    basisu::vector<basisu::image> images;
    images.push_back(image);

    size_t fileSize;
    void* rawData = basisu::basis_compress(
        format,
        images,
        basisu::cFlagThreaded,
        1.0f,
        &fileSize,
        nullptr
    );

    std::string filename = std::format("output/atlas{}.ktx2", i + 1);
    std::ofstream output{filename};
    if (!output.good()) {
        throw SpritesheetBuilderException("Unable to write to file: " + filename);
    }
    output.write(static_cast<char*>(rawData), (std::streamsize) fileSize);
    output.close();

    basisu::basis_free_data(rawData);
}

void encodePng(const std::vector<uint8_t>& data, const Atlas& atlas, uint16_t i) {
    basisu::image image;
    image.init(data.data(), atlas.w, atlas.h, 4);
    basisu::save_png(std::format("output/atlas{}.png", i + 1).c_str(), image);
}

void blitAtlas(const Atlas& atlas, const WebPConfig& webpConfig, const SpritesheetBuilderConfig& config, uint16_t i) {
    auto atlasBuffer = std::vector<uint8_t>((size_t) atlas.w * atlas.h * 4);

    for (const Sprite& sprite : atlas.sprites) {
        const Rect& rect = sprite.get_rect();
        int rx = rect.x + config.padding;
        int ry = rect.y + config.padding;
        int rw = rect.w - config.padding * 2;
        int rh = rect.h - config.padding * 2;

        // Copy the sprite data from the sprite buffer to the atlas buffer
        uint8_t* atlasData = atlasBuffer.data();
        uint8_t* spriteData = sprite.pixels().data();
        size_t rowStride = rw * 4;
        for (int y = 0; y < rh; ++y) {
            std::memcpy(
                atlasData + (ptrdiff_t) ((ry + y) * atlas.w + rx) * 4,
                spriteData + (ptrdiff_t) y * rw * 4,
                rowStride
            );
        }
    }

    for (TextureFormat format : config.formats) {
        switch (format) {
            case TextureFormat::BasisuEtc1s:
                encodeUastc(atlasBuffer, atlas, basist::basis_tex_format::cETC1S, i);
                break;
            case TextureFormat::BasisuUastc:
                encodeUastc(atlasBuffer, atlas, basist::basis_tex_format::cUASTC4x4, i);
                break;
            case TextureFormat::Webp:
                encodeWebp(atlasBuffer, atlas, webpConfig, i);
                break;
            case TextureFormat::Png:
                encodePng(atlasBuffer, atlas, i);
                break;
        }
    }
}

namespace spritesheetc {

void buildSpritesheets(const SpritesheetBuilderConfig& config) {
    Timer total;

    if (!std::filesystem::is_directory(config.outputDirectory)) {
        throw SpritesheetBuilderException("Output path does not exist or is not a directory: " + config.outputDirectory);
    }

    WebPConfig webpConfig;
    if (config.formats.contains(TextureFormat::Webp)) {
        WebPConfigInit(&webpConfig);
        webpConfig.method = config.encoderMethod;
        webpConfig.lossless = config.encoderLossless ? 1 : 0;
        webpConfig.quality = config.encoderQuality;
        webpConfig.thread_level = config.encoderMultithreading ? 1 : 0;
    }

    std::vector<std::thread> threadPool;
    uint16_t numThreads = config.builderThreads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.builderThreads;
    threadPool.reserve(numThreads);

    size_t numInputFiles = config.inputFiles.size();

    std::atomic<size_t> spritesRendered = 0,
                        atlasesEncoded = 0;

    // Used to block the main thread while the worker threads do work
    std::latch renderingFinished{numThreads};
    std::latch atlasEncodingFinished{numThreads};

    // Used to block the worker threads while the main thread does work
    std::latch atlasPackingFinished{1};

    std::vector<Sprite> sprites{numInputFiles};

    std::vector<Atlas> atlases;

    Timer parseSprites;

    for (uint16_t i = 0; i < numThreads; ++i) {
        threadPool.emplace_back([&, i] {
            // Step 1: Render sprites
            resvg_options* opt = resvg_options_create();
            while (true) {
                size_t idx = spritesRendered.fetch_add(1, std::memory_order::relaxed);
                if (idx >= numInputFiles) break;

                sprites[idx].data = renderSprite(config.inputFiles[idx], opt, config);
            }
            resvg_options_destroy(opt);
            renderingFinished.count_down();

            // Step 2: Pack sprites into atlases (done on main thread)
            atlasPackingFinished.wait();

            // Step 3: Encode
            while (true) {
                size_t idx = atlasesEncoded.fetch_add(1, std::memory_order::relaxed);
                if (idx >= atlases.size()) break;

                Timer render;
                const Atlas& atlas = atlases[idx];
                blitAtlas(atlas, webpConfig, config, idx);
                render.stop(std::format(
                    "[spritesheetc] atlas {}/{} ({} sprites) rendered",
                    i, atlases.size(), atlas.sprites.size()
                ), config.logStatus);
            }
            atlasEncodingFinished.count_down();
        });
    }

    // Step 1: Render sprites
    renderingFinished.wait();
    parseSprites.stop(std::format("[spritesheetc] {} sprites rendered", numInputFiles), config.logStatus);

    // Step 2: Pack sprites into atlases
    Timer pack;
    atlases = packAtlases(sprites, config);
    atlasPackingFinished.count_down();
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()), config.logStatus);

    // Step 3: Encode atlases
    atlasEncodingFinished.wait();
    total.stop(std::format("[spritesheetc] {} atlases rendered", atlases.size()), config.logStatus);

    for (std::thread& thread : threadPool) thread.join();
}

void readDirectory(const std::string& path, std::vector<std::string>& files) {
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_directory()) continue;
        std::string filePath = entry.path();
        if (!filePath.ends_with(".svg")) continue;
        files.emplace_back(filePath);
    }
}

std::vector<std::string> imagePathsFromDirectory(const std::string& path) {
    std::vector<std::string> files;
    readDirectory(path, files);
    return files;
}

std::vector<std::string> imagePathsFromDirectories(const std::vector<std::string>& paths) {
    std::vector<std::string> files;
    for (const std::string& path : paths) {
        readDirectory(path, files);
    }
    return files;
}

std::vector<std::string> imagePathsFromFileList(const std::string& path) {
    std::vector<std::string> files;
    auto inputFile = std::ifstream(path);
    std::string filePath;
    while (std::getline(inputFile, filePath)) {
        if (filePath.starts_with("#")) continue; // allow comments
        if (!filePath.ends_with(".svg")) continue; // ignore anything that isn't an SVG

        files.push_back(filePath);
    }
    return files;
}

}

int main(int argc, char* argv[]) {
    // for (int i = 0; i < argc; ++i) {
    //     std::string arg = argv[i];
    //     if (arg == "-f") {
    //
    //     }
    // }
    buildSpritesheets({
        .inputFiles = imagePathsFromDirectories({
            "../../Suroi/client/public/img/game/shared",
            "../../Suroi/client/public/img/game/normal"
        }),
        .formats = {TextureFormat::Webp},
        .encoderMultithreading = false,
        .encoderQuality = 0,
        .encoderMethod = 0,
    });
    return 0;
}
