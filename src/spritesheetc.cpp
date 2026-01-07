#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <latch>
#include <iostream>

#include "basisu_comp.h"
#include "rectpack2D/finders_interface.h"
#include "webp/encode.h"

#include "spritesheetc.h"

#include "thorvg.h"

using namespace spritesheetc;

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

template<typename T>
struct Buffer {
    T* data;
    size_t size;

    explicit Buffer(size_t size) : data(new T[size]), size(size) { }
    ~Buffer() { delete[] data; }

    Buffer(Buffer&& other) noexcept : data(other.data), size(other.size) {
        other.data = nullptr;
        other.size = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data;
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
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
    tvg::Picture* picture;
};

struct Sprite {
    std::unique_ptr<SpriteData> data;

    [[nodiscard]] const std::string& name() const { return data->name; }

    [[nodiscard]] Rect& get_rect() const { return data->rect; }

    [[nodiscard]] tvg::Picture* picture() const { return data->picture; }
};

struct Atlas {
    uint16_t w, h;
    uint16_t spriteMaxW, spriteMaxH;
    std::vector<Sprite> sprites;
    Spritesheet spritesheet;
    Buffer<uint32_t> pixels;
};

std::unique_ptr<SpriteData> parseSprite(
    const std::string& filePath,
    const SpritesheetBuilderConfig& config
) {
    tvg::Picture* picture = tvg::Picture::gen();
    if (picture->load(filePath.c_str()) != tvg::Result::Success) {
        throw SpritesheetBuilderException(filePath + ": Failed to load image");
    }

    float fWidth, fHeight;
    if (picture->bounds(nullptr, nullptr, &fWidth, &fHeight) != tvg::Result::Success) {
        throw SpritesheetBuilderException(filePath + ": Failed to parse bounds");
    }

    int width = std::ceil(fWidth), height = std::ceil(fHeight);
    if (width > config.maxAtlasSize || height > config.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width {}, height {}, maximum {}",
            filePath, width, height, config.maxAtlasSize
        ));
    }

    std::string filename = filePath.substr(filePath.find_last_of("/\\") + 1);
    std::string name = filename.substr(0, filename.find_last_of('.'));
    int doublePadding = config.padding * 2;

    return std::make_unique<SpriteData>(
        name,
        rectpack2D::rect_xywh(
            -1, -1,
            width + doublePadding, height + doublePadding
        ),
        picture
    );
}

std::vector<Atlas> packAtlases(std::vector<Sprite>& sprites, const SpritesheetBuilderConfig& config) {
    std::vector<Atlas> atlases;
    constexpr auto insertCallback = [](auto&) {
        return rectpack2D::callback_result::CONTINUE_PACKING;
    };
    uint16_t doublePadding = config.padding * 2;
    auto finderInput = rectpack2D::make_finder_input(
        config.maxAtlasSize + doublePadding,
        config.packingQuality,
        insertCallback,
        insertCallback,
        rectpack2D::flipping_option::DISABLED
    );
    while (!sprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(sprites, finderInput);
        uint16_t binWidth = result.w - doublePadding;
        uint16_t binHeight = result.h - doublePadding;

        Atlas atlas = {
            .w = binWidth,
            .h = binHeight,
            .pixels = Buffer<uint32_t>(binWidth * binHeight)
        };

        for (size_t i = 0; i < sprites.size(); ) {
            Sprite& sprite = sprites[i];
            const Rect& rect = sprite.get_rect();
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            atlas.spriteMaxW = std::max((uint16_t) (rect.w - doublePadding), atlas.spriteMaxW);
            atlas.spriteMaxH = std::max((uint16_t) (rect.h - doublePadding), atlas.spriteMaxH);

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
    picture.argb = atlas.pixels.data;
    picture.argb_stride = atlas.w;

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

void encodeUastc(const Atlas& atlas, basist::basis_tex_format format, bool multithreaded, uint16_t i) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::image image;
    image.init(reinterpret_cast<const uint8_t*>(atlas.pixels.data), atlas.w, atlas.h, 4);
    basisu::vector<basisu::image> images;
    images.push_back(image);

    size_t fileSize;
    void* rawData = basisu::basis_compress(
        format,
        images,
        multithreaded ? basisu::cFlagThreaded : 0,
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
    image.init(reinterpret_cast<const uint8_t*>(atlas.pixels.data), atlas.w, atlas.h, 4);
    basisu::save_png(std::format("output/atlas{}.png", i + 1).c_str(), image);
}

void renderAtlas(Atlas& atlas, const SpritesheetBuilderConfig& config) {
    uint32_t* atlasData = atlas.pixels.data;

    tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
    canvas->target(atlasData, atlas.w, atlas.w, atlas.h, tvg::ColorSpace::ARGB8888);

    for (const Sprite& sprite : atlas.sprites) {
        tvg::Picture* picture = sprite.picture();
        const Rect& rect = sprite.get_rect();
        picture->translate(rect.x, rect.y);
        canvas->push(sprite.picture());
    }

    canvas->draw();
}

void encodeAtlas(const Atlas& atlas, const WebPConfig& webpConfig, const SpritesheetBuilderConfig& config, uint16_t i) {
    for (TextureFormat format : config.formats) {
        switch (format) {
        case TextureFormat::BasisuEtc1s:
            encodeUastc(atlas, basist::basis_tex_format::cETC1S, config.encoderMultithreading, i);
            break;
        case TextureFormat::BasisuUastc:
            encodeUastc(atlas, basist::basis_tex_format::cUASTC4x4, config.encoderMultithreading, i);
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

    tvg::Initializer::init(numThreads);

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

    // Phase 1: Parse sprites
    Timer parseSprites;
    std::vector<Sprite> sprites{numInputFiles};
    job(numInputFiles, [&](size_t idx) {
        sprites[idx].data = parseSprite(config.inputFiles[idx], config);
    });
    parseSprites.stop(std::format("[spritesheetc] {} sprites parsed", numInputFiles), config.logStatus);

    // Phase 2: Pack sprites into atlases
    Timer pack;
    std::vector<Atlas> atlases = packAtlases(sprites, config);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()), config.logStatus);

    // Phase 3: Render atlases
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
        renderAtlas(atlases[idx], config);
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
        .encoderMultithreading = false,
        .encoderQuality = 0,
        .encoderMethod = 0,
    });
    return 0;
}
