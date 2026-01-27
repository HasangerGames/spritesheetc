#include <format>

#include <napi.h>
#include "spritesheetc.h"

#include "SpritesheetcAddon.h"

using namespace spritesheetc;

bool typeError(Napi::Env env, const std::string& msg) {
    Napi::TypeError::New(env, msg)
        .ThrowAsJavaScriptException();
    return false;
}

bool checkArgumentCount(const Napi::CallbackInfo& info) {
    if (info.Length() < 1 || info.Length() > 2) {
        return typeError(info.Env(), std::format("Wrong number of arguments: expected 1-2, got {}", info.Length()));
    }
    return true;
}

bool parseInputs(const Napi::CallbackInfo& info, std::vector<std::string>& inputs, const std::string& inputsName) {
    Napi::Env env = info.Env();

    if (!info[0].IsArray()) {
        return typeError(env, inputsName + " (first argument) must be an array");
    }
    auto jsInputs = info[0].As<Napi::Array>();
    inputs.reserve(jsInputs.Length());
    for (uint32_t i = 0; i < jsInputs.Length(); ++i) {
        Napi::Value rawValue = jsInputs.Get(i);
        if (!rawValue.IsString()) {
            return typeError(env, inputsName + " (first argument) must contain only strings");
        }
        inputs.emplace_back(rawValue.As<Napi::String>().Utf8Value());
    }

    return true;
}

bool parseOpts(const Napi::CallbackInfo& info, BuilderOptions& opts) {
    if (info.Length() == 1) return true;
    Napi::Env env = info.Env();

    if (!info[1].IsObject()) {
        return typeError(env, "opts (second argument) must be an object");
    }
    auto jsOpts = info[1].As<Napi::Object>();

    auto getString = [&](const std::string& propName, std::string& dest) {
        if (!jsOpts.Has(propName)) return;
        Napi::Value value = jsOpts.Get(propName);
        if (!value.IsString()) {
            typeError(env, propName + " must be a string");
            throw std::runtime_error("Type error");
        }
        dest = value.As<Napi::String>();
    };

    auto getBoolean = [&](const std::string& propName, bool& dest) {
        if (!jsOpts.Has(propName)) return;
        Napi::Value value = jsOpts.Get(propName);
        if (!value.IsBoolean()) {
            typeError(env, propName + " must be a boolean");
            throw std::runtime_error("Type error");
        }
        dest = value.As<Napi::Boolean>().Value();
    };

    auto getNumber = [&](const std::string& propName, auto& dest) {
        if (!jsOpts.Has(propName)) return;
        Napi::Value value = jsOpts.Get(propName);
        if (!value.IsNumber()) {
            typeError(env, propName + " must be a number");
            throw std::runtime_error("Type error");
        }
        dest = value.As<Napi::Number>().Int64Value();
    };

    try {
        getString("atlasName", opts.atlasName);
        getString("outputDirectory", opts.outputDirectory);
        getBoolean("cache", opts.cache);
        getNumber("maxOutputDirSize", opts.maxOutputDirSize);
        getBoolean("logStatus", opts.logStatus);

        Napi::Value rawFormats = jsOpts.Get("formats");
        if (!rawFormats.IsArray()) {
            return typeError(env, "formats must be an array");
        }
        auto jsFormats = rawFormats.As<Napi::Array>();
        opts.formats = {};
        for (uint32_t i = 0; i < jsFormats.Length(); ++i) {
            Napi::Value rawFormat = jsFormats.Get(i);
            if (!rawFormat.IsString()) {
                return typeError(env, "formats must contain only strings");
            }
            std::string format = rawFormat.As<Napi::String>().Utf8Value();
            if (format == "ktx2") {
                opts.formats.emplace(TextureFormat::Ktx2);
            } else if (format == "webp") {
                opts.formats.emplace(TextureFormat::Webp);
            } else if (format == "png") {
                opts.formats.emplace(TextureFormat::Png);
            } else {
                return typeError(env, "Unsupported format: " + format + ". Expected one of: ktx2, webp, png");
            }
        }

        Napi::Value rawResolutions = jsOpts.Get("resolutions");
        if (!rawResolutions.IsArray()) {
            return typeError(env, "resolutions must be an array");
        }
        auto jsResolutions = rawResolutions.As<Napi::Array>();
        opts.resolutions = {};
        for (uint32_t i = 0; i < jsResolutions.Length(); ++i) {
            Napi::Value rawResolution = jsResolutions.Get(i);
            if (!rawResolution.IsNumber()) {
                return typeError(env, "resolutions must contain only numbers");
            }
            opts.resolutions.emplace(rawResolution.As<Napi::Number>().FloatValue());
        }

        getNumber("maxAtlasSize", opts.maxAtlasSize);
        getBoolean("powerOfTwo", opts.powerOfTwo);
        getBoolean("square", opts.square);
        getBoolean("fixedSize", opts.fixedSize);
        getNumber("padding", opts.padding);
        getBoolean("allowRotation", opts.allowRotation);
        getBoolean("allowTrimming", opts.allowTrimming);
        getString("extension", opts.extension);
        getBoolean("multithreaded", opts.multithreaded);

        std::string speed;
        getString("speed", speed);
        if (speed == "slow") {
            opts.speed = EncoderSpeed::Slow;
        } else if (speed == "medium") {
            opts.speed = EncoderSpeed::Medium;
        } else if (speed == "fast") {
            opts.speed = EncoderSpeed::Fast;
        } else {
            return typeError(env, "Unsupported speed: " + speed + ". Expected one of: slow, medium, fast");
        }
    } catch (std::runtime_error&) {
        return false;
    }

    return true;
}

Napi::Array vectorToArray(Napi::Env env, const std::vector<std::string>& vector) {
    Napi::Array array = Napi::Array::New(env, vector.size());
    for (size_t i = 0; i < vector.size(); ++i) {
        array.Set(i, Napi::String::New(env, vector[i]));
    }
    return array;
}

Napi::Value SpritesheetcAddon::buildSpritesheets(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!checkArgumentCount(info)) {
        return env.Null();
    }

    std::vector<std::string> inputFiles;
    if (!parseInputs(info, inputFiles, "inputFiles")) {
        return env.Null();
    }

    BuilderOptions opts;
    if (!parseOpts(info, opts)) {
        return env.Null();
    }

    return vectorToArray(env, spritesheetc::buildSpritesheets(inputFiles, opts));
}

Napi::Value SpritesheetcAddon::buildSpritesheetsFromDirectories(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!checkArgumentCount(info)) {
        return env.Null();
    }

    std::vector<std::string> inputDirectories;
    if (!parseInputs(info, inputDirectories, "inputDirectories")) {
        return env.Null();
    }

    BuilderOptions opts;
    if (!parseOpts(info, opts)) {
        return env.Null();
    }

    return vectorToArray(env, spritesheetc::buildSpritesheetsFromDirectories(inputDirectories, opts));
}

Napi::Value SpritesheetcAddon::buildSpritesheetsFromFileList(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!checkArgumentCount(info)) {
        return env.Null();
    }

    if (!info[0].IsString()) {
        typeError(env, "inputFileList (first argument) must be a string");
        return env.Null();
    }
    std::string inputFileList = info[0].As<Napi::String>();

    BuilderOptions opts;
    if (!parseOpts(info, opts)) {
        return env.Null();
    }

    return vectorToArray(env, spritesheetc::buildSpritesheetsFromFileList(inputFileList, opts));
}


