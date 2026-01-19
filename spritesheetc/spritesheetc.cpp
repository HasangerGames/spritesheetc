#include <atomic>
#include <iostream>
#include <thread>

#include "basisu_comp.h"
#include "resvg.h"
#include "spng.h"
#include "rectpack2D/finders_interface.h"
#include "webp/encode.h"
#include "blockingconcurrentqueue.h"
#include "xxhash.h"
#include "glaze/glaze.hpp"

#include "spritesheetc.h"

using namespace spritesheetc;

class ThreadPool {
public:
    explicit ThreadPool(uint16_t numThreads) {
        if (numThreads == 1) return;
        m_threads.reserve(numThreads);
        for (uint16_t i = 0; i < numThreads; ++i) {
            m_threads.emplace_back(&ThreadPool::threadLoop, this);
        }
    }
    ~ThreadPool() { shutdown(); }

    void reset(size_t numJobs) {
        if (m_threads.empty()) return;
        m_remainingJobs.store(numJobs, std::memory_order_relaxed);
    }

    void queueJob(std::move_only_function<void()> job) {
        if (m_threads.empty()) {
            job();
        } else {
            thread_local moodycamel::ProducerToken token{m_queue};
            m_queue.enqueue(token, std::move(job));
        }
    }

    void waitForJobs() const {
        if (m_threads.empty()) return;
        size_t current = m_remainingJobs;
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
        while (true) {
            std::move_only_function<void()> job;
            thread_local moodycamel::ConsumerToken token{m_queue};
            m_queue.wait_dequeue(token, job);
            if (job == nullptr) return; // null job = shutdown signal
            job();
            if (m_remainingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                m_remainingJobs.notify_one();
            }
        }
    }
    moodycamel::BlockingConcurrentQueue<std::move_only_function<void()>> m_queue;
    std::atomic<size_t> m_remainingJobs;
    std::vector<std::jthread> m_threads;
};

class Timer {
public:
    explicit Timer(bool enable = true) : m_disabled(!enable) {
        if (!m_disabled) m_start = std::chrono::steady_clock::now();
    }

    void stop(const std::string& label, double threshold = 0) {
        if (m_disabled) return;
        auto elapsed = std::chrono::steady_clock::now() - m_start;
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        if (ms < threshold) return;
        std::cout << std::format("{}: {:.3f} ms\n", label, ms);
    }
private:
    std::chrono::steady_clock::time_point m_start;
    bool m_disabled;
};

using SpacesType = rectpack2D::empty_spaces<true>;
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
    std::unique_ptr<char[]> data;
    size_t dataSize = 0;
    std::string name;
    uint16_t width = 0, height = 0;
    float bboxX = 0, bboxY = 0;
    bool trimmed = false;
    Rect rect;
    resvg_render_tree* tree = nullptr;

    [[nodiscard]] Rect& get_rect() { return rect; }
};

struct Atlas {
    uint16_t w, h;
    std::unique_ptr<uint32_t[]> pixels;
    size_t id;
    std::vector<Sprite> sprites;
    std::atomic<size_t> spritesToBeRendered;
    std::unordered_map<float, Spritesheet> spritesheets;
};

void loadSprite(const std::string& filePath, Sprite& sprite, XXH64_hash_t& hash) {
    FILE* input = std::fopen(filePath.c_str(), "rb");

    sprite.dataSize = std::filesystem::file_size(filePath);

    sprite.data = std::make_unique_for_overwrite<char[]>(sprite.dataSize);
    char* spriteData = sprite.data.get();
    std::fread(spriteData, sprite.dataSize, 1, input);
    std::fclose(input);

    hash = XXH64(spriteData, sprite.dataSize, 0) ^ XXH64(filePath.data(), filePath.size(), 0);
}

void parseSprite(
    const std::string& filePath,
    Sprite& sprite,
    resvg_options* opt,
    const BuilderOptions& opts
) {
    auto err = (resvg_error) resvg_parse_tree_from_data(sprite.data.get(), sprite.dataSize, opt, &sprite.tree);
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
    sprite.data.release();

    resvg_size size = resvg_get_image_size(sprite.tree);
    int width = std::ceil(size.width), height = std::ceil(size.height);
    if (width > opts.maxAtlasSize || height > opts.maxAtlasSize) {
        throw SpritesheetBuilderException(std::format(
            "{}: Maximum atlas size exceeded: width {}, height {}, maximum {}",
            filePath, width, height, opts.maxAtlasSize
        ));
    }

    sprite.name = std::filesystem::path(filePath).stem().string() + opts.extension;
    sprite.width = width;
    sprite.height = height;

    if (opts.allowTrimming) {
        resvg_rect bbox;
        resvg_get_image_bbox(sprite.tree, &bbox);
        float bx = std::floor(bbox.x);
        float by = std::floor(bbox.y);
        float bWidth = std::min((float) width, std::ceil(bbox.width));
        float bHeight = std::min((float) height, std::ceil(bbox.height));
        if (bx > 0 || by > 0 || bWidth < (float) width || bHeight < (float) height) {
            sprite.trimmed = true;
            sprite.bboxX = bx;
            sprite.bboxY = by;
            width = (uint16_t) bWidth;
            height = (uint16_t) bHeight;
        }
    }

    sprite.rect = rectpack2D::rect_xywh(-1, -1, width + opts.padding, height + opts.padding); // -1 means rect hasn't been packed yet
}

template<typename T>
int16_t floor(T val, float scale) {
    return std::floor((float) val * scale);
}

template<typename T>
int16_t ceil(T val, float scale) {
    return std::ceil((float) val * scale);
}

std::deque<Atlas> packAtlases(
    std::vector<Sprite>& sprites,
    int& spriteMaxW,
    int& spriteMaxH,
    const BuilderOptions& opts
) {
    std::deque<Atlas> atlases;

    constexpr auto insertCallback = [](auto&) {
        return rectpack2D::callback_result::CONTINUE_PACKING;
    };
    auto finderInput = rectpack2D::make_finder_input(
        opts.maxAtlasSize + opts.padding,
        1, // discard_step (packing quality)
        insertCallback,
        insertCallback,
        opts.allowRotation
            ? rectpack2D::flipping_option::ENABLED
            : rectpack2D::flipping_option::DISABLED
    );

    size_t atlasIdx = 0;
    while (!sprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(sprites, finderInput);

        uint16_t binWidth = (uint16_t) std::ceil(result.w) - opts.padding;
        uint16_t binHeight = (uint16_t) std::ceil(result.h) - opts.padding;
        if (opts.square) {
            binWidth = binHeight = std::max(binWidth, binHeight);
        }
        if (opts.powerOfTwo) {
            binWidth = std::bit_ceil(binWidth);
            binHeight = std::bit_ceil(binHeight);
        }
        Atlas& atlas = atlases.emplace_back(
            binWidth,
            binHeight,
            std::make_unique_for_overwrite<uint32_t[]>(binWidth * binHeight),
            ++atlasIdx
        );

        for (float scale : opts.resolutions) {
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

            rect.w -= opts.padding;
            rect.h -= opts.padding;

            int width = rect.w, height = rect.h;
            for (auto& [scale, spritesheet] : atlas.spritesheets) {
                int16_t spriteWidth = ceil(rect.flipped ? height : width, scale);
                int16_t spriteHeight = ceil(rect.flipped ? width : height, scale);
                spritesheet.frames[sprite.name] = {
                    .frame = {
                        .x = floor(rect.x, scale),
                        .y = floor(rect.y, scale),
                        .w = spriteWidth,
                        .h = spriteHeight,
                    },
                    .sourceSize = {
                        .w = ceil(sprite.width, scale),
                        .h = ceil(sprite.height, scale),
                    },
                    .spriteSourceSize = {
                        .x = floor(sprite.bboxX, scale),
                        .y = floor(sprite.bboxY, scale),
                        .w = spriteWidth,
                        .h = spriteHeight,
                    },
                    .rotated = rect.flipped,
                    .trimmed = sprite.trimmed,
                };
            }

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

static uint8_t unpremulTable[256][256];

void initUnpremulTable() {
    for (int a = 0; a < 256; ++a) {
        for (int c = 0; c < 256; ++c) {
            unpremulTable[a][c] = a == 0 ? 0 : (c * 255 + a / 2) / a;
        }
    }
}

void renderSprite(
    const Sprite& sprite,
    const std::unique_ptr<uint32_t[]>& spriteBuffer,
    Atlas& atlas,
    bool argb
) {
    const Rect& rect = sprite.rect;
    int width = rect.w, height = rect.h;

    uint32_t* spriteData = spriteBuffer.get();
    std::memset(spriteData, 0, width * height * 4);

    // The actual rendering happens here
    // We render to a temporary sprite buffer to clip the SVG to its viewBox
    resvg_transform transform;
    if (rect.flipped) {
        transform = {
            0,  1,
            -1, 0,
            (float) width - sprite.bboxY,
            -sprite.bboxX,
        };
    } else {
        transform = {
            1, 0,
            0, 1,
            -sprite.bboxX,
            -sprite.bboxY,
        };
    }
    resvg_render(
        sprite.tree,
        transform,
        width,
        height,
        reinterpret_cast<char*>(spriteData)
    );
    resvg_tree_destroy(sprite.tree);

    // This pair of loops has three functions:
    // 1. Copy data from the sprite buffer to the atlas buffer
    // 2. Un-premultiply the premultiplied alpha outputted by resvg to avoid artifacts
    // 3. Convert RGBA -> ARGB if only outputting WebP to avoid a second conversion later
    uint16_t atlasWidth = atlas.w;
    uint32_t* atlasData = atlas.pixels.get() + rect.x + rect.y * atlasWidth;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t pixel = spriteData[x];
            uint8_t a = pixel >> 24;
            if (a == 0) {
                atlasData[x] = 0;
            } else if (a == 255) {
                if (argb) {
                    uint32_t rb = pixel & 0x00ff00ff;
                    uint32_t g  = pixel & 0x0000ff00;
                    rb = rb >> 16 | rb << 16; // swap R and B
                    atlasData[x] = 255 << 24 | rb | g;
                } else {
                    atlasData[x] = pixel;
                }
            } else {
                uint8_t r = unpremulTable[a][pixel       & 0xff];
                uint8_t g = unpremulTable[a][pixel >> 8  & 0xff];
                uint8_t b = unpremulTable[a][pixel >> 16 & 0xff];
                if (argb) {
                    atlasData[x] = a << 24 | r << 16 | g << 8 | b;
                } else {
                    atlasData[x] = a << 24 | b << 16 | g << 8 | r;
                }
            }
        }
        atlasData += atlasWidth;
        spriteData += width;
    }
}

void encodeWebp(const Atlas& atlas, FILE* file, const BuilderOptions& opts, bool argb) {
    WebPConfig config;
    WebPConfigInit(&config);
    config.lossless = true;
    config.thread_level = opts.multithreaded;
    switch (opts.speed) {
    case EncoderSpeed::Slow:
        config.method = 6;
        config.quality = 100;
        break;
    case EncoderSpeed::Medium:
        config.method = 3;
        config.quality = 75;
        break;
    case EncoderSpeed::Fast:
        config.method = 0;
        config.quality = 0;
        break;
    }

    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        throw SpritesheetBuilderException("WebP picture init failed");
    }
    picture.width = atlas.w;
    picture.height = atlas.h;
    picture.use_argb = 1;

    if (argb) {
        picture.argb = atlas.pixels.get();
        picture.argb_stride = atlas.w;
    } else if (WebPPictureImportRGBA(
        &picture,
        reinterpret_cast<const uint8_t*>(atlas.pixels.get()),
        atlas.w * 4
    ) == 0) {
        throw SpritesheetBuilderException("WebP data import failed");
    }

    picture.custom_ptr = file;
    picture.writer = [](const uint8_t* data, size_t size, const WebPPicture* picture) -> int {
        auto* file = static_cast<FILE*>(picture->custom_ptr);
        std::fwrite(data, size, 1, file);
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
    image.init(reinterpret_cast<const uint8_t*>(atlas.pixels.get()), atlas.w, atlas.h, 4);
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

    std::fwrite(data, size, 1, file);

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

    int code = spng_encode_image(encoder, atlas.pixels.get(), atlas.w * atlas.h * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if (code != 0) {
        throw SpritesheetBuilderException(std::format("PNG encode failed: {}", spng_strerror(code)));
    }

    spng_ctx_free(encoder);
}

template<class Fn>
void iterateTextureFormats(const BuilderOptions& opts, XXH64_hash_t hash, Fn&& callback) {
    for (TextureFormat format : opts.formats) {
        std::string extension;
        switch (format) {
        case TextureFormat::Ktx2Etc1s:
        case TextureFormat::Ktx2Uastc:
            extension = "ktx2";
            break;
        case TextureFormat::Webp:
            extension = "webp";
            break;
        case TextureFormat::Png:
            extension = "png";
            break;
        }
        for (float scale : opts.resolutions) {
            std::string basePath = std::filesystem::path(opts.outputDirectory) / std::format(
                "{}@{}x-{:x}",
                opts.atlasName,
                scale,
                hash
            );
            callback(format, scale, basePath, extension);
        }
    }
}

void encodeAtlas(
    Atlas& atlas,
    ThreadPool& threadPool,
    const std::set<std::pair<TextureFormat, float>>& cachedFormats,
    const BuilderOptions& opts,
    XXH64_hash_t hash,
    bool argb,
    bool multithreading
) {
    iterateTextureFormats(opts, hash, [&](TextureFormat format, float scale, const std::string& basePath, const std::string& extension) {
        if (cachedFormats.contains({format, scale})) return;

        std::string filePath = std::format("{}-{}.{}", basePath, atlas.id, extension);

        Spritesheet& spritesheet = atlas.spritesheets[scale];
        spritesheet.meta.image = filePath;
        std::string jsonPath = filePath + ".json";
        if (glz::error_ctx error = glz::write_file_json(spritesheet, jsonPath, std::string{})) {
            throw SpritesheetBuilderException(jsonPath + ": Failed to write JSON: " + glz::format_error(error));
        }

        threadPool.queueJob([&atlas, &opts, format, scale, filePath, argb, multithreading] {
            FILE* file = std::fopen(filePath.c_str(), "wb");
            if (file == nullptr) {
                throw SpritesheetBuilderException("Unable to write to file: " + filePath);
            }
            std::setvbuf(file, nullptr, _IOFBF, 1'000'000); // 1 MB buffer
            switch (format) {
            case TextureFormat::Ktx2Etc1s:
                encodeKtx2(atlas, file, basist::basis_tex_format::cETC1S, multithreading);
                break;
            case TextureFormat::Ktx2Uastc:
                encodeKtx2(atlas, file, basist::basis_tex_format::cUASTC4x4, multithreading);
                break;
            case TextureFormat::Webp:
                encodeWebp(atlas, file, opts, argb);
                break;
            case TextureFormat::Png:
                encodePng(atlas, file);
                break;
            }
            std::fclose(file);
        });
    });
}

namespace spritesheetc {

std::vector<std::string> buildSpritesheets(const std::vector<std::string>& inputFiles, const BuilderOptions& opts) {
    bool log = opts.logStatus;
    Timer total{log};

    if (!std::filesystem::is_directory(opts.outputDirectory)) {
        throw SpritesheetBuilderException("Output path does not exist or is not a directory: " + opts.outputDirectory);
    }

    // Create the thread pool
    uint16_t numThreads = opts.multithreaded
        ? std::max(1u, std::thread::hardware_concurrency())
        : 1;
    ThreadPool threadPool{numThreads};

    std::vector<std::string> outputFiles;

    // Phase 1: Load sprites, create a hash from all SVG files and paths
    Timer load{log};
    size_t numInputFiles = inputFiles.size();
    std::vector<Sprite> sprites{numInputFiles};
    auto hashes = std::make_unique_for_overwrite<XXH64_hash_t[]>(numInputFiles);
    threadPool.reset(numInputFiles);
    for (size_t i = 0; i < numInputFiles; ++i) {
        threadPool.queueJob([&, i] {
            loadSprite(inputFiles[i], sprites[i], hashes.get()[i]);
        });
    }
    threadPool.waitForJobs();
    XXH64_hash_t hash = XXH64(hashes.get(), numInputFiles * sizeof(XXH64_hash_t), 0);
    load.stop(std::format("[spritesheetc] {} sprites loaded", numInputFiles));

    // Check cache, exit if atlases already exist with current hash
    std::set<std::pair<TextureFormat, float>> cachedFormats;
    if (opts.cache) {
        bool allExist = true;
        iterateTextureFormats(opts, hash, [&](TextureFormat format, float scale, const std::string& basePath, const std::string& extension) {
            std::ifstream cacheFile{std::format("{}.{}.cache", basePath, extension)};
            if (!cacheFile.good()) {
                allExist = false;
                return;
            }

            std::string file;
            while (std::getline(cacheFile, file)) {
                outputFiles.emplace_back(file);
            }
            cachedFormats.emplace(format, scale);
        });
        if (allExist) {
            if (opts.logStatus) std::cout << "[spritesheetc] Cache hit! Nothing to do, exiting.\n";
            return outputFiles;
        }
    }

    // Remove old atlases from the output directory if it exceeds the maximum size
    size_t outputDirSize = 0;
    std::map<std::filesystem::file_time_type, std::filesystem::directory_entry> files;
    std::string hashStr = std::format("{:x}", hash);
    for (const std::filesystem::directory_entry& path : std::filesystem::directory_iterator(opts.outputDirectory)) {
        std::string filename = path.path().filename();
        if (filename.contains(hashStr)) continue; // skip files with current hash
        outputDirSize += path.file_size();
        if (!filename.ends_with(".cache")) continue;
        files.emplace(path.last_write_time(), path);
    }
    if (outputDirSize > opts.maxOutputDirSize) {
        for (const std::filesystem::directory_entry& cacheEntry : files | std::views::values) {
            std::ifstream cacheFile{cacheEntry.path()};
            std::string file;
            while (std::getline(cacheFile, file)) {
                std::filesystem::remove(file);
                std::filesystem::remove(file + ".json");
            }
            std::filesystem::remove(cacheEntry);
        }
    }

    // Phase 2: Parse sprites
    Timer parse{log};
    resvg_options* resvgOpts = resvg_options_create();
    threadPool.reset(numInputFiles);
    for (size_t i = 0; i < numInputFiles; ++i) {
        threadPool.queueJob([&, i] {
            parseSprite(inputFiles[i], sprites[i], resvgOpts, opts);
        });
    }
    threadPool.waitForJobs();
    resvg_options_destroy(resvgOpts);
    parse.stop(std::format("[spritesheetc] {} sprites parsed", numInputFiles));

    // Phase 3: Pack sprites into atlases
    Timer pack{log};
    int spriteMaxW = 0, spriteMaxH = 0;
    std::deque<Atlas> atlases = packAtlases(sprites, spriteMaxW, spriteMaxH, opts);
    pack.stop(std::format("[spritesheetc] {} atlases packed", atlases.size()));

    // Phase 4: Render sprites to atlases + encode atlases
    Timer render{log};
    bool argb = opts.formats.size() == 1 && opts.formats.contains(TextureFormat::Webp);
    size_t numEncodeJobs = atlases.size() * opts.formats.size() * opts.resolutions.size();
    bool encoderMultithreaded = opts.multithreaded && numEncodeJobs < numThreads;
    initUnpremulTable();
    threadPool.reset(numInputFiles + numEncodeJobs);
    for (Atlas& atlas : atlases) {
        for (const Sprite& sprite : atlas.sprites) {
            threadPool.queueJob([&] {
                thread_local auto spriteBuffer = std::make_unique_for_overwrite<uint32_t[]>(spriteMaxW * spriteMaxH);
                renderSprite(sprite, spriteBuffer, atlas, argb);

                // Once all an atlas's sprites have been rendered, we move on to encoding
                if (atlas.spritesToBeRendered.fetch_sub(1, std::memory_order_acq_rel) > 1) return;

                encodeAtlas(
                    atlas,
                    threadPool,
                    cachedFormats,
                    opts,
                    hash,
                    argb,
                    encoderMultithreaded
                );
            });
        }
    }
    threadPool.waitForJobs();
    render.stop(std::format("[spritesheetc] {} atlases written", atlases.size()));

    // Populate outputFiles and create cache file,
    // which is done even if cache is false, because the cache file is also used when deleting old files
    iterateTextureFormats(opts, hash, [&](TextureFormat, float, const std::string& basePath, const std::string& extension) {
        std::ofstream cacheFile{std::format("{}.{}.cache", basePath, extension)};
        for (size_t i = 1; i <= atlases.size(); ++i) {
            std::string imagePath = std::format("{}-{}.{}", basePath, i, extension);
            outputFiles.emplace_back(imagePath);
            cacheFile << imagePath << "\n";
        }
    });

    total.stop("[spritesheetc] Done. Total time");
    return outputFiles;
}

void readDirectory(const std::filesystem::path& path, std::vector<std::string>& files) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (entry.is_directory()) continue;

        std::filesystem::path filePath = entry.path();
        if (filePath.extension() != ".svg") continue;

        files.emplace_back(filePath);
    }
}

std::vector<std::string> buildSpritesheetsFromDirectories(const std::vector<std::string>& inputDirectories, const BuilderOptions& opts) {
    std::vector<std::string> files;
    for (const std::string& pathStr : inputDirectories) {
        readDirectory(pathStr, files);
    }
    return buildSpritesheets(files, opts);
}

std::vector<std::string> buildSpritesheetsFromFileList(const std::string& inputFileList, const BuilderOptions& opts) {
    std::vector<std::string> files;
    std::ifstream inputFile{inputFileList};
    std::string pathStr;
    while (std::getline(inputFile, pathStr)) {
        if (pathStr.starts_with("#")) continue; // allow comments

        std::filesystem::path path{pathStr};
        if (path.extension() != ".svg") {
            throw SpritesheetBuilderException("Non-SVG file specified in file list: " + pathStr);
        }
        if (std::filesystem::is_directory(path)) {
            readDirectory(path, files);
            continue;
        }

        files.emplace_back(pathStr);
    }
    return buildSpritesheets(files, opts);
}

}
