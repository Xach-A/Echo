// Converts application icons (from executable paths) into D3D11 textures
// for display in the Applications view.
#pragma once

#include <string>
#include <unordered_map>

#include "imgui.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace echo::ui {

class TextureCache {
public:
    void setDevice(ID3D11Device* d) { device_ = d; }
    // Returns 0 if no icon could be loaded.
    ImTextureID iconForExe(const std::wstring& exePath);
    void clear();
    ~TextureCache() { clear(); }

private:
    ID3D11Device* device_ = nullptr;
    std::unordered_map<std::wstring, ID3D11ShaderResourceView*> cache_;
};

} // namespace echo::ui
