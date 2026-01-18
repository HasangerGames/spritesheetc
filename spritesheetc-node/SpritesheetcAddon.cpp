#include <format>

#include <napi.h>
#include "spritesheetc.h"

#include "SpritesheetcAddon.h"

Napi::Value typeError(Napi::Env env, const std::string& msg) {
    Napi::TypeError::New(env, msg)
        .ThrowAsJavaScriptException();
    return env.Null();
}

Napi::Value SpritesheetcAddon::buildSpritesheets(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() != 2) {
        return typeError(env, std::format("Wrong number of arguments: expected 2, got {}", info.Length()));
    }

    if (!info[0].IsArray()) {
        return typeError(env, "First argument (inputFiles) must be an array");
    }
    auto jsInputFiles = info[0].As<Napi::Array>();
    std::vector<std::string> inputFiles;
    inputFiles.reserve(jsInputFiles.Length());
    for (uint32_t i = 0; i < jsInputFiles.Length(); ++i) {
        Napi::Value rawValue = jsInputFiles.Get(i);
        if (!rawValue.IsString()) {
            return typeError(env, "Input files must be strings");
        }
        inputFiles.emplace_back(rawValue.As<Napi::String>().Utf8Value());
    }

    if (!info[1].IsObject()) {
        return typeError(env, "Second argument (opts) must be an object");
    }
    auto jsOpts = info[1].As<Napi::Object>();
    spritesheetc::BuilderOptions opts;

    Napi::Value atlasName = jsOpts.Get("atlasName");
    if (!atlasName.IsString()) {
        return typeError(env, "atlasName must be a string");
    }
    opts.atlasName = atlasName.As<Napi::String>().Utf8Value();

    std::vector<std::string> result = spritesheetc::buildSpritesheetsFromDirectories(inputFiles, opts);
    Napi::Array jsResult = Napi::Array::New(env, result.size());
    for (size_t i = 0; i < result.size(); ++i) {
        jsResult.Set(i, Napi::String::New(env, result[i]));
    }
    return jsResult;
}

Napi::Value SpritesheetcAddon::buildSpritesheetsFromDirectories(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), "Testing testing");
}

Napi::Value SpritesheetcAddon::buildSpritesheetsFromFileList(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), "Testing testing");
}


