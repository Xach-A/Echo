#include "core/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace echo {

namespace {
void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::wstring& path, int sampleRate, int channels, int bitDepth) {
    close();
    f_ = _wfopen(path.c_str(), L"wb");
    if (!f_) return false;
    sampleRate_ = sampleRate;
    channels_ = channels;
    bits_ = (bitDepth == 16 || bitDepth == 24 || bitDepth == 32) ? bitDepth : 24;
    dataBytes_ = frames_ = 0;
    uint8_t hdr[44] = {};
    std::fwrite(hdr, 1, sizeof(hdr), f_); // placeholder, filled by updateHeader()
    updateHeader();
    return true;
}

void WavWriter::updateHeader() {
    if (!f_) return;
    uint8_t h[44];
    const uint16_t fmt = bits_ == 32 ? 3 : 1; // IEEE float : PCM
    const uint32_t blockAlign = static_cast<uint32_t>(channels_ * bits_ / 8);
    const uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(dataBytes_, 0xFFFFFFFFull - 36));
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, fmt);
    put16(h + 22, static_cast<uint16_t>(channels_));
    put32(h + 24, static_cast<uint32_t>(sampleRate_));
    put32(h + 28, static_cast<uint32_t>(sampleRate_) * blockAlign);
    put16(h + 32, static_cast<uint16_t>(blockAlign));
    put16(h + 34, static_cast<uint16_t>(bits_));
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    const long pos = std::ftell(f_);
    std::fseek(f_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), f_);
    std::fseek(f_, pos, SEEK_SET);
    std::fflush(f_);
}

bool WavWriter::write(const float* x, size_t frames) {
    if (!f_) return false;
    const size_t samples = frames * channels_;
    const size_t bps = bits_ / 8;
    buf_.resize(samples * bps);
    uint8_t* d = buf_.data();
    for (size_t i = 0; i < samples; ++i) {
        const float v = std::isfinite(x[i]) ? std::clamp(x[i], -1.0f, 1.0f) : 0.0f;
        if (bits_ == 16) {
            const int16_t s = static_cast<int16_t>(std::lrintf(v * 32767.0f));
            std::memcpy(d + i * 2, &s, 2);
        } else if (bits_ == 24) {
            const int32_t s = static_cast<int32_t>(std::lrint(v * 8388607.0));
            d[i * 3] = s & 0xFF;
            d[i * 3 + 1] = (s >> 8) & 0xFF;
            d[i * 3 + 2] = (s >> 16) & 0xFF;
        } else {
            std::memcpy(d + i * 4, &x[i], 4);
        }
    }
    const size_t w = std::fwrite(d, 1, buf_.size(), f_);
    dataBytes_ += w;
    frames_ += frames;
    return w == buf_.size();
}

void WavWriter::close() {
    if (!f_) return;
    updateHeader();
    std::fclose(f_);
    f_ = nullptr;
}

bool readWav(const std::wstring& path, WavData& out, std::string* error) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    std::vector<uint8_t> all;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) all.insert(all.end(), chunk, chunk + n);
    std::fclose(f);
    if (all.size() < 12 || std::memcmp(all.data(), "RIFF", 4) || std::memcmp(all.data() + 8, "WAVE", 4)) {
        if (error) *error = "not a RIFF/WAVE file";
        return false;
    }
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t pos = 12;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    while (pos + 8 <= all.size()) {
        const uint32_t len = get32(all.data() + pos + 4);
        const uint8_t* body = all.data() + pos + 8;
        if (!std::memcmp(all.data() + pos, "fmt ", 4) && len >= 16) {
            fmt = get16(body);
            ch = get16(body + 2);
            rate = static_cast<int>(get32(body + 4));
            bits = get16(body + 14);
            if (fmt == 0xFFFE && len >= 40) fmt = get16(body + 24); // extensible: subformat tag
        } else if (!std::memcmp(all.data() + pos, "data", 4)) {
            data = body;
            dataLen = std::min<size_t>(len, all.size() - (pos + 8));
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || ch <= 0) {
        if (error) *error = "missing fmt or data chunk";
        return false;
    }
    out.sampleRate = rate;
    out.channels = ch;
    const size_t bps = bits / 8;
    const size_t count = bps ? dataLen / bps : 0;
    out.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + i * bps;
        if (fmt == 3 && bits == 32) std::memcpy(&out.samples[i], p, 4);
        else if (bits == 16) out.samples[i] = static_cast<int16_t>(get16(p)) / 32768.0f;
        else if (bits == 24) {
            const int32_t v = (p[0] << 8) | (p[1] << 16) | (static_cast<int32_t>(p[2]) << 24);
            out.samples[i] = v / 2147483648.0f;
        } else if (bits == 32) out.samples[i] = static_cast<int32_t>(get32(p)) / 2147483648.0f;
        else if (bits == 8) out.samples[i] = (p[0] - 128) / 128.0f;
    }
    return true;
}

} // namespace echo
