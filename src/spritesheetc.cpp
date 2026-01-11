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
#include "spng.h"
#include "rectpack2D/finders_interface.h"
#include "webp/encode.h"
#include "xxhash.h"
#include "glaze/glaze.hpp"

#include "spritesheetc.h"

using namespace spritesheetc;

template<typename T>
struct Buffer {
    T* data;
    size_t size;

    Buffer() : data(nullptr), size(0) { }
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

using Job = std::function<void(size_t, Buffer<uint32_t>&)>;

class ThreadPool {
public:
    explicit ThreadPool(uint16_t numThreads) {
        m_threads.reserve(numThreads);
        for (uint16_t i = 0; i < numThreads; ++i) {
            m_threads.emplace_back(&ThreadPool::threadLoop, this);
        }
    }
    ~ThreadPool() {
        for (std::jthread& thread : m_threads) thread.request_stop();
        // wake up the threads
        ++m_jobIdx;
        m_jobIdx.notify_all();
    }

    void queueJob(size_t numItems, Job job) {
        m_currentJob = std::move(job);
        m_currentIdx = 0;
        m_numItems = numItems;
        m_latch = std::make_unique<std::latch>((ptrdiff_t) numItems);
        ++m_jobIdx;
        m_jobIdx.notify_all();
        m_latch->wait();
    }
private:
    void threadLoop(const std::stop_token& stopToken) {
        Buffer<uint32_t> buffer;
        size_t lastJobIdx = 0;
        while (true) {
            m_jobIdx.wait(lastJobIdx);
            if (stopToken.stop_requested()) return;
            lastJobIdx = m_jobIdx;

            while (true) {
                size_t idx = m_currentIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= m_numItems) break;

                m_currentJob(idx, buffer);
                m_latch->count_down();
            }
        }
    }

    Job m_currentJob;
    std::atomic<size_t> m_currentIdx;
    size_t m_numItems;
    std::atomic<size_t> m_jobIdx = 0;
    std::unique_ptr<std::latch> m_latch;
    std::vector<std::jthread> m_threads;
};

class Timer {
public:
    explicit Timer(const SpritesheetBuilderConfig& config) {
        m_disabled = !config.logStatus;
        if (m_disabled) return;
        m_start = std::chrono::steady_clock::now();
    }

    void stop(const std::string& label, double threshold = 0) {
        if (m_disabled) return;

        if (m_stopped) {
            std::cout << label << ": Already stopped\n";
            return;
        }
        m_stopped = true;

        auto elapsed = std::chrono::steady_clock::now() - m_start;
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        if (ms < threshold) return;
        std::cout << std::format("{}: {:.3f} ms\n", label, ms);
    }
private:
    std::chrono::steady_clock::time_point m_start;
    bool m_stopped = false;
    bool m_disabled;
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
    SpritesheetSize spriteSourceSize;
    bool rotated;
    bool trimmed;
};

struct SpritesheetMeta {
    std::string image;
    float scale = 1.0f;
    SpritesheetSize size{0, 0};
};

using SpritesheetFrames = std::unordered_map<std::string, SpritesheetFrame>;

struct Spritesheet {
    SpritesheetMeta meta;
    SpritesheetFrames frames;
};

struct Atlas;

struct Sprite {
    Buffer<char> data;
    std::string name;
    Rect rect;
    resvg_render_tree* tree = nullptr;
    Atlas* atlas;

    [[nodiscard]] Rect& get_rect() { return rect; }
};

struct Atlas {
    uint16_t w, h;
    Buffer<uint32_t> pixels;
    size_t id;
    std::atomic<size_t> spritesToBeRendered;
    std::unordered_map<float, Spritesheet> spritesheets;
};

uint16_t coord(int val, float scale) {
    return (uint16_t) std::round((float) val * scale);
}

void loadSprite(const std::string& filePath, Sprite& sprite, XXH64_hash_t& hash) {
    std::ifstream input{filePath, std::ios::binary};

    input.seekg(0, std::ios::end);
    std::streamsize fileSize = input.tellg();
    input.seekg(0, std::ios::beg);

    sprite.data = Buffer<char>(fileSize);
    char* spriteData = sprite.data.data;
    input.read(spriteData, fileSize);

    hash = XXH64(spriteData, fileSize, 0);
}

void parseSprite(
    const std::string& filePath,
    Sprite& sprite,
    resvg_options* opt,
    const SpritesheetBuilderConfig& config
) {
    auto err = (resvg_error) resvg_parse_tree_from_data(sprite.data.data, sprite.data.size, opt, &sprite.tree);
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

    resvg_size size = resvg_get_image_size(sprite.tree);
    uint16_t width = std::ceil(size.width), height = std::ceil(size.height);
    if (width > config.maxAtlasSize || height > config.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width {}, height {}, maximum {}",
            filePath, width, height, config.maxAtlasSize
        ));
    }

    int doublePadding = config.padding * 2;

    sprite.name = std::filesystem::path(filePath).stem();
    sprite.rect = rectpack2D::rect_xywh(
        -1, -1,
        width + doublePadding, height + doublePadding
    );
}

std::deque<Atlas> packAtlases(
    std::vector<Sprite>& unpackedSprites,
    std::vector<Sprite>& packedSprites,
    int& spriteMaxW,
    int& spriteMaxH,
    const SpritesheetBuilderConfig& config
) {
    std::deque<Atlas> atlases;

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

    size_t atlasIdx = 0;
    while (!unpackedSprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(unpackedSprites, finderInput);

        uint16_t binWidth = result.w - doublePadding;
        uint16_t binHeight = result.h - doublePadding;
        Atlas& atlas = atlases.emplace_back(
            binWidth,
            binHeight,
            Buffer<uint32_t>(binWidth * binHeight),
            ++atlasIdx
        );

        for (float scale : config.resolutions) {
            Spritesheet spritesheet;
            spritesheet.meta.size = {
                .w = coord(binWidth, scale),
                .h = coord(binHeight, scale),
            };
            spritesheet.meta.scale = scale;
            atlas.spritesheets.emplace(scale, spritesheet);
        }

        size_t spritesInAtlas = 0;
        for (size_t i = 0; i < unpackedSprites.size(); ) {
            Sprite& sprite = unpackedSprites[i];
            Rect& rect = sprite.rect;
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            rect.w -= doublePadding;
            rect.h -= doublePadding;

            spriteMaxW = std::max(rect.w, spriteMaxW);
            spriteMaxH = std::max(rect.h, spriteMaxH);

            ++spritesInAtlas;
            sprite.atlas = &atlas;

            packedSprites.push_back(std::move(sprite));
            unpackedSprites[i] = std::move(unpackedSprites.back());
            unpackedSprites.pop_back();
        }

        atlas.spritesToBeRendered = spritesInAtlas;
    }
    return atlases;
}

void renderSprite(
    const Sprite& sprite,
    Atlas& atlas,
    const Buffer<uint32_t>& spriteBuffer,
    const SpritesheetBuilderConfig& config
) {
    const Rect& rect = sprite.rect;

    for (auto& [scale, spritesheet] : atlas.spritesheets) {
        spritesheet.frames[sprite.name] = {
            .frame = {
                .x = coord(rect.x, scale),
                .y = coord(rect.y, scale),
                .w = coord(rect.w, scale),
                .h = coord(rect.h, scale),
            },
            .sourceSize = {
                .w = coord(rect.w, scale),
                .h = coord(rect.h, scale),
            },
            .rotated = false,
            .trimmed = false,
        };
    }

    int width = rect.w, height = rect.h;
    uint32_t* spriteData = spriteBuffer.data;
    std::memset(spriteData, 0, width * height * 4);

    resvg_render(
        sprite.tree,
        resvg_transform_identity(),
        width,
        height,
        reinterpret_cast<char*>(spriteData)
    );
    resvg_tree_destroy(sprite.tree);

    uint16_t atlasWidth = atlas.w;
    uint32_t* atlasData = atlas.pixels.data + rect.x + (rect.y * atlasWidth);
    uint16_t rowBytes = width * 4;

    for (int y = 0; y < height; ++y) {
        std::memcpy(atlasData, spriteData, rowBytes);
        atlasData += atlasWidth;
        spriteData += width;
    }
}

void encodeWebp(const Atlas& atlas, FILE* file, const WebPConfig& config) {
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

    picture.custom_ptr = file;
    picture.writer = [](const uint8_t* data, size_t size, const WebPPicture* picture) -> int {
        auto* file = static_cast<FILE*>(picture->custom_ptr);
        fwrite(data, size, 1, file);
        return 1;
    };

    if (WebPEncode(&config, &picture) == 0) {
        throw SpritesheetBuilderException("WebP encode failed");
    }

    WebPPictureFree(&picture);
}

void encodeKtx2(const Atlas& atlas, FILE* file, basist::basis_tex_format format, bool multithreaded) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::image image;
    image.init(reinterpret_cast<const uint8_t*>(atlas.pixels.data), atlas.w, atlas.h, 4);
    basisu::vector<basisu::image> images;
    images.push_back(image);

    uint32_t flags = basisu::cFlagKTX2;
    if (multithreaded) {
        flags |= basisu::cFlagThreaded;
    }

    size_t size;
    void* data = basisu::basis_compress(
        format,
        images,
        flags,
        1.0f,
        &size,
        nullptr
    );

    fwrite(data, size, 1, file);

    basisu::basis_free_data(data);
}

void encodePng(const Atlas& atlas, FILE* file) {
    spng_ctx* encoder = spng_ctx_new(SPNG_CTX_ENCODER);

    spng_ihdr ihdr = {
        .width = atlas.w,
        .height = atlas.h,
        .bit_depth = 8,
        .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA,
    };
    spng_set_ihdr(encoder, &ihdr);

    spng_set_png_file(encoder, file);

    int code = spng_encode_image(encoder, atlas.pixels.data, atlas.pixels.size * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if (code != 0) {
        throw SpritesheetBuilderException(std::format("PNG encode failed: {}", spng_strerror(code)));
    }

    spng_ctx_free(encoder);
}

std::string extensionFromTextureFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::Ktx2Etc1s:
    case TextureFormat::Ktx2Uastc:
        return "ktx2";
    case TextureFormat::Webp:
        return "webp";
    case TextureFormat::Png:
        return "png";
    default:
        throw SpritesheetBuilderException("No known extension for texture format");
    }
}

void encodeAtlas(
    Atlas& atlas,
    const SpritesheetBuilderConfig& config,
    XXH64_hash_t hash,
    bool multithreading
) {
    WebPConfig webpConfig;
    if (config.formats.contains(TextureFormat::Webp)) {
        WebPConfigInit(&webpConfig);
        webpConfig.method = config.encoderMethod;
        webpConfig.lossless = config.encoderLossless ? 1 : 0;
        webpConfig.quality = config.encoderQuality;
        webpConfig.thread_level = multithreading ? 1 : 0;
    }

    for (TextureFormat format : config.formats) {
        std::string extension = extensionFromTextureFormat(format);
        for (float scale : config.resolutions) {
            std::string basePath = std::format(
                "{}/{}@{}x-{:x}",
                config.outputDirectory,
                config.atlasName,
                scale,
                hash
            );
            std::string filePath = std::format("{}-{}.{}", basePath, atlas.id, extension);

            Spritesheet& spritesheet = atlas.spritesheets[scale];
            spritesheet.meta.image = filePath;
            std::string jsonPath = filePath + ".json";
            if (glz::error_ctx error = glz::write_file_json(spritesheet, jsonPath, std::string{})) {
                throw SpritesheetBuilderException(jsonPath + ": Failed to write JSON: " + glz::format_error(error));
            }

            FILE* file = fopen(filePath.c_str(), "wb");
            if (file == nullptr) {
                throw SpritesheetBuilderException("Unable to write to file: " + filePath);
            }
            setvbuf(file, nullptr, _IOFBF, 1'000'000); // 1 MB buffer
            switch (format) {
            case TextureFormat::Ktx2Etc1s:
                encodeKtx2(atlas, file, basist::basis_tex_format::cETC1S, multithreading);
                break;
            case TextureFormat::Ktx2Uastc:
                encodeKtx2(atlas, file, basist::basis_tex_format::cUASTC4x4, multithreading);
                break;
            case TextureFormat::Webp:
                encodeWebp(atlas, file, webpConfig);
                break;
            case TextureFormat::Png:
                encodePng(atlas, file);
                break;
            }
            fclose(file);

            // Create an empty file to signal to the cache system that this collection of spritesheets exists
            if (config.cache) {
                std::ofstream cacheFile{std::format("{}.{}.cache", basePath, extension)};
            }
        }
    }
}

namespace spritesheetc {

void buildSpritesheets(const SpritesheetBuilderConfig& config) {
    Timer total{config};

    if (!std::filesystem::is_directory(config.outputDirectory)) {
        throw SpritesheetBuilderException("Output path does not exist or is not a directory: " + config.outputDirectory);
    }

    // Create the thread pool
    size_t numInputFiles = config.inputFiles.size();
    uint16_t numThreads = config.builderThreads == 0
        ? std::max(1u, std::thread::hardware_concurrency())
        : config.builderThreads;
    ThreadPool threadPool{numThreads};

    // Phase 1: Hash sprites + check cache
    Timer load{config};
    std::vector<Sprite> unpackedSprites{numInputFiles};
    Buffer<XXH64_hash_t> hashes{numInputFiles};

    threadPool.queueJob(numInputFiles, [&](size_t i, Buffer<uint32_t>&) {
        loadSprite(config.inputFiles[i], unpackedSprites[i], hashes.data[i]);
    });

    load.stop(std::format("[spritesheetc] {} sprites loaded", numInputFiles));

    // Check cache
    XXH64_hash_t hash = XXH64(hashes.data, hashes.size * sizeof(XXH64_hash_t), 0);
    if (config.cache) {
        bool filesExist = true;
        for (TextureFormat format : config.formats) {
            std::string extension = extensionFromTextureFormat(format);
            for (float scale : config.resolutions) {
                std::string cachePath = std::format(
                    "{}/{}@{}x-{:x}.{}.cache",
                    config.outputDirectory,
                    config.atlasName,
                    scale,
                    hash,
                    extension
                );
                if (!std::filesystem::exists(cachePath)) {
                    filesExist = false;
                    break;
                }
            }
            if (!filesExist) break;
        }
        if (filesExist) {
            if (config.logStatus) std::cout << "[spritesheetc] Cache hit! Nothing to do, exiting.\n";
            return;
        }
    }

    // Phase 2: Parse sprites
    Timer parse{config};
    resvg_options* opt = resvg_options_create();

    threadPool.queueJob(numInputFiles, [&](size_t i, Buffer<uint32_t>&) {
        parseSprite(config.inputFiles[i], unpackedSprites[i], opt, config);
    });

    resvg_options_destroy(opt);
    parse.stop(std::format("[spritesheetc] {} sprites parsed", numInputFiles));

    // Phase 3: Pack sprites into atlases
    Timer pack{config};
    std::vector<Sprite> packedSprites;
    packedSprites.reserve(numInputFiles);
    int spriteMaxW = 0, spriteMaxH = 0;
    std::deque<Atlas> atlases = packAtlases(unpackedSprites, packedSprites, spriteMaxW, spriteMaxH, config);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()));

    // Phase 4: Render sprites to atlases + encode atlases
    Timer render{config};
    bool multithreading = config.encoderMultithreading && atlases.size() < numThreads;

    threadPool.queueJob(numInputFiles, [&](size_t i, Buffer<uint32_t>& spriteBuffer) {
        if (spriteBuffer.size == 0) {
            spriteBuffer = Buffer<uint32_t>(spriteMaxW * spriteMaxH);
        }
        Sprite& sprite = packedSprites[i];
        Atlas& atlas = *sprite.atlas;
        renderSprite(sprite, atlas, spriteBuffer, config);

        if (atlas.spritesToBeRendered.fetch_sub(1, std::memory_order::acq_rel) == 1) {
            encodeAtlas(atlas, config, hash, multithreading);
        }
    });

    render.stop(std::format("[spritesheetc] {} atlases written", atlases.size()));

    total.stop("[spritesheetc] Done. Total time");
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
        .encoderQuality = 0,
        .encoderMethod = 0,
    });
    return 0;
}
