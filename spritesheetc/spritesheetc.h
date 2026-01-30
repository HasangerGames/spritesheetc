#pragma once

#include <set>
#include <string>
#include <stdexcept>
#include <vector>
#include <cstdint>

namespace spritesheetc {

struct SpritesheetBuilderException final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TextureFormat {
    /** KTX2 UASTC supercompressed with Zstandard. */
    Ktx2,
    /** Lossless WebP. */
    Webp,
    /** Standard PNG. */
    Png,
};

enum class EncoderSpeed {
    /** Slowest, but produces the smallest files. */
    Slow,
    /** In between Slow and Fast. */
    Medium,
    /** Fastest, but produces the largest files. */
    Fast,
};

struct BuilderOptions {
    /**
     * Paths to inputs.
     * Each must be a .svg file,
     * a directory containing .svg files,
     * or a .txt file containing a newline-separated list of said files or directories,
     * where lines starting with # are ignored.
     */
    std::vector<std::string> inputs;

    /** Folder to output the generated atlases to. Default "output". */
    std::string outputDir = "output";

    /** Name of the output atlases. Default "atlas". */
    std::string atlasName = "atlas";

    /**
     * List of formats to output the generated atlases in.
     * Ktx2 is KTX2 UASTC supercompressed with Zstandard.
     * Webp is lossless WebP.
     * Png is standard PNG.
     * Default Webp.
     */
    std::set<TextureFormat> formats = {TextureFormat::Webp};

    /**
     * List of resolutions to output the generated atlases in.
     * Values range from 0.25-1. For example, 0.5 is half resolution.
     * Default 1.
     */
    std::set<float> resolutions = {1.0f};

    /**
     * Controls the speed of the encoder.
     * Slow is the slowest but produces the smallest files.
     * Medium is between Slow and Fast.
     * Fast is the fastest but produces the largest files.
     * Default Medium.
     */
    EncoderSpeed speed = EncoderSpeed::Medium;

    /** Maximum allowed size of each atlas texture. Cannot be greater than 16384. Default 4096. */
    uint16_t maxAtlasSize = 4096;

    /** Ensures atlas dimensions are powers of two. Default false. */
    bool powerOfTwo = false;

    /** Ensures atlas width and height are equal. Default false. */
    bool square = false;

    /** Forces all atlases to maxAtlasSize. Default false. */
    bool fixedSize = false;

    /**
     * Padding to add around each sprite, in pixels.
     * This prevents pixels from leaking between sprites.
     * Default 2.
     */
    uint8_t padding = 2;

    /** Allows sprites to be rotated 90 degrees to save atlas space. Default true. */
    bool allowRotation = true;

    /** Removes transparent pixels from the edges of sprites to save atlas space. Default true. */
    bool allowTrimming = true;

    /**
     * Controls the extension added to the name of each sprite.
     * If set to an empty string (""), the extension will be removed.
     * For example, given an input file input/foo.svg, if extension is "", the sprite will be named "foo".
     * If extension is ".img", the sprite will be named "foo.img".
     * Default empty string.
     */
    std::string extension;

    /**
     * Maximum size of the output directory, in bytes.
     * If the total size of the atlases in the output directory exceeds this size,
     * older atlases will be deleted automatically.
     * Default 500'000'000 (500 MB).
     */
    size_t maxOutputDirSize = 500'000'000;

    /**
     * Checks if any changes have been made to the inputs since the last run of the spritesheet builder
     * (i.e. if atlases containing all the inputs exist in the output directory).
     * This check is based on the combined hashes of the inputs.
     * If no changes are detected, the builder will exit without building anything.
     * Default true.
     */
    bool cache = true;

    /** Logs the status of the spritesheet builder as it builds. Default true. */
    bool logStatus = true;

    /** Enables multithreading. Default true. */
    bool multithreaded = true;
};

/**
 * Builds a collection of spritesheets.
 * @param opts Options for the spritesheet builder
 * @return Paths to the outputted images. To access the metadata associated with an image, simply add ".json" to the path
 */
std::vector<std::string> buildSpritesheets(const BuilderOptions& opts);

}
