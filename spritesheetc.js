#!/usr/bin/env node
const spritesheetc = require(".");

spritesheetc.buildSpritesheetsFromDirectories([
    "../Suroi/client/public/img/game/shared",
    "../Suroi/client/public/img/game/normal"
], {
    cache: false,
    maxOutputDirSize: 30_000_000,
    speed: "fast",
});
