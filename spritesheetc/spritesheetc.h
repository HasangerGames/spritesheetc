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
    Ktx2Etc1s,
    Ktx2Uastc,
    Webp,
    Png,
};

enum class EncoderSpeed {
    Slow,
    Medium,
    Fast,
};

struct BuilderOptions {
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
    bool powerOfTwo = false;

    /** Ensures atlas width and height are equal. Default false. */
    bool square = false;

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

    /** Enables multithreading. Default true. */
    bool multithreaded = true;

    /**
     * Controls the speed of the encoder.
     * Slow is the slowest but produces the smallest files.
     * Medium is between Slow and Fast.
     * Fast is the fastest but produces the largest files.
     * Default Medium.
     */
    EncoderSpeed speed = EncoderSpeed::Medium;
};

/**
 * Builds a collection of spritesheets.
 * @param inputFiles Paths to the input files. Each must be in SVG format
 * @param opts Options for the spritesheet builder
 * @return Paths to the outputted images. To access the metadata, simply add ".json" to the path
 */
std::vector<std::string> buildSpritesheets(const std::vector<std::string>& inputFiles, const BuilderOptions& opts = {});

std::vector<std::string> buildSpritesheetsFromDirectories(const std::vector<std::string>& inputDirectories, const BuilderOptions& opts = {});

std::vector<std::string> buildSpritesheetsFromFileList(const std::string& inputFileList, const BuilderOptions& opts = {});

}
