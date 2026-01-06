#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <latch>
#include <iostream>

#include "basisu_comp.h"
#include "resvg.h"
#include "rectpack2D/finders_interface.h"
#include "webp/encode.h"

#include "spritesheetc.h"

using namespace spritesheetc;

/**
 * Simple timer for profiling code.
 */
class Timer {
public:
    explicit Timer(bool autostart = true) {
        if (autostart) start();
    }

    void start() {
        if (m_started) {
            std::cout << "Timer already started\n";
            return;
        }
        m_start = std::chrono::steady_clock::now();
        m_started = true;
    }

    void stop(const std::string& label, bool enableLogging = true, double threshold = 0) {
        if (!m_start.has_value()) {
            std::cout << label << ": Invalid time\n";
            return;
        }

        if (m_stopped) {
            std::cout << label << ": Already stopped\n";
            return;
        }
        m_stopped = true;

        if (!enableLogging) return;

        auto elapsed = std::chrono::steady_clock::now() - m_start.value();
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        if (ms < threshold) return;
        std::cout << std::format("{}: {:.3f} ms\n", label, ms);
    }
private:
    std::optional<std::chrono::steady_clock::time_point> m_start = std::nullopt;
    bool m_started = false;
    bool m_stopped = false;
};

using SpacesType = rectpack2D::empty_spaces<false>;
using Rect = rectpack2D::output_rect_t<SpacesType>;

struct SpritesheetRect {
    uint16_t x, y, w, h;
};

struct SpritesheetSize {
    uint16_t w, h;
};

struct SpritesheetFrame {
    SpritesheetRect frame;
    SpritesheetSize sourceSize;
};

struct SpritesheetMeta {
    std::string image;
    float scale;
    SpritesheetSize size;
};

using SpritesheetFrames = std::unordered_map<std::string, SpritesheetFrame>;

struct Spritesheet {
    SpritesheetMeta meta;
    SpritesheetFrames frames;
};

struct SpriteData {
    std::string name;
    Rect rect;
    std::vector<uint8_t> pixels;
};

struct Sprite {
    std::unique_ptr<SpriteData> data;

    [[nodiscard]] const std::string& name() const { return data->name; }

    [[nodiscard]] auto& get_rect() { return data->rect; }
    [[nodiscard]] const auto& get_rect() const { return data->rect; }

    [[nodiscard]] std::vector<uint8_t>& pixels() const { return data->pixels; }
};

struct Atlas {
    uint16_t w, h;
    std::vector<Sprite> sprites;
    Spritesheet spritesheet;
    std::vector<uint8_t> pixels;
};

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

void encodeWebp(const Atlas& atlas, const WebPConfig& config, uint16_t i) {
    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        throw SpritesheetBuilderException("WebP picture init failed");
    }
    picture.width = atlas.w;
    picture.height = atlas.h;
    picture.use_argb = 1;

    if (WebPPictureImportRGBA(
        &picture,
        atlas.pixels.data(),
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

void encodeUastc(const Atlas& atlas, basist::basis_tex_format format, uint16_t i) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::image image;
    image.init(atlas.pixels.data(), atlas.w, atlas.h, 4);
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

void encodePng(const Atlas& atlas, uint16_t i) {
    basisu::image image;
    image.init(atlas.pixels.data(), atlas.w, atlas.h, 4);
    basisu::save_png(std::format("output/atlas{}.png", i + 1).c_str(), image);
}

void blitAtlas(Atlas& atlas, const SpritesheetBuilderConfig& config) {
    atlas.pixels = std::vector<uint8_t>((size_t) atlas.w * atlas.h * 4);
    uint8_t* atlasData = atlas.pixels.data();

    for (const Sprite& sprite : atlas.sprites) {
        const Rect& rect = sprite.get_rect();
        int rx = rect.x + config.padding;
        int ry = rect.y + config.padding;
        int rw = rect.w - config.padding * 2;
        int rh = rect.h - config.padding * 2;

        uint8_t* spriteData = sprite.pixels().data();
        size_t rowStride = rw * 4;
        for (int y = 0; y < rh; ++y) {
            ptrdiff_t dstOffset = ((ry + y) * atlas.w + rx) * 4;
            ptrdiff_t srcOffset = y * rw * 4;
            std::memcpy(atlasData + dstOffset, spriteData + srcOffset, rowStride);
        }
    }
}

void encodeAtlas(const Atlas& atlas, const WebPConfig& webpConfig, const SpritesheetBuilderConfig& config, uint16_t i) {
    for (TextureFormat format : config.formats) {
        switch (format) {
        case TextureFormat::BasisuEtc1s:
            encodeUastc(atlas, basist::basis_tex_format::cETC1S, i);
            break;
        case TextureFormat::BasisuUastc:
            encodeUastc(atlas, basist::basis_tex_format::cUASTC4x4, i);
            break;
        case TextureFormat::Webp:
            encodeWebp(atlas, webpConfig, i);
            break;
        case TextureFormat::Png:
            encodePng(atlas, i);
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

    std::vector<std::thread> threadPool;
    uint16_t numThreads = config.builderThreads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.builderThreads;
    threadPool.reserve(numThreads);

    auto job = [&](size_t numItems, const std::function<void(size_t idx)>& exec) {
        threadPool.clear();
        std::atomic<size_t> numFinishedItems = 0;
        for (uint16_t i = 0; i < numThreads; ++i) {
            threadPool.emplace_back([&] {
                while (true) {
                    size_t idx = numFinishedItems.fetch_add(1, std::memory_order::relaxed);
                    if (idx >= numItems) break;

                    exec(idx);
                }
            });
        }
        for (std::thread& thread : threadPool) thread.join();
    };

    size_t numInputFiles = config.inputFiles.size();

    // Step 1: Render sprites
    Timer renderSprites;
    std::vector<Sprite> sprites{numInputFiles};
    resvg_options* opt = resvg_options_create();
    job(numInputFiles, [&](size_t idx) {
        sprites[idx].data = renderSprite(config.inputFiles[idx], opt, config);
    });
    resvg_options_destroy(opt);
    renderSprites.stop(std::format("[spritesheetc] {} sprites rendered", numInputFiles), config.logStatus);

    // Step 2: Pack sprites into atlases
    Timer pack;
    std::vector<Atlas> atlases = packAtlases(sprites, config);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()), config.logStatus);

    // Step 3: Render atlases
    Timer render;
    WebPConfig webpConfig;
    if (config.formats.contains(TextureFormat::Webp)) {
        WebPConfigInit(&webpConfig);
        webpConfig.method = config.encoderMethod;
        webpConfig.lossless = config.encoderLossless ? 1 : 0;
        webpConfig.quality = config.encoderQuality;
        webpConfig.thread_level = config.encoderMultithreading ? 1 : 0;
    }
    job(atlases.size(), [&](size_t idx) {
        blitAtlas(atlases[idx], config);
        encodeAtlas(atlases[idx], webpConfig, config, idx);
    });
    render.stop(std::format("[spritesheetc] {} atlases rendered", atlases.size()), config.logStatus);

    total.stop("[spritesheetc] Done. Total time", config.logStatus);
}

void readDirectory(const std::string& path, std::vector<std::string>& files) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(path)) {
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
        .encoderQuality = 0,
        .encoderMethod = 0,
    });
    return 0;
}
