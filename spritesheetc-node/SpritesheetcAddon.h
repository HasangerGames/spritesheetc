#pragma once

#include <napi.h>

class SpritesheetcAddon : public Napi::Addon<SpritesheetcAddon> {
public:
    SpritesheetcAddon(Napi::Env, Napi::Object exports) {
        DefineAddon(exports, {
            InstanceMethod("buildSpritesheets", &SpritesheetcAddon::buildSpritesheets),
        });
    }
private:
    Napi::Value buildSpritesheets(const Napi::CallbackInfo& info);
};

NODE_API_ADDON(SpritesheetcAddon)
