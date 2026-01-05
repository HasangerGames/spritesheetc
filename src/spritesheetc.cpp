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

#include "basisu_comp.h"

#include "resvg.h"

#include "rectpack2D/finders_interface.h"

#include "webp/encode.h"

using namespace spritesheetc;

Sprite parseSprite(const std::string& filePath, resvg_options* opt, const SpritesheetBuilderConfig& config) {
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
    uint16_t width = (uint16_t) std::ceil(size.width) + config.padding * 2;
    uint16_t height = (uint16_t) std::ceil(size.height) + config.padding * 2;
    if (width > config.maxAtlasSize || height > config.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width + padding {}, height + padding {}, maximum {}\n",
            filePath, width, height, config.maxAtlasSize
        ));
    }

    std::string filename = filePath.substr(filePath.find_last_of("/\\") + 1);
    std::string name = filename.substr(0, filename.find_last_of('.'));
    return {
        name,
        rectpack2D::rect_xywh(-1, -1, width, height),
        tree
    };
}

std::vector<Atlas> packAtlases(std::vector<Sprite>& sprites, const SpritesheetBuilderConfig& config) {
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
            const Sprite& sprite = sprites[i];
            if (sprite.rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            atlas.spriteMaxW = std::max(atlas.spriteMaxW, (uint16_t) sprite.rect.w);
            atlas.spriteMaxH = std::max(atlas.spriteMaxH, (uint16_t) sprite.rect.h);

            atlas.spritesheet.frames[sprite.name] = {
                .frame = {
                    .x = (uint16_t) sprite.rect.x,
                    .y = (uint16_t) sprite.rect.y,
                    .w = (uint16_t) sprite.rect.w,
                    .h = (uint16_t) sprite.rect.h
                }
            };

            // move sprite from sprites to packedSprites
            atlas.sprites.push_back(std::move(sprites[i]));
            sprites[i] = std::move(sprites.back());
            sprites.pop_back();
        }

        atlases.emplace_back(atlas);
    }
    return atlases;
}

void encodeWebp(const Buffer& data, const Atlas& atlas, const WebPConfig& config, uint16_t i) {
    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        throw SpritesheetBuilderException("WebP picture init failed");
    }
    picture.width = atlas.w;
    picture.height = atlas.h;
    picture.use_argb = 1;

    if (WebPPictureImportRGBA(
        &picture,
        reinterpret_cast<const uint8_t*>(data.data()),
        atlas.w * 4
    ) == 0) {
        throw SpritesheetBuilderException("WebP data import failed");
    }

    std::string filename = std::format("output/atlas{}.webp", i + 1);
    auto output = std::ofstream(filename);
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

void encodeUastc(const Buffer& data, const Atlas& atlas, basist::basis_tex_format format, uint16_t i) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::image image;
    image.init(reinterpret_cast<const uint8_t*>(data.data()), atlas.w, atlas.h, 4);
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
    auto output = std::ofstream(filename);
    if (!output.good()) {
        throw SpritesheetBuilderException("Unable to write to file: " + filename);
    }
    output.write(static_cast<char*>(rawData), (std::streamsize) fileSize);
    output.close();

    basisu::basis_free_data(rawData);
}

void encodePng(const Buffer& data, const Atlas& atlas, uint16_t i) {
    basisu::image image;
    image.init(reinterpret_cast<const uint8_t*>(data.data()), atlas.w, atlas.h, 4);
    basisu::save_png(std::format("output/atlas{}.png", i + 1).c_str(), image);
}

void renderAtlas(const Atlas& atlas, const WebPConfig& webpConfig, const SpritesheetBuilderConfig& config, uint16_t i) {
    auto atlasBuffer = Buffer((size_t) atlas.w * atlas.h * 4);
    auto spriteBuffer = Buffer((size_t) atlas.spriteMaxW * atlas.spriteMaxH * 4);

    for (size_t j = 0; j < atlas.sprites.size(); ++j) {
        const Sprite& sprite = atlas.sprites[j];
        Rect rect = sprite.rect;

        // Zero out only the portion of the sprite buffer we need
        // Don't bother zeroing if we just allocated the buffer (j == 0)
        if (j != 0) std::memset(spriteBuffer.data(), 0, (size_t) rect.w * rect.h * 4);

        rect.x += config.padding;
        rect.y += config.padding;
        rect.w -= config.padding * 2;
        rect.h -= config.padding * 2;

        resvg_render(
            sprite.tree,
            resvg_transform_identity(),
            rect.w,
            rect.h,
            spriteBuffer.data()
        );
        resvg_tree_destroy(sprite.tree);

        // Copy the sprite data from the sprite buffer to the atlas buffer
        for (int y = 0; y < rect.h; ++y) {
            std::memcpy(
                atlasBuffer.data() + (ptrdiff_t) ((rect.y + y) * atlas.w + rect.x) * 4,
                spriteBuffer.data() + (ptrdiff_t) y * rect.w * 4,
                (size_t) rect.w * 4
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

    uint16_t numThreads = config.builderThreads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.builderThreads;
    std::vector<std::thread> threadPool;
    threadPool.reserve(numThreads);

    Timer parseSprites;

    size_t numInputFiles = config.inputFiles.size();
    auto poolSprites = std::vector<std::vector<Sprite>>(numThreads);
    auto fileIndex = std::atomic<size_t>(0);
    for (uint16_t i = 0; i < numThreads; ++i) {
        threadPool.emplace_back([&, i] {
            std::vector<Sprite>& threadSprites = poolSprites[i];
            threadSprites.reserve(numInputFiles / numThreads);
            resvg_options* opt = resvg_options_create();
            while (true) {
                size_t idx = fileIndex.fetch_add(1, std::memory_order::relaxed);
                if (idx >= numInputFiles) break;

                threadSprites.emplace_back(parseSprite(config.inputFiles[idx], opt, config));
            }
            resvg_options_destroy(opt);
        });
    }
    for (std::thread& thread : threadPool) {
        thread.join();
    }

    std::vector<Sprite> sprites;
    sprites.reserve(numInputFiles);
    for (std::vector<Sprite>& threadSprites : poolSprites) {
        sprites.insert(
            sprites.end(),
            std::make_move_iterator(threadSprites.begin()),
            std::make_move_iterator(threadSprites.end())
        );
    }

    parseSprites.stop(std::format("[spritesheetc] {} sprites parsed", sprites.size()), config.logStatus);

    Timer pack;
    std::vector<Atlas> atlases = packAtlases(sprites, config);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()), config.logStatus);

    WebPConfig webpConfig;
    if (config.formats.contains(TextureFormat::Webp)) {
        WebPConfigInit(&webpConfig);
        webpConfig.method = config.encoderMethod;
        webpConfig.lossless = config.encoderLossless ? 1 : 0;
        webpConfig.quality = config.encoderQuality;
        webpConfig.thread_level = config.encoderMultithreading ? 1 : 0;
    }

    auto atlasIndex = std::atomic<size_t>(0);
    auto atlasesFinished = std::atomic<size_t>(1);
    threadPool.clear();
    for (uint16_t i = 0; i < numThreads; ++i) {
        threadPool.emplace_back([&] {
            while (true) {
                size_t idx = atlasIndex.fetch_add(1, std::memory_order::relaxed);
                if (idx >= atlases.size()) break;

                Timer render;
                const Atlas& atlas = atlases[idx];
                renderAtlas(atlas, webpConfig, config, idx);
                render.stop(std::format(
                    "[spritesheetc] atlas {}/{} ({} sprites) rendered",
                    atlasesFinished.fetch_add(1), atlases.size(), atlas.sprites.size()
                ), config.logStatus);
            }
        });
    }
    for (std::thread& thread : threadPool) {
        thread.join();
    }

    total.stop(std::format("[spritesheetc] {} atlases rendered", atlases.size()), config.logStatus);
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
    });
    return 0;
}
