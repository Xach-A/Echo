// Installing VB-CABLE on request: the pinned official download and unpacking
// it with Windows' own tar (the real download is checked by
// `echo_hwtest vbcable-download`).
#include "doctest/doctest.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/VbCable.h"

using namespace echo;

namespace {

uint32_t crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}

// A zip with one stored (uncompressed) file.
std::vector<uint8_t> storedZip(const std::string& name, const std::string& data) {
    std::vector<uint8_t> z;
    const uint32_t crc = crc32(data), n = static_cast<uint32_t>(data.size());
    put32(z, 0x04034b50);
    put16(z, 10), put16(z, 0), put16(z, 0), put16(z, 0), put16(z, 0x21);
    put32(z, crc), put32(z, n), put32(z, n);
    put16(z, static_cast<uint32_t>(name.size())), put16(z, 0);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), data.begin(), data.end());
    const uint32_t central = static_cast<uint32_t>(z.size());
    put32(z, 0x02014b50);
    put16(z, 20), put16(z, 10), put16(z, 0), put16(z, 0), put16(z, 0), put16(z, 0x21);
    put32(z, crc), put32(z, n), put32(z, n);
    put16(z, static_cast<uint32_t>(name.size())), put16(z, 0), put16(z, 0), put16(z, 0), put16(z, 0);
    put32(z, 0), put32(z, 0);
    z.insert(z.end(), name.begin(), name.end());
    const uint32_t size = static_cast<uint32_t>(z.size()) - central;
    put32(z, 0x06054b50);
    put16(z, 0), put16(z, 0), put16(z, 1), put16(z, 1);
    put32(z, size), put32(z, central), put16(z, 0);
    return z;
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    FILE* f = _wfopen(p.wstring().c_str(), L"wb");
    REQUIRE(f);
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

} // namespace

TEST_CASE("vb-cable: the official pack from vb-audio.com is pinned by size and SHA-256") {
    CHECK(std::string(win::kVbCableUrl).rfind("https://download.vb-audio.com/", 0) == 0);
    CHECK(std::string(win::kVbCableSha256).size() == 64);
    CHECK(win::kVbCableBytes == 1318877u);
    CHECK(std::wstring(win::kVbCablePage) == L"https://vb-audio.com/Cable/");
}

TEST_CASE("vb-cable: a download is unpacked with Windows' tar, and a broken one is refused") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "echo-test-unzip";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    writeFile(dir / "pack.zip", storedZip("readme.txt", "VB-CABLE test\r\n"));
    std::string err;
    REQUIRE_MESSAGE(win::unzip((dir / "pack.zip").wstring(), (dir / "out").wstring(), &err), err);
    FILE* f = _wfopen((dir / "out" / "readme.txt").wstring().c_str(), L"rb");
    REQUIRE(f);
    char buf[64] = {};
    const size_t got = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    CHECK(std::string(buf, got) == "VB-CABLE test\r\n");
    // Not a zip.
    writeFile(dir / "bad.zip", {'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'});
    CHECK_FALSE(win::unzip((dir / "bad.zip").wstring(), (dir / "bad").wstring(), &err));
    CHECK_FALSE(err.empty());
    fs::remove_all(dir, ec);
}
