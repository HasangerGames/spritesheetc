#pragma once

#include <set>
#include <string>

namespace spritesheetc {

struct SpritesheetBuilderException final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TextureFormat {
    Ktx2Etc1s,
    Ktx2Uastc,
    Webp,
    Png
};

struct SpritesheetBuilderConfig {
    /** List of files to add to the atlas(es). Must be in SVG format. */
    std::vector<std::string> inputFiles;

    /** Name of the output atlases. Default "atlas". */
    std::string atlasName = "atlas";

    /** Folder to output the generated atlases to. Default "output". */
    std::string outputDirectory = "output";

    /**
     * Checks if any changes have been made to the inputFiles since the last run of the spritesheet builder
     * (i.e. if atlases containing the inputFiles exist in the output directory).
     * This check is based on the combined hashes of the inputFiles.
     * If no changes are detected, the builder will exit without building anything.
     * Default true.
     */
    bool cache = true;

    /**
     * Maximum size of the output directory, in bytes.
     * If the output directory exceeds this size, the oldest atlases will be deleted automatically.
     * Default 500'000 (500 MB).
     */
    size_t maxOutputDirSize = 500'000;

    /** Logs the status of the spritesheet builder as it builds. Default true. */
    bool logStatus = true;

    /** List of formats to output the generated atlases in. Default Webp. */
    std::set<TextureFormat> formats = {TextureFormat::Webp};

    /**
     * List of resolutions to output the generated atlases in.
     * Values range from 0-1. For example, 0.5 is half resolution.
     * Default 1.
     */
    std::set<float> resolutions = {1.0f};

    /** Maximum allowed size of each atlas texture. Default 4096. */
    uint16_t maxAtlasSize = 4096;

    /** Ensures atlas dimensions are powers of two. Default false. */
    bool pot = false;

    /** Ensures atlases are square. Default false. */
    bool square = false;

    /**
     * Packing value passed to rectpack2D.
     * 1 yields the best possible packing.
     * Larger values are faster but yield worse packing.
     * Negative values can theoretically yield better packing for small rectangles,
     * but 1 should be good enough for most use cases.
     * Default 1.
     */
    int packingQuality = 1;

    /**
     * Padding to add around each sprite, in pixels.
     * This prevents pixels from leaking between sprites.
     * Default 2.
     */
    uint8_t padding = 2;

    /** Allows sprites to be flipped to save atlas space. Default true. */
    bool allowRotation = true;

    /** Removes transparent pixels from the edges of sprites to save atlas space. Default true. */
    bool allowTrimming = true;

    /**
     * Number of threads the spritesheet builder should use.
     * The encoder may use more threads than this if encoderMultithreading is enabled,
     * which it is by default.
     * Passing 0 will create as many threads as the CPU supports.
     * Default 0.
     */
    uint16_t builderThreads = 0;

    /**
     * Enables multithreading for the encoder specifically.
     * The encoding libraries used provide no control over the number of threads,
     * so only a boolean toggle is given here.
     * Default true.
     */
    bool encoderMultithreading = true;

    /**
     * Encoding quality. 0 = lowest, 100 = highest.
     * For lossless formats, higher values increase encoding time but yield smaller file sizes.
     * Default 100.
     */
    uint8_t encoderQuality = 100;

    /**
     * Encoder method, used by WebP only. Ranges from 0-6.
     * 6 has the highest encoding time but the smallest file size.
     * 0 has the lowest encoding time but the largest file size.
     * Default 6.
     */
    uint8_t encoderMethod = 6;

    /** Enables lossless compression, used by WebP only. Default true. */
    bool encoderLossless = true;
};

void buildSpritesheets(const SpritesheetBuilderConfig& config);

std::vector<std::string> imagePathsFromDirectory(const std::string& path);

std::vector<std::string> imagePathsFromDirectories(const std::vector<std::string>& paths);

std::vector<std::string> imagePathsFromFileList(const std::string& path);

}
