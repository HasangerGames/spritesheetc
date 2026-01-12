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
#include "blockingconcurrentqueue.h"
#include "xxhash.h"
#include "glaze/glaze.hpp"

#include "spritesheetc.h"

#include <pstl/glue_execution_defs.h>

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

using Job = std::function<void(Buffer<uint32_t>&)>;

class ThreadPool {
public:
    explicit ThreadPool(uint16_t numThreads) {
        m_threads.reserve(numThreads);
        for (uint16_t i = 0; i < numThreads; ++i) {
            m_threads.emplace_back(&ThreadPool::threadLoop, this);
        }
    }
    ~ThreadPool() { shutdown(); }

    void queueJobsAndWait(std::vector<Job>& jobs) {
        size_t current = m_remainingJobs = jobs.size();
        m_queue.enqueue_bulk(m_producerToken, std::make_move_iterator(jobs.begin()), jobs.size());
        while (current > 0) {
            m_remainingJobs.wait(current, std::memory_order_acquire);
            current = m_remainingJobs.load(std::memory_order_acquire);
        }
    }

    void shutdown() {
        // Queue null jobs to shut down threads
        for (size_t i = 0; i < m_threads.size(); ++i) {
            m_queue.enqueue(nullptr);
        }
    }
private:
    void threadLoop() {
        Buffer<uint32_t> buffer;
        moodycamel::ConsumerToken consumerToken(m_queue);
        while (true) {
            Job job;
            m_queue.wait_dequeue(consumerToken, job);
            if (job == nullptr) return; // null job = shutdown signal
            job(buffer);
            if (m_remainingJobs.fetch_sub(1, std::memory_order::acq_rel) == 1) {
                m_remainingJobs.notify_one();
            }
        }
    }

    moodycamel::BlockingConcurrentQueue<Job> m_queue;
    moodycamel::ProducerToken m_producerToken{m_queue};
    std::atomic<size_t> m_remainingJobs;
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
    int16_t x, y, w, h;
};

struct SpritesheetSize {
    int16_t w, h;
};

struct SpritesheetFrame {
    SpritesheetRect frame;
    SpritesheetSize sourceSize;
    SpritesheetRect spriteSourceSize;
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

struct Sprite {
    Buffer<char> data;
    std::string name;
    uint16_t width, height;
    float bboxX = 0, bboxY = 0;
    bool trimmed;
    Rect rect;
    resvg_render_tree* tree = nullptr;

    [[nodiscard]] Rect& get_rect() { return rect; }
};

struct Atlas {
    uint16_t w, h;
    Buffer<uint32_t> pixels;
    size_t id;
    std::vector<Sprite> sprites;
    std::atomic<size_t> spritesToBeRendered;
    std::unordered_map<float, Spritesheet> spritesheets;
};

void loadSprite(const std::string& filePath, Sprite& sprite, XXH64_hash_t& hash) {
    std::ifstream input{filePath, std::ios::binary};

    input.seekg(0, std::ios::end);
    std::streamsize fileSize = input.tellg();
    input.seekg(0, std::ios::beg);

    sprite.data = Buffer<char>(fileSize);
    char* spriteData = sprite.data.data;
    input.read(spriteData, fileSize);

    hash = XXH64(spriteData, fileSize, 0) ^ XXH64(filePath.data(), filePath.size(), 0);
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

    sprite.name = std::filesystem::path(filePath).stem();
    sprite.width = width;
    sprite.height = height;

    if (config.allowTrimming) {
        resvg_rect bbox;
        resvg_get_image_bbox(sprite.tree, &bbox);
        float bx = std::floor(bbox.x);
        float by = std::floor(bbox.y);
        float bWidth = std::min((float) width, std::ceil(bbox.width));
        float bHeight = std::min((float) height, std::ceil(bbox.height));
        if (bx > 0 || by > 0 || bWidth < width || bHeight < height) {
            sprite.trimmed = true;
            sprite.bboxX = bx;
            sprite.bboxY = by;
            width = bWidth;
            height = bHeight;
        }
    }
    width += config.padding * 2;
    height += config.padding * 2;
    sprite.rect = rectpack2D::rect_xywh(-1, -1, width, height); // -1 means rect hasn't been packed yet
}

int16_t floor(int val, float scale) {
    return std::floor((float) val * scale);
}

int16_t ceil(int val, float scale) {
    return std::ceil((float) val * scale);
}

std::deque<Atlas> packAtlases(
    std::vector<Sprite>& sprites,
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
    while (!sprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(sprites, finderInput);

        int16_t binWidth = result.w - doublePadding;
        int16_t binHeight = result.h - doublePadding;
        Atlas& atlas = atlases.emplace_back(
            binWidth,
            binHeight,
            Buffer<uint32_t>(binWidth * binHeight),
            ++atlasIdx
        );

        for (float scale : config.resolutions) {
            Spritesheet spritesheet;
            spritesheet.meta.size = {
                .w = ceil(binWidth, scale),
                .h = ceil(binHeight, scale),
            };
            spritesheet.meta.scale = scale;
            atlas.spritesheets.emplace(scale, spritesheet);
        }

        for (size_t i = 0; i < sprites.size(); ) {
            Sprite& sprite = sprites[i];
            Rect& rect = sprite.rect;
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            rect.w -= doublePadding;
            rect.h -= doublePadding;

            spriteMaxW = std::max(rect.w, spriteMaxW);
            spriteMaxH = std::max(rect.h, spriteMaxH);

            atlas.sprites.push_back(std::move(sprite));
            sprites[i] = std::move(sprites.back());
            sprites.pop_back();
        }

        atlas.spritesToBeRendered = atlas.sprites.size();
    }
    return atlases;
}

void renderSprite(
    const Sprite& sprite,
    Atlas& atlas,
    const Buffer<uint32_t>& spriteBuffer,
    bool argb
) {
    const Rect& rect = sprite.rect;

    for (auto& [scale, spritesheet] : atlas.spritesheets) {
        spritesheet.frames[sprite.name] = {
            .frame = {
                .x = floor(rect.x, scale),
                .y = floor(rect.y, scale),
                .w = ceil(rect.w, scale),
                .h = ceil(rect.h, scale),
            },
            .sourceSize = {
                .w = ceil(sprite.width, scale),
                .h = ceil(sprite.height, scale),
            },
            .spriteSourceSize = {
                .x = floor(sprite.bboxX, scale),
                .y = floor(sprite.bboxY, scale),
                .w = ceil(rect.w, scale),
                .h = ceil(rect.h, scale),
            },
            .rotated = false,
            .trimmed = sprite.trimmed,
        };
    }

    int width = rect.w, height = rect.h;
    uint32_t* spriteData = spriteBuffer.data;
    std::memset(spriteData, 0, width * height * 4);

    resvg_transform transform = resvg_transform_identity();
    transform.e = -sprite.bboxX;
    transform.f = -sprite.bboxY;
    resvg_render(
        sprite.tree,
        transform,
        width,
        height,
        reinterpret_cast<char*>(spriteData)
    );
    resvg_tree_destroy(sprite.tree);

    // resvg outputs premultiplied alpha,
    // so we need to un-premultiply it before passing to the encoder or it'll cause artifacts.
    // also, if we're only outputting webp,
    // we can convert to argb (the format libwebp accepts) now to avoid a second conversion later.
    uint16_t atlasWidth = atlas.w;
    uint32_t* atlasData = atlas.pixels.data + rect.x + (rect.y * atlasWidth);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t pixel = spriteData[x];
            uint8_t r, g, b;
            uint8_t a = pixel >> 24;
            if (a == 0 || a == 255) { // skip un-premultiplying if alpha is 0 or 255
                r = pixel;
                g = pixel >> 8;
                b = pixel >> 16;
            } else {
                auto unmultiply = [a](uint8_t c) -> uint8_t {
                    return ((uint32_t) c * 255 + (a / 2)) / a;
                };
                r = unmultiply(pixel);
                g = unmultiply(pixel >> 8);
                b = unmultiply(pixel >> 16);
            }
            if (argb) {
                atlasData[x] = a << 24 | r << 16 | g << 8 | b;
            } else {
                atlasData[x] = a << 24 | b << 16 | g << 8 | r;
            }
        }
        atlasData += atlasWidth;
        spriteData += width;
    }
}

void encodeWebp(const Atlas& atlas, FILE* file, const WebPConfig& config, bool argb) {
    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        throw SpritesheetBuilderException("WebP picture init failed");
    }
    picture.width = atlas.w;
    picture.height = atlas.h;
    picture.use_argb = 1;

    if (argb) {
        picture.argb = atlas.pixels.data;
        picture.argb_stride = atlas.w;
    } else if (WebPPictureImportRGBA(
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
    bool argb,
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
                encodeWebp(atlas, file, webpConfig, argb);
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
    std::vector<Job> jobs;
    jobs.reserve(numInputFiles);
    std::vector<Sprite> sprites{numInputFiles};

    // Phase 1: Load sprites
    Timer load{config};
    Buffer<XXH64_hash_t> hashes{numInputFiles};

    for (size_t i = 0; i < numInputFiles; ++i) {
        jobs.emplace_back([&, i](Buffer<uint32_t>&) {
            loadSprite(config.inputFiles[i], sprites[i], hashes.data[i]);
        });
    }
    threadPool.queueJobsAndWait(jobs);

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

    jobs.clear();
    for (size_t i = 0; i < numInputFiles; ++i) {
        jobs.emplace_back([&, i](Buffer<uint32_t>&) {
            parseSprite(config.inputFiles[i], sprites[i], opt, config);
        });
    }
    threadPool.queueJobsAndWait(jobs);

    resvg_options_destroy(opt);
    parse.stop(std::format("[spritesheetc] {} sprites parsed", numInputFiles));

    // Phase 3: Pack sprites into atlases
    Timer pack{config};
    int spriteMaxW = 0, spriteMaxH = 0;
    std::deque<Atlas> atlases = packAtlases(sprites, spriteMaxW, spriteMaxH, config);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()));

    // Phase 4: Render sprites to atlases + encode atlases
    Timer render{config};
    bool argb = config.formats.size() == 1 && config.formats.contains(TextureFormat::Webp);
    bool multithreading = config.encoderMultithreading && atlases.size() < numThreads;

    jobs.clear();
    for (Atlas& atlas : atlases) {
        for (const Sprite& sprite : atlas.sprites) {
            jobs.emplace_back([&](Buffer<uint32_t>& spriteBuffer) {
                if (spriteBuffer.size == 0) {
                    spriteBuffer = Buffer<uint32_t>(spriteMaxW * spriteMaxH);
                }
                renderSprite(sprite, atlas, spriteBuffer, argb);

                if (atlas.spritesToBeRendered.fetch_sub(1, std::memory_order::acq_rel) == 1) {
                    encodeAtlas(atlas, config, hash, argb, multithreading);
                }
            });
        }
    }
    threadPool.queueJobsAndWait(jobs);

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
        .cache = false,
        .encoderQuality = 0,
        .encoderMethod = 0,
    });
    return 0;
}
