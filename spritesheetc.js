#!/usr/bin/env node
const { buildSpritesheets } = require('.');
const { Command, Option } = require('commander');
const { version } = require('./package.json');

const program = new Command();
program
    .name('spritesheetc')
    .description('High-performance spritesheet generator')
    .version(version)
    .requiredOption('-i, --inputs <files...>', 'inputs (REQUIRED): .svg files, directories containing .svg files, or .txt lists of files/directories (newline-separated, lines starting with # ignored)')
    .option('-o, --output-dir <directory>', 'directory to output atlases to', 'output')
    .option('-n, --atlas-name <name>', 'name of output atlases', 'atlas')
    .addOption(new Option('-f, --formats <formats...>', 'output texture formats')
        .choices(['ktx2', 'webp', 'png']).default(['webp']))
    .addOption(new Option('-r, --resolutions <resolutions...>', 'output resolutions, must be between 0.25-1')
        .argParser((res, previous) => previous.concat([parseFloat(res)])).default([], '[1]'))
    .addOption(new Option('-s, --speed <speed>', 'encoding speed: slow means smaller files, fast means larger files')
        .choices(['slow', 'medium', 'fast']).default('medium'))
    .option('-a, --max-atlas-size <size>', 'max size of each atlas texture, cannot be greater than 16384', 4096)
    .option('-t, --power-of-two', 'ensures atlas dimensions are powers of two', false)
    .option('-u, --square', 'ensures atlas width and height are equal', false)
    .option('-x, --fixed-size', 'forces all atlases to max-atlas-size', false)
    .addOption(new Option('-p, --padding <pixels>', 'adds padding around sprites to prevent pixels leaking')
        .argParser(n => parseInt(n)).default(2))
    .option('-l, --no-allow-rotation', 'disables rotating sprites 90 degrees to save atlas space')
    .option('-g, --no-allow-trimming', 'disables removing transparent pixels from the edges of sprites to save atlas space')
    .option('-e, --extension <extension>', 'extension to add to sprite names', '')
    .addOption(new Option('-z, --max-output-dir-size <size>', 'max size of output directory in bytes, deleting old atlases if exceeded (default: 500,000,000 = 500 MB)')
        .argParser(parseInt))
    .option('-c, --no-cache', 'disables cache')
    .option('-q, --no-log-status', 'disables logging')
    .option('-m, --no-multithreaded', 'disables multithreading')
    .parse();

const opts = program.opts();
if (!opts.resolutions.length) opts.resolutions = [1];
buildSpritesheets(opts);
