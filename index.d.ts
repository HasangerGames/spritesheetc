export interface BuilderOptions {
    /**
     * Paths to inputs.
     * Each must be a .svg file,
     * a directory containing .svg files,
     * or a .txt file containing a newline-separated list of said files or directories,
     * where lines starting with # are ignored.
     */
    inputs: string[];

    /** Folder to output the generated atlases to. Default "output". */
    outputDir?: string;

    /** Name of the output atlases. Default "atlas". */
    atlasName?: string;

    /**
     * List of formats to output the generated atlases in.
     * "ktx2" is KTX2 UASTC supercompressed with Zstandard.
     * "webp" is lossless WebP.
     * "png" is standard PNG.
     * Default "webp".
     */
    formats?: ("ktx2" | "webp" | "png")[];

    /**
     * List of resolutions to output the generated atlases in.
     * Values range from 0.25-1. For example, 0.5 is half resolution.
     * Default 1.
     */
    resolutions?: number[];

    /**
     * Controls the speed of the encoder.
     * "slow" is the slowest but produces the smallest files.
     * "medium" is between Slow and Fast.
     * "fast" is the fastest but produces the largest files.
     * Default "medium".
     */
    speed?: "slow" | "medium" | "fast";

    /** Maximum allowed size of each atlas texture. Cannot be greater than 16384. Default 4096. */
    maxAtlasSize?: number;

    /** Ensures atlas dimensions are powers of two. Default false. */
    powerOfTwo?: boolean;

    /** Ensures atlas width and height are equal. Default false. */
    square?: boolean;

    /** Forces all atlases to maxAtlasSize. Default false. */
    fixedSize?: boolean;

    /**
     * Padding to add around each sprite, in pixels.
     * This prevents pixels from leaking between sprites.
     * Default 2.
     */
    padding?: number;

    /** Allows sprites to be rotated 90 degrees to save atlas space. Default true. */
    allowRotation?: boolean;

    /** Removes transparent pixels from the edges of sprites to save atlas space. Default true. */
    allowTrimming?: boolean;

    /**
     * Controls the extension added to the name of each sprite.
     * If set to an empty string (""), the extension will be removed.
     * For example, given an input file input/foo.svg, if extension is "", the sprite will be named "foo".
     * If extension is ".img", the sprite will be named "foo.img".
     * Default empty string.
     */
    extension?: string;

    /**
     * Maximum size of the output directory, in bytes.
     * If the total size of the atlases in the output directory exceeds this size,
     * older atlases will be deleted automatically.
     * Default 500'000'000 (500 MB).
     */
    maxOutputDirSize?: number;

    /**
     * Checks if any changes have been made to the inputs since the last run of the spritesheet builder
     * (i.e. if atlases containing all the inputs exist in the output directory).
     * This check is based on the combined hashes of the inputs.
     * If no changes are detected, the builder will exit without building anything.
     * Default true.
     */
    cache?: boolean;

    /** Logs the status of the spritesheet builder as it builds. Default true. */
    logStatus?: boolean;

    /** Enables multithreading. Default true. */
    multithreaded?: boolean;
}

/**
 * Builds a collection of spritesheets.
 * @param opts Options for the spritesheet builder
 * @return Paths to the outputted images. To access the metadata associated with an image, simply add ".json" to the path
 */
export function buildSpritesheets(opts: BuilderOptions): string[];
