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

    void queueJob(std::function<void()> job) {
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
        for (std::thread& thread : m_threads) thread.join();
    }
private:
    void threadLoop() {
        while (true) {
            std::function<void()> job;
            thread_local moodycamel::ConsumerToken token{m_queue};
            m_queue.wait_dequeue(token, job);
            if (job == nullptr) return; // null job = shutdown signal
            job();
            if (m_remainingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                m_remainingJobs.notify_one();
            }
        }
    }
    moodycamel::BlockingConcurrentQueue<std::function<void()>> m_queue;
    std::atomic<size_t> m_remainingJobs;
    std::vector<std::thread> m_threads;
};

class Timer {
public:
    explicit Timer(bool enable = true) : m_disabled(!enable) {
        if (!m_disabled) m_start = std::chrono::steady_clock::now();
    }

    void stop(const std::string& label, double threshold = 0) const {
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
    int16_t x, y;
    uint16_t w, h;
};

struct SpritesheetSize {
    uint16_t w, h;
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

struct SubAtlas {
    uint16_t w = 0, h = 0;
    std::unique_ptr<uint32_t[]> pixels;
    Spritesheet spritesheet;
};

struct Atlas {
    size_t id;
    std::vector<Sprite> sprites;
    std::unordered_map<float, SubAtlas> subAtlases;
    std::atomic<size_t> spritesToBeRendered;
};

void loadSprite(const std::string& filePath, Sprite& sprite, XXH64_hash_t& hash) {
    std::FILE* input = std::fopen(filePath.c_str(), "rb");

    sprite.dataSize = std::filesystem::file_size(filePath);

    sprite.data = std::make_unique_for_overwrite<char[]>(sprite.dataSize);
    char* spriteData = sprite.data.get();
    std::ignore = std::fread(spriteData, sprite.dataSize, 1, input);
    std::fclose(input);

    hash = XXH3_64bits(spriteData, sprite.dataSize) ^ XXH3_64bits(filePath.data(), filePath.size());
}

std::mutex g_out;

void parseSprite(
    const std::string& filePath,
    Sprite& sprite,
    resvg_options* opt,
    const BuilderOptions& opts
) {
    auto err = static_cast<resvg_error>(resvg_parse_tree_from_data(sprite.data.get(), sprite.dataSize, opt, &sprite.tree));
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
        float bWidth  = std::ceil(bbox.width - std::min(bbox.x, 0.0f));  // actual width of the SVG
        float bHeight = std::ceil(bbox.height - std::min(bbox.y, 0.0f)); // actual height of the SVG
        auto vWidth   = static_cast<float>(width);                       // width of the viewBox
        auto vHeight  = static_cast<float>(height);                      // height of the viewBox
        // If the actual size of the SVG is smaller than the viewBox, we can trim it to save atlas space
        if (
            std::floor(bbox.x) > 0
            || std::floor(bbox.y) > 0
            || bWidth < vWidth
            || bHeight < vHeight
        ) {
            sprite.trimmed = true;
            sprite.bboxX = bbox.x;
            sprite.bboxY = bbox.y;
            width = static_cast<uint16_t>(std::min(bWidth, vWidth));
            height = static_cast<uint16_t>(std::min(bHeight, vHeight));
        }
    }

    sprite.rect = rectpack2D::rect_xywh(-1, -1, width + opts.padding, height + opts.padding); // -1 means rect hasn't been packed yet
}

template<typename T>
int16_t floor(T val, float scale) {
    return std::floor(static_cast<float>(val) * scale);
}

template<typename T>
uint16_t ceil(T val, float scale) {
    return std::ceil(static_cast<float>(val) * scale);
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

    size_t atlasId = 0;
    while (!sprites.empty()) {
        rectpack2D::rect_wh result = rectpack2D::find_best_packing<SpacesType>(sprites, finderInput);

        uint16_t binWidth, binHeight;
        if (opts.fixedSize) {
            binWidth = binHeight = opts.maxAtlasSize;
        } else {
            binWidth = static_cast<uint16_t>(std::ceil(result.w)) - opts.padding;
            binHeight = static_cast<uint16_t>(std::ceil(result.h)) - opts.padding;
            if (opts.square) {
                binWidth = binHeight = std::max(binWidth, binHeight);
            }
            if (opts.powerOfTwo) {
                binWidth = std::bit_ceil(binWidth);
                binHeight = std::bit_ceil(binHeight);
            }
        }

        Atlas& atlas = atlases.emplace_back(++atlasId);

        for (float scale : opts.resolutions) {
            uint16_t scaledWidth = ceil(binWidth, scale);
            uint16_t scaledHeight = ceil(binHeight, scale);
            SubAtlas& subAtlas = atlas.subAtlases[scale] = {
                .w = scaledWidth,
                .h = scaledHeight,
                .pixels = std::make_unique_for_overwrite<uint32_t[]>(scaledWidth * scaledHeight),
            };
            subAtlas.spritesheet.meta.size = {
                .w = scaledWidth,
                .h = scaledHeight,
            };
            subAtlas.spritesheet.meta.scale = scale;
        }

        for (size_t i = 0; i < sprites.size(); ) {
            Sprite& sprite = sprites[i];
            Rect& rect = sprite.rect;
            if (rect.x == -1) { // -1 means sprite hasn't been packed yet
                ++i;
                continue;
            }

            int width = rect.w -= opts.padding;
            int height = rect.h -= opts.padding;

            for (auto& [scale, subAtlas] : atlas.subAtlases) {
                uint16_t spriteWidth = ceil(rect.flipped ? height : width, scale);
                uint16_t spriteHeight = ceil(rect.flipped ? width : height, scale);
                subAtlas.spritesheet.frames[sprite.name] = {
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
    SubAtlas& subAtlas,
    float scale,
    bool argb
) {
    const Rect& rect = sprite.rect;
    int width = ceil(rect.w, scale);
    int height = ceil(rect.h, scale);

    uint32_t* spriteData = spriteBuffer.get();
    std::memset(spriteData, 0, width * height * 4);

    // The actual rendering happens here
    // We render to a temporary sprite buffer to clip the SVG to its viewBox
    resvg_transform transform;
    if (rect.flipped) {
        transform = {
            0, scale,
            -scale, 0,
            static_cast<float>(width) + sprite.bboxY * scale,
            -sprite.bboxX * scale,
        };
    } else {
        transform = {
            scale, 0,
            0, scale,
            -sprite.bboxX * scale,
            -sprite.bboxY * scale,
        };
    }
    resvg_render(
        sprite.tree,
        transform,
        width,
        height,
        reinterpret_cast<char*>(spriteData)
    );

    // This pair of loops has three functions:
    // 1. Copy data from the sprite buffer to the atlas buffer
    // 2. Un-premultiply the premultiplied alpha outputted by resvg to avoid artifacts
    // 3. Convert RGBA -> ARGB if only outputting WebP to avoid a second conversion later
    uint16_t atlasWidth = subAtlas.w;
    uint32_t* atlasData = subAtlas.pixels.get()
        + floor(rect.x, scale)
        + floor(rect.y, scale) * atlasWidth;
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

void encodeWebp(SubAtlas& subAtlas, std::FILE* file, const BuilderOptions& opts, bool argb) {
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
            config.quality = 50;
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
    picture.width = subAtlas.w;
    picture.height = subAtlas.h;
    picture.use_argb = 1;

    if (argb) {
        picture.argb = subAtlas.pixels.get();
        picture.argb_stride = subAtlas.w;
    } else if (WebPPictureImportRGBA(
        &picture,
        reinterpret_cast<const uint8_t*>(subAtlas.pixels.get()),
        subAtlas.w * 4
    ) == 0) {
        throw SpritesheetBuilderException("WebP data import failed");
    }

    picture.custom_ptr = file;
    picture.writer = [](const uint8_t* data, size_t size, const WebPPicture* picture) -> int {
        auto* file = static_cast<std::FILE*>(picture->custom_ptr);
        std::fwrite(data, size, 1, file);
        return 1;
    };

    if (WebPEncode(&config, &picture) == 0) {
        throw SpritesheetBuilderException("WebP encode failed");
    }

    WebPPictureFree(&picture);
}

void encodeKtx2(SubAtlas& subAtlas, std::FILE* file, const BuilderOptions& opts, basisu::job_pool* pool) {
    if (!basisu::basisu_encoder_init()) {
        throw SpritesheetBuilderException("BasisU encoder init failed");
    }

    basisu::basis_compressor_params params;
    params.m_uastc = true;
    params.m_create_ktx2_file = true;
    params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
    params.m_perceptual = false;
    params.m_status_output = false;
    if (pool) {
        params.m_multithreading = true;
        params.m_pJob_pool = pool;
        params.m_pack_uastc_ldr_4x4_flags = basisu::cFlagThreaded;
    }
    switch (opts.speed) {
        case EncoderSpeed::Slow:
            params.m_ktx2_zstd_supercompression_level = 22;
            break;
        case EncoderSpeed::Medium:
            params.m_ktx2_zstd_supercompression_level = 6;
            break;
        case EncoderSpeed::Fast:
            params.m_ktx2_zstd_supercompression_level = 1;
            break;
    }

    basisu::image image;
    image.init(
        reinterpret_cast<const uint8_t*>(subAtlas.pixels.get()),
        subAtlas.w,
        subAtlas.h,
        4
    );
    params.m_source_images.push_back(image);

    basisu::basis_compressor compressor;
    if (!compressor.init(params)) {
        throw SpritesheetBuilderException("BasisU compressor init failed");
    }

    basisu::basis_compressor::error_code err = compressor.process();
    if (err != basisu::basis_compressor::error_code::cECSuccess) {
        throw SpritesheetBuilderException("BasisU compression failed");
    }

    const basisu::uint8_vec& output_ktx2 = compressor.get_output_ktx2_file();
    std::fwrite(output_ktx2.data(), 1, output_ktx2.size(), file);
}

void encodePng(SubAtlas& subAtlas, std::FILE* file) {
    spng_ctx* encoder = spng_ctx_new(SPNG_CTX_ENCODER);

    spng_ihdr ihdr = {
        .width = subAtlas.w,
        .height = subAtlas.h,
        .bit_depth = 8,
        .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA,
    };
    spng_set_ihdr(encoder, &ihdr);

    spng_set_png_file(encoder, file);

    int code = spng_encode_image(
        encoder,
        subAtlas.pixels.get(),
        subAtlas.w * subAtlas.h * 4,
        SPNG_FMT_PNG,
        SPNG_ENCODE_FINALIZE
    );
    if (code != 0) {
        throw SpritesheetBuilderException(std::format("PNG encode failed: {}", spng_strerror(code)));
    }

    spng_ctx_free(encoder);
}

std::string getExtension(TextureFormat format) {
    switch (format) {
        case TextureFormat::Ktx2:
            return "ktx2";
        case TextureFormat::Webp:
            return "webp";
        case TextureFormat::Png:
            return "png";
        default:
            std::unreachable();
    }
}

std::string getBasePath(const BuilderOptions& opts, float scale, XXH64_hash_t hash) {
    return (std::filesystem::path(opts.outputDir) / std::format(
        "{}@{}x-{:x}",
        opts.atlasName,
        scale,
        hash
    )).string();
}

void encodeAtlas(
    Atlas& atlas,
    ThreadPool& threadPool,
    const std::set<std::pair<TextureFormat, float>>& cachedFormats,
    const BuilderOptions& opts,
    XXH64_hash_t hash,
    bool argb,
    basisu::job_pool* basisPool
) {
    for (TextureFormat format : opts.formats) {
        std::string extension = getExtension(format);
        for (float scale : opts.resolutions) {
            if (cachedFormats.contains({format, scale})) return;

            std::string filePath = std::format(
                "{}-{}.{}",
                getBasePath(opts, scale, hash),
                atlas.id,
                extension
            );

            SubAtlas& subAtlas = atlas.subAtlases[scale];
            Spritesheet& spritesheet = subAtlas.spritesheet;
            spritesheet.meta.image = filePath;
            std::string jsonPath = filePath + ".json";
            if (glz::error_ctx error = glz::write_file_json(spritesheet, jsonPath, std::string{})) {
                throw SpritesheetBuilderException(jsonPath + ": Failed to write JSON: " + glz::format_error(error));
            }

            threadPool.queueJob([&subAtlas, &opts, format, filePath, argb, basisPool] {
                std::FILE* file = std::fopen(filePath.c_str(), "wb");
                if (file == nullptr) {
                    throw SpritesheetBuilderException("Unable to write to file: " + filePath);
                }
                std::setvbuf(file, nullptr, _IOFBF, 1'000'000); // 1 MB buffer
                switch (format) {
                    case TextureFormat::Ktx2:
                        encodeKtx2(subAtlas, file, opts, basisPool);
                        break;
                    case TextureFormat::Webp:
                        encodeWebp(subAtlas, file, opts, argb);
                        break;
                    case TextureFormat::Png:
                        encodePng(subAtlas, file);
                        break;
                }
                std::fclose(file);
            });
        }
    }
}

void readPath(const std::string& inputPath, std::vector<std::string>& files) {
    std::filesystem::path path{inputPath};
    std::string extension = path.extension().string();
    if (extension == ".svg") {
        files.emplace_back(inputPath);
    } else if (extension == ".txt") {
        std::ifstream inputFile{path};
        std::string filePath;
        while (std::getline(inputFile, filePath)) {
            // allow comments and blank lines
            if (
                filePath.starts_with("#")
                || filePath.empty()
                || std::ranges::all_of(filePath, isspace)
            ) continue;

            readPath(filePath, files);
        }
    } else if (extension.empty() && std::filesystem::is_directory(path)) {
        for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_directory()) continue;

            const std::filesystem::path& filePath = entry.path();
            if (filePath.extension() != ".svg") continue;

            files.emplace_back(filePath.string());
        }
    } else {
        throw SpritesheetBuilderException("Unsupported file type: " + inputPath);
    }
}

namespace spritesheetc {

std::vector<std::string> buildSpritesheets(const BuilderOptions& opts) {
    bool log = opts.logStatus;
    Timer total{log};

    if (opts.inputs.empty()) {
        throw SpritesheetBuilderException("No inputs specified");
    }

    std::vector<std::string> inputFiles;
    for (const std::string& input : opts.inputs) {
        readPath(input, inputFiles);
    }
    std::ranges::sort(inputFiles);

    if (
        !std::filesystem::is_directory(opts.outputDir)
        && !std::filesystem::create_directories(opts.outputDir)
    ) {
        throw SpritesheetBuilderException("Unable to create output directory: " + opts.outputDir);
    }

    if (opts.maxAtlasSize > 16384) {
        throw SpritesheetBuilderException(std::format("maxAtlasSize must be 16384 or less. Got {}", opts.maxAtlasSize));
    }

    for (float scale : opts.resolutions) {
        if (scale < 0.25 || scale > 1) {
            throw SpritesheetBuilderException(std::format("Invalid resolution: {}. Must be between 0.25 and 1", scale));
        }
    }

    // Create the thread pool
    uint16_t numThreads = opts.multithreaded
        ? std::max(1u, std::thread::hardware_concurrency())
        : 1;
    ThreadPool threadPool{numThreads};

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
    XXH64_hash_t hash = XXH3_64bits(hashes.get(), numInputFiles * sizeof(XXH64_hash_t));
    load.stop(std::format("[spritesheetc] {} sprites loaded", numInputFiles));

    // Check cache, exit if atlases already exist with current hash
    std::vector<std::string> outputFiles;
    std::set<std::pair<TextureFormat, float>> cachedFormats;
    if (opts.cache) {
        bool allExist = true;
        for (TextureFormat format : opts.formats) {
            std::string extension = getExtension(format);
            for (float scale : opts.resolutions) {
                std::ifstream cacheFile{std::format("{}.{}.cache", getBasePath(opts, scale, hash), extension)};
                if (!cacheFile.good()) {
                    allExist = false;
                    continue;
                }

                std::string file;
                while (std::getline(cacheFile, file)) {
                    outputFiles.emplace_back(file);
                }
                cachedFormats.emplace(format, scale);
            }
        }
        if (allExist) {
            if (opts.logStatus) std::cout << "[spritesheetc] Cache hit! Nothing to do, exiting.\n";
            return outputFiles;
        }
    }

    // Remove old atlases from the output directory if it exceeds the maximum size
    size_t outputDirSize = 0;
    std::map<std::filesystem::file_time_type, std::filesystem::directory_entry> files;
    std::string hashStr = std::format("{:x}", hash);
    for (const std::filesystem::directory_entry& path : std::filesystem::directory_iterator(opts.outputDir)) {
        std::string filename = path.path().filename().string();
        if (filename.contains(hashStr)) continue; // skip files with current hash
        if (path.is_directory()) continue;
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
    std::unique_ptr<basisu::job_pool> basisPool;
    if (opts.formats.contains(TextureFormat::Ktx2)) {
        basisPool = std::make_unique<basisu::job_pool>(numThreads);
    }
    initUnpremulTable();
    threadPool.reset(numInputFiles + numEncodeJobs);
    for (Atlas& atlas : atlases) {
        for (const Sprite& sprite : atlas.sprites) {
            threadPool.queueJob([&] {
                thread_local auto spriteBuffer = std::make_unique_for_overwrite<uint32_t[]>(spriteMaxW * spriteMaxH);
                for (float scale : opts.resolutions) {
                    renderSprite(sprite, spriteBuffer, atlas.subAtlases[scale], scale, argb);
                }
                resvg_tree_destroy(sprite.tree);

                // Once all an atlas's sprites have been rendered, we move on to encoding
                if (atlas.spritesToBeRendered.fetch_sub(1, std::memory_order_acq_rel) > 1) return;

                encodeAtlas(
                    atlas,
                    threadPool,
                    cachedFormats,
                    opts,
                    hash,
                    argb,
                    basisPool.get()
                );
            });
        }
    }
    threadPool.waitForJobs();
    render.stop(std::format("[spritesheetc] {} atlases written", atlases.size()));

    // Populate outputFiles and create cache file,
    // which is done even if cache is false, because the cache file is also used when deleting old files
    for (TextureFormat format : opts.formats) {
        std::string extension = getExtension(format);
        for (float scale : opts.resolutions) {
            std::string basePath = getBasePath(opts, scale, hash);
            std::ofstream cacheFile{std::format("{}.{}.cache", basePath, extension)};
            for (size_t i = 1; i <= atlases.size(); ++i) {
                std::string imagePath = std::format("{}-{}.{}", basePath, i, extension);
                outputFiles.emplace_back(imagePath);
                cacheFile << imagePath << "\n";
            }
        }
    }

    total.stop("[spritesheetc] Done. Total time");
    return outputFiles;
}

}
