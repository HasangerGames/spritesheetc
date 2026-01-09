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
#include <memory_resource>
#include <queue>

#include "basisu_comp.h"
#include "resvg.h"
#include "rectpack2D/finders_interface.h"
#include "webp/encode.h"

#include "spritesheetc.h"

using namespace spritesheetc;

class Timer {
public:
    explicit Timer(bool autostart = true) {
        if (autostart) start();
    }

    void start() {
        if (m_started) {
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

struct Buffer {
    uint32_t* data;
    size_t size;
    std::pmr::memory_resource* mem = nullptr;

    Buffer() : data(nullptr), size(0) { }

    explicit Buffer(std::pmr::memory_resource& mem, size_t size) :
        data(static_cast<uint32_t*>(mem.allocate(size * sizeof(uint32_t), alignof(uint32_t)))),
        size(size) { }
    ~Buffer() { deallocate(); }

    Buffer(Buffer&& other) noexcept : data(other.data), size(other.size), mem(other.mem) {
        other.data = nullptr;
        other.size = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            deallocate();
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    void deallocate() const {
        if (mem != nullptr) mem->deallocate(data, size * sizeof(uint32_t), alignof(uint32_t));
    }
};

using Job = std::function<void(Buffer&)>;

class ThreadPool {
public:
    std::vector<std::jthread> threads;
    std::mutex mutex;
    std::condition_variable cVar;
    std::queue<Job> jobs;

    explicit ThreadPool(uint16_t numThreads) {
        threads.reserve(numThreads);
        for (uint16_t i = 0; i < numThreads; ++i) {
            threads.emplace_back(&ThreadPool::threadLoop, this);
        }
    }
    ~ThreadPool() { shutdown(); }

    void shutdown() {
        for (std::jthread& thread : threads) thread.request_stop();
        cVar.notify_all();
    }
private:
    void threadLoop(const std::stop_token& stopToken) {
        thread_local Buffer buffer;
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex);
                cVar.wait(lock, [&] { return !jobs.empty() || stopToken.stop_requested(); });
                if (stopToken.stop_requested()) return;
                job = jobs.front();
                jobs.pop();
            }
            job(buffer);
        }
    }
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

struct Atlas;

struct SpriteData {
    std::string name;
    uint16_t width, height;
    Rect rect;
    resvg_render_tree* tree;
};

struct Sprite {
    std::unique_ptr<SpriteData> data;

    SpriteData* operator->() const { return data.operator->(); }

    [[nodiscard]] Rect& get_rect() const { return data->rect; }
};

struct Atlas {
    uint16_t w, h;
    Buffer pixels;
    std::vector<Sprite> sprites;
    Spritesheet spritesheet;
};

std::unique_ptr<SpriteData> parseSprite(
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
    int width = std::ceil(size.width), height = std::ceil(size.height);
    if (width > config.maxAtlasSize || height > config.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width {}, height {}, maximum {}",
            filePath, width, height, config.maxAtlasSize
        ));
    }

    int doublePadding = config.padding * 2;

    return std::make_unique<SpriteData>(
        std::filesystem::path(filePath).stem(),
        width,
        height,
        rectpack2D::rect_xywh(
            -1, -1,
            width + doublePadding, height + doublePadding
        ),
        tree
    );
}

std::vector<Atlas> packAtlases(
    std::vector<Sprite>& sprites,
    uint16_t& spriteMaxW,
    uint16_t& spriteMaxH,
    const SpritesheetBuilderConfig& config,
    std::pmr::memory_resource& mem
) {
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
        Atlas& atlas = atlases.emplace_back(
            binWidth,
            binHeight,
            Buffer(mem, binWidth * binHeight)
        );

        for (size_t i = 0; i < sprites.size(); ) {
            Sprite& sprite = sprites[i];
            const Rect& rect = sprite->rect;
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            spriteMaxW = std::max((uint16_t) (rect.w - doublePadding), spriteMaxW);
            spriteMaxH = std::max((uint16_t) (rect.h - doublePadding), spriteMaxH);

            atlas.spritesheet.frames[sprite->name] = {
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
    }
    return atlases;
}

void renderSprite(const Sprite& sprite, const Atlas& atlas, const Buffer& spriteBuffer) {
    uint16_t width = sprite->width;
    uint16_t height = sprite->height;
    uint32_t* spriteData = spriteBuffer.data;
    std::memset(spriteData, 0, width * height * 4);

    resvg_render(
        sprite->tree,
        resvg_transform_identity(),
        width,
        height,
        reinterpret_cast<char*>(spriteData)
    );
    resvg_tree_destroy(sprite->tree);

    uint16_t atlasWidth = atlas.w;
    const Rect& rect = sprite->rect;
    uint32_t* atlasData = atlas.pixels.data + rect.x + (rect.y * atlasWidth);
    uint16_t rowBytes = width * 4;

    for (uint16_t y = 0; y < height; ++y) {
        std::memcpy(atlasData, spriteData, rowBytes);
        atlasData += atlasWidth;
        spriteData += width;
    }
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
        reinterpret_cast<const uint8_t*>(atlas.pixels.data),
        atlas.w * 4
    ) == 0) {
        throw SpritesheetBuilderException("WebP data import failed");
    }

    std::string filename = std::format("output/atlas{}.webp", i + 1);
    FILE* output = fopen(filename.c_str(), "wb");
    if (output == nullptr) {
        throw SpritesheetBuilderException("Unable to write to file: " + filename);
    }
    constexpr size_t bufferSize = 1'000'000;
    char* buffer = new char[bufferSize];
    setvbuf(output, buffer, _IOFBF, bufferSize);

    picture.custom_ptr = output;
    picture.writer = [](const uint8_t* data, size_t size, const WebPPicture* picture) -> int {
        auto* output = static_cast<FILE*>(picture->custom_ptr);
        fwrite(data, size, 1, output);
        return 1;
    };

    if (WebPEncode(&config, &picture) == 0) {
        throw SpritesheetBuilderException("WebP encode failed");
    }

    fclose(output);
    delete[] buffer;

    WebPPictureFree(&picture);
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

    uint16_t numThreads = config.builderThreads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.builderThreads;
    ThreadPool threadPool{numThreads};

    size_t numInputFiles = config.inputFiles.size();

    // Phase 1: Parse sprites
    Timer parse;
    std::vector<Sprite> sprites{numInputFiles};
    std::latch parseLatch{(ptrdiff_t) numInputFiles};
    resvg_options* opt = resvg_options_create();

    for (size_t i = 0; i < numInputFiles; ++i) {
        threadPool.jobs.emplace([&, i](Buffer&) {
            sprites[i].data = parseSprite(config.inputFiles[i], opt, config);
            parseLatch.count_down();
        });
    }
    threadPool.cVar.notify_all();

    parseLatch.wait();
    resvg_options_destroy(opt);
    parse.stop(std::format("[spritesheetc] {} sprites parsed", numInputFiles), config.logStatus);

    // Phase 2: Pack sprites into atlases
    Timer pack;
    uint16_t spriteMaxW = 0, spriteMaxH = 0;
    std::pmr::unsynchronized_pool_resource mem;
    std::vector<Atlas> atlases = packAtlases(sprites, spriteMaxW, spriteMaxH, config, mem);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()), config.logStatus);

    WebPConfig webpConfig;
    if (config.formats.contains(TextureFormat::Webp)) {
        WebPConfigInit(&webpConfig);
        webpConfig.method = config.encoderMethod;
        webpConfig.lossless = config.encoderLossless ? 1 : 0;
        webpConfig.quality = config.encoderQuality;
        webpConfig.thread_level = config.encoderMultithreading ? 1 : 0;
    }

    // Phase 3: Render sprites to atlases + encode atlases
    Timer render;
    std::latch renderLatch{(ptrdiff_t) atlases.size()};
    std::vector<std::atomic<size_t>> remainingSprites(atlases.size());

    for (size_t i = 0; i < atlases.size(); ++i) {
        Atlas& atlas = atlases[i];
        remainingSprites[i] = atlas.sprites.size();
        for (const Sprite& sprite : atlas.sprites) {
            threadPool.jobs.emplace([&, i](Buffer& spriteBuffer) {
                if (spriteBuffer.size == 0) {
                    thread_local std::pmr::unsynchronized_pool_resource spriteMem;
                    spriteBuffer = Buffer(spriteMem, spriteMaxW * spriteMaxH);
                }
                renderSprite(sprite, atlas, spriteBuffer);

                if (remainingSprites[i].fetch_sub(1, std::memory_order::relaxed) > 1) return;

                encodeAtlas(atlas, webpConfig, config, i);
                renderLatch.count_down();
            });
        }
    }
    threadPool.cVar.notify_all();

    renderLatch.wait();
    render.stop(std::format("[spritesheetc] {} atlases written", atlases.size()), config.logStatus);

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
