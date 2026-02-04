#include <format>

#include <napi.h>
#include "spritesheetc.h"

#include "SpritesheetcAddon.h"

using namespace spritesheetc;

Napi::Value SpritesheetcAddon::buildSpritesheets(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    struct TypeError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    try {
        if (info.Length() != 1) {
            throw TypeError(std::format("Wrong number of arguments: expected 1, got {}", info.Length()));
        }

        if (!info[0].IsObject()) {
            throw TypeError("Argument must be an object");
        }
        auto jsOpts = info[0].As<Napi::Object>();
        BuilderOptions opts;

        auto getString = [&](const std::string& propName, std::string& dest) {
            if (!jsOpts.Has(propName)) return;
            Napi::Value value = jsOpts.Get(propName);
            if (!value.IsString()) {
                throw TypeError(propName + " must be a string");
            }
            dest = value.As<Napi::String>();
        };

        auto getBoolean = [&](const std::string& propName, bool& dest) {
            if (!jsOpts.Has(propName)) return;
            Napi::Value value = jsOpts.Get(propName);
            if (!value.IsBoolean()) {
                throw TypeError(propName + " must be a boolean");
            }
            dest = value.As<Napi::Boolean>().Value();
        };

        auto getNumber = [&](const std::string& propName, auto& dest) {
            if (!jsOpts.Has(propName)) return;
            Napi::Value value = jsOpts.Get(propName);
            if (!value.IsNumber()) {
                throw TypeError(propName + " must be a number");
            }
            dest = value.As<Napi::Number>().Int64Value();
        };
        
        if (!jsOpts.Has("inputs")) {
            throw TypeError("inputs not specified");
        }
        Napi::Value rawInputs = jsOpts.Get("inputs");
        if (!rawInputs.IsArray()) {
            throw TypeError("inputs must be an array");
        }
        auto jsInputs = rawInputs.As<Napi::Array>();
        opts.inputs.reserve(jsInputs.Length());
        for (uint32_t i = 0; i < jsInputs.Length(); ++i) {
            Napi::Value rawInput = jsInputs.Get(i);
            if (!rawInput.IsString()) {
                throw TypeError("inputs must contain only strings");
            }
            opts.inputs.emplace_back(rawInput.As<Napi::String>());
        }

        getString("outputDir", opts.outputDir);
        getString("atlasName", opts.atlasName);

        if (jsOpts.Has("formats")) {
            Napi::Value rawFormats = jsOpts.Get("formats");
            if (!rawFormats.IsArray()) {
                throw TypeError("formats must be an array");
            }
            auto jsFormats = rawFormats.As<Napi::Array>();
            opts.formats = {};
            for (uint32_t i = 0; i < jsFormats.Length(); ++i) {
                Napi::Value rawFormat = jsFormats.Get(i);
                if (!rawFormat.IsString()) {
                    throw TypeError("formats must contain only strings");
                }
                std::string format = rawFormat.As<Napi::String>().Utf8Value();
                if (format == "ktx2") {
                    opts.formats.emplace(TextureFormat::Ktx2);
                } else if (format == "webp") {
                    opts.formats.emplace(TextureFormat::Webp);
                } else if (format == "png") {
                    opts.formats.emplace(TextureFormat::Png);
                } else {
                    throw TypeError("Unsupported format: " + format + ". Expected one of: ktx2, webp, png");
                }
            }
        }

        if (jsOpts.Has("resolutions")) {
            Napi::Value rawResolutions = jsOpts.Get("resolutions");
            if (!rawResolutions.IsArray()) {
                throw TypeError("resolutions must be an array");
            }
            auto jsResolutions = rawResolutions.As<Napi::Array>();
            opts.resolutions = {};
            for (uint32_t i = 0; i < jsResolutions.Length(); ++i) {
                Napi::Value rawResolution = jsResolutions.Get(i);
                if (!rawResolution.IsNumber()) {
                    throw TypeError("resolutions must contain only numbers");
                }
                opts.resolutions.emplace(rawResolution.As<Napi::Number>().FloatValue());
            }
        }

        std::string speed;
        getString("speed", speed);
        if (speed == "slow") {
            opts.speed = EncoderSpeed::Slow;
        } else if (speed == "medium") {
            opts.speed = EncoderSpeed::Medium;
        } else if (speed == "fast") {
            opts.speed = EncoderSpeed::Fast;
        } else if (!speed.empty()) {
            throw TypeError("Unsupported speed: " + speed + ". Expected one of: slow, medium, fast");
        }

        getNumber("maxAtlasSize", opts.maxAtlasSize);
        getBoolean("powerOfTwo", opts.powerOfTwo);
        getBoolean("square", opts.square);
        getBoolean("fixedSize", opts.fixedSize);
        getNumber("padding", opts.padding);
        getBoolean("allowRotation", opts.allowRotation);
        getBoolean("allowTrimming", opts.allowTrimming);
        getString("extension", opts.extension);
        getNumber("maxOutputDirSize", opts.maxOutputDirSize);
        getBoolean("cache", opts.cache);
        getBoolean("logStatus", opts.logStatus);
        getBoolean("multithreaded", opts.multithreaded);

        std::vector<std::string> outputs = spritesheetc::buildSpritesheets(opts);
        Napi::Array jsOutputs = Napi::Array::New(env, outputs.size());
        for (size_t i = 0; i < outputs.size(); ++i) {
            jsOutputs.Set(i, Napi::String::New(env, outputs[i]));
        }
        return jsOutputs;
    } catch (const TypeError& e) {
        Napi::TypeError::New(env, e.what()).ThrowAsJavaScriptException();
    } catch (const std::runtime_error& e) {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    }

    return env.Null();
}
