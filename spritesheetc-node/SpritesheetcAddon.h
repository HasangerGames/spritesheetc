#pragma once

class SpritesheetcAddon : public Napi::Addon<SpritesheetcAddon> {
public:
    SpritesheetcAddon(Napi::Env env, Napi::Object exports) {
        DefineAddon(exports, {
            InstanceMethod("buildSpritesheets", &SpritesheetcAddon::buildSpritesheets),
            InstanceMethod("buildSpritesheetsFromDirectories", &SpritesheetcAddon::buildSpritesheetsFromDirectories),
            InstanceMethod("buildSpritesheetsFromFileList", &SpritesheetcAddon::buildSpritesheetsFromFileList),
        });
    }
private:
    Napi::Value buildSpritesheets(const Napi::CallbackInfo& info);
    Napi::Value buildSpritesheetsFromDirectories(const Napi::CallbackInfo& info);
    Napi::Value buildSpritesheetsFromFileList(const Napi::CallbackInfo& info);
};

NODE_API_ADDON(SpritesheetcAddon)
