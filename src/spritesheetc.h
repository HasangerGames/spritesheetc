#pragma once

#include <chrono>
#include <iostream>
#include <set>
#include <string>

#include "resvg.h"

#include "rectpack2D/empty_spaces.h"
#include "rectpack2D/finders_interface.h"

/**
 * Simple timer for profiling code.
 */
class Timer {
public:
    Timer() : Timer("") { }
    explicit Timer(std::string name) :
        m_name(std::move(name)), m_start(std::chrono::steady_clock::now()) { }

    void stop() {
        stop(m_name);
    }

    void stop(const std::string& name, bool enableLogging = true, double threshold = 0) {
        if (m_stopped) return;
        m_stopped = true;

        if (!enableLogging) return;

        auto elapsed = std::chrono::steady_clock::now() - m_start;
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        if (ms < threshold) return;
        std::cout << std::format("{}: {:.3f} ms\n", name, ms);
    }
private:
    std::string m_name;
    std::chrono::steady_clock::time_point m_start;
    bool m_stopped = false;
};

namespace fs = std::filesystem;

using SpacesType = rectpack2D::empty_spaces<false>;
using Rect = rectpack2D::output_rect_t<SpacesType>;
using Buffer = std::vector<char>;

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

struct Sprite {
    std::string name;
    Rect rect;
    resvg_render_tree* tree;

    auto& get_rect() { return rect; }
    const auto& get_rect() const { return rect; }
};

struct Atlas {
    uint16_t w, h;
    uint16_t spriteMaxW = 0, spriteMaxH = 0;
    std::vector<Sprite> sprites;
    Spritesheet spritesheet;
};

namespace spritesheetc {

struct SpritesheetBuilderException final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TextureFormat {
    BasisuEtc1s,
    BasisuUastc,
    Webp,
    Png
};

struct SpritesheetBuilderConfig {
    /** List of files to add to the atlas(es). Must be in SVG format. */
    std::vector<std::string> inputFiles;

    /** Template for naming the output files. */
    std::string atlasName = "atlas-<hash>@<scale>.<ext>";

    /** Folder to output generated atlases to. Default "output". */
    std::string outputDirectory = "output";

    /**
     * Checks if any changes have been made to the inputFiles since the last run of the spritesheet builder.
     * This check is based on the last modified/accessed time of the files, as recorded in a JSON file in the output directory.
     * If no changes are detected, the builder will exit without building anything.
     * Default true.
     */
    bool cache = true;

    /** Logs the status of the spritesheet builder as it builds. Default true. */
    bool logStatus = true;

    /**
     * List of formats and scales to output the generated textures in.
     * Scale ranges from 0-1. For example, a scale of 0.5
     * Default BasisuUastc @ 1x.
     */
    std::set<TextureFormat> formats = {TextureFormat::BasisuUastc};
    // std::unordered_map<TextureFormat, std::set<float>> formats = {{TextureFormat::BasisuUastc, {1.0f}}};

    /** Maximum allowed size of each atlas texture. Default 4096. */
    uint16_t maxAtlasSize = 4096;

    /** Ensures atlas dimensions are powers of two. Default true. */
    bool pot = true;

    /** Ensures atlases are square. Default true. */
    bool square = true;

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

    /** Removes transparent pixels from the edges of sprites to save atlas space. Default true. */
    bool edgeDetection = true;

    /** Allows sprites to be flipped to save atlas space. Default true. */
    bool allowFlipping = true;

    /**
     * Number of threads to use when parsing and rasterizing SVGs.
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
