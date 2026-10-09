#include "ui/TextureCache.h"

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <vector>

namespace echo::ui {

ImTextureID TextureCache::iconForExe(const std::wstring& exePath) {
    if (!device_ || exePath.empty()) return ImTextureID{};
    auto it = cache_.find(exePath);
    if (it != cache_.end()) return reinterpret_cast<ImTextureID>(it->second);
    ID3D11ShaderResourceView* srv = nullptr;
    HICON large = nullptr;
    if (ExtractIconExW(exePath.c_str(), 0, &large, nullptr, 1) == 1 && large) {
        ICONINFO ii{};
        if (GetIconInfo(large, &ii)) {
            BITMAP bm{};
            GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
            const int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
            if (w > 0 && h > 0 && w <= 256 && h <= 256) {
                std::vector<uint32_t> px(static_cast<size_t>(w) * h);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                HDC dc = GetDC(nullptr);
                bool ok = ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
                bool anyAlpha = false;
                for (uint32_t p : px) anyAlpha |= (p >> 24) != 0;
                if (ok && !anyAlpha) {
                    // Old-style icon: use the mask for transparency.
                    std::vector<uint32_t> mask(static_cast<size_t>(w) * h);
                    if (GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS) == h)
                        for (size_t i = 0; i < px.size(); ++i) px[i] |= (mask[i] & 0xFFFFFF) ? 0 : 0xFF000000u;
                }
                ReleaseDC(nullptr, dc);
                if (ok) {
                    // BGRA -> RGBA
                    for (uint32_t& p : px) p = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = w;
                    desc.Height = h;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA data{px.data(), static_cast<UINT>(w * 4), 0};
                    ID3D11Texture2D* tex = nullptr;
                    if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &tex))) {
                        device_->CreateShaderResourceView(tex, nullptr, &srv);
                        tex->Release();
                    }
                }
            }
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
        DestroyIcon(large);
    }
    cache_[exePath] = srv; // cache failures too (nullptr)
    return reinterpret_cast<ImTextureID>(srv);
}

void TextureCache::clear() {
    for (auto& [k, v] : cache_)
        if (v) v->Release();
    cache_.clear();
}

} // namespace echo::ui
