#include "image_codec.h"
#include "font.h" // flateCompress

#include <zlib.h>

#include <cstring>
#include <vector>

namespace {

uint32_t be32(const std::string& b, size_t off) {
    if (off + 3 >= b.size()) return 0;
    return (static_cast<uint32_t>(static_cast<uint8_t>(b[off])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(b[off + 3]));
}

bool isPng(const std::string& b) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return b.size() > 8 && std::memcmp(b.data(), sig, 8) == 0;
}

bool isJpeg(const std::string& b) {
    return b.size() > 3 && static_cast<uint8_t>(b[0]) == 0xFF && static_cast<uint8_t>(b[1]) == 0xD8;
}

// ---- JPEG ------------------------------------------------------------------

// Walks the marker segments to the frame header. Only the size and component
// count are needed; the entropy-coded data is handed to PDF verbatim.
bool jpegInfo(const std::string& b, int& w, int& h, int& comps) {
    size_t i = 2;
    while (i + 3 < b.size()) {
        if (static_cast<uint8_t>(b[i]) != 0xFF) { ++i; continue; }
        uint8_t marker = static_cast<uint8_t>(b[i + 1]);
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
        if (i + 3 >= b.size()) return false;
        size_t len = (static_cast<size_t>(static_cast<uint8_t>(b[i + 2])) << 8) |
                     static_cast<uint8_t>(b[i + 3]);
        // SOF0/1/2/9/10 carry the frame geometry; SOF4/8/12 are not frames.
        bool isSof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 &&
                     marker != 0xCC;
        if (isSof) {
            if (i + 9 >= b.size()) return false;
            h = (static_cast<int>(static_cast<uint8_t>(b[i + 5])) << 8) |
                static_cast<uint8_t>(b[i + 6]);
            w = (static_cast<int>(static_cast<uint8_t>(b[i + 7])) << 8) |
                static_cast<uint8_t>(b[i + 8]);
            comps = static_cast<uint8_t>(b[i + 9]);
            return w > 0 && h > 0;
        }
        i += 2 + len;
    }
    return false;
}

// ---- PNG -------------------------------------------------------------------

struct PngInfo {
    int width = 0, height = 0;
    int bitDepth = 0, colorType = 0, interlace = 0;
    std::string idat;
    std::string palette;
    std::string trns;
};

bool pngChunks(const std::string& b, PngInfo& info) {
    size_t i = 8;
    bool sawHeader = false;
    while (i + 8 <= b.size()) {
        uint32_t len = be32(b, i);
        if (i + 12 + len > b.size()) break;
        std::string type = b.substr(i + 4, 4);
        size_t dataOff = i + 8;
        if (type == "IHDR" && len >= 13) {
            info.width = static_cast<int>(be32(b, dataOff));
            info.height = static_cast<int>(be32(b, dataOff + 4));
            info.bitDepth = static_cast<uint8_t>(b[dataOff + 8]);
            info.colorType = static_cast<uint8_t>(b[dataOff + 9]);
            info.interlace = static_cast<uint8_t>(b[dataOff + 12]);
            sawHeader = true;
        } else if (type == "PLTE") {
            info.palette = b.substr(dataOff, len);
        } else if (type == "tRNS") {
            info.trns = b.substr(dataOff, len);
        } else if (type == "IDAT") {
            info.idat += b.substr(dataOff, len);
        } else if (type == "IEND") {
            break;
        }
        i += 12 + len;
    }
    return sawHeader && info.width > 0 && info.height > 0 && !info.idat.empty();
}

std::string inflateAll(const std::string& in, size_t expected) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    std::string out;
    out.resize(expected ? expected : in.size() * 4 + 1024);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    size_t written = 0;
    int rc = Z_OK;
    while (true) {
        if (written == out.size()) out.resize(out.size() * 2);
        zs.next_out = reinterpret_cast<Bytef*>(&out[written]);
        zs.avail_out = static_cast<uInt>(out.size() - written);
        rc = inflate(&zs, Z_NO_FLUSH);
        written = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) break;
        if (zs.avail_in == 0 && zs.avail_out != 0) break;
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && written == 0) return {};
    out.resize(written);
    return out;
}

int paethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Reverses the per-scanline PNG filters in place, producing raw samples.
bool unfilter(const std::string& raw, int width, int height, int bytesPerPixel,
              size_t rowBytes, std::vector<uint8_t>& out) {
    size_t stride = rowBytes + 1; // each row is prefixed by its filter type
    if (raw.size() < stride * static_cast<size_t>(height)) return false;
    out.assign(static_cast<size_t>(height) * rowBytes, 0);

    for (int y = 0; y < height; ++y) {
        uint8_t filter = static_cast<uint8_t>(raw[static_cast<size_t>(y) * stride]);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(raw.data()) + y * stride + 1;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * rowBytes;
        const uint8_t* prev = y > 0 ? out.data() + static_cast<size_t>(y - 1) * rowBytes : nullptr;

        for (size_t x = 0; x < rowBytes; ++x) {
            int a = x >= static_cast<size_t>(bytesPerPixel) ? dst[x - bytesPerPixel] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= static_cast<size_t>(bytesPerPixel)) ? prev[x - bytesPerPixel] : 0;
            int v = src[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paethPredictor(a, b, c); break;
                default: return false;
            }
            dst[x] = static_cast<uint8_t>(v & 0xFF);
        }
    }
    (void)width;
    return true;
}

bool decodePng(const std::string& bytes, EncodedImage& out) {
    PngInfo info;
    if (!pngChunks(bytes, info)) return false;
    // Interlaced and 16-bit PNGs are rare enough in documents that supporting
    // them is not worth the extra de-interlacing pass; skipping the picture is
    // better than embedding a scrambled one.
    if (info.interlace != 0) return false;
    if (info.bitDepth != 8) return false;

    int channels;
    switch (info.colorType) {
        case 0: channels = 1; break; // gray
        case 2: channels = 3; break; // RGB
        case 3: channels = 1; break; // palette index
        case 4: channels = 2; break; // gray + alpha
        case 6: channels = 4; break; // RGBA
        default: return false;
    }
    size_t rowBytes = static_cast<size_t>(info.width) * static_cast<size_t>(channels);
    std::string raw = inflateAll(info.idat, (rowBytes + 1) * static_cast<size_t>(info.height));
    std::vector<uint8_t> px;
    if (!unfilter(raw, info.width, info.height, channels, rowBytes, px)) return false;

    size_t pixels = static_cast<size_t>(info.width) * static_cast<size_t>(info.height);
    std::string colorData;
    std::string alpha;

    if (info.colorType == 3) {
        if (info.palette.size() < 3) return false;
        colorData.resize(pixels * 3);
        size_t entries = info.palette.size() / 3;
        for (size_t i = 0; i < pixels; ++i) {
            size_t idx = px[i];
            if (idx >= entries) idx = 0;
            colorData[i * 3 + 0] = info.palette[idx * 3 + 0];
            colorData[i * 3 + 1] = info.palette[idx * 3 + 1];
            colorData[i * 3 + 2] = info.palette[idx * 3 + 2];
        }
        if (!info.trns.empty()) {
            alpha.resize(pixels);
            for (size_t i = 0; i < pixels; ++i) {
                size_t idx = px[i];
                alpha[i] = idx < info.trns.size() ? info.trns[idx] : static_cast<char>(0xFF);
            }
        }
        out.colorSpace = "DeviceRGB";
    } else if (info.colorType == 0) {
        colorData.assign(reinterpret_cast<char*>(px.data()), pixels);
        out.colorSpace = "DeviceGray";
    } else if (info.colorType == 2) {
        colorData.assign(reinterpret_cast<char*>(px.data()), pixels * 3);
        out.colorSpace = "DeviceRGB";
    } else if (info.colorType == 4) {
        colorData.resize(pixels);
        alpha.resize(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            colorData[i] = static_cast<char>(px[i * 2]);
            alpha[i] = static_cast<char>(px[i * 2 + 1]);
        }
        out.colorSpace = "DeviceGray";
    } else { // 6: RGBA
        colorData.resize(pixels * 3);
        alpha.resize(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            colorData[i * 3 + 0] = static_cast<char>(px[i * 4 + 0]);
            colorData[i * 3 + 1] = static_cast<char>(px[i * 4 + 1]);
            colorData[i * 3 + 2] = static_cast<char>(px[i * 4 + 2]);
            alpha[i] = static_cast<char>(px[i * 4 + 3]);
        }
        out.colorSpace = "DeviceRGB";
    }

    out.data = flateCompress(colorData);
    if (out.data.empty()) return false;
    // Transparency becomes a soft mask so the picture composites over
    // whatever is behind it instead of getting a black box.
    if (!alpha.empty()) {
        bool anyTransparent = false;
        for (char c : alpha) {
            if (static_cast<uint8_t>(c) != 0xFF) { anyTransparent = true; break; }
        }
        if (anyTransparent) out.smask = flateCompress(alpha);
    }
    out.filter = "FlateDecode";
    out.width = info.width;
    out.height = info.height;
    return true;
}

} // namespace

bool probeImageSize(const std::string& bytes, const std::string& mime, int& w, int& h) {
    (void)mime;
    if (isPng(bytes)) {
        PngInfo info;
        if (!pngChunks(bytes, info)) return false;
        w = info.width;
        h = info.height;
        return true;
    }
    if (isJpeg(bytes)) {
        int comps = 0;
        return jpegInfo(bytes, w, h, comps);
    }
    return false;
}

bool encodeImageForPdf(const Image& img, EncodedImage& out) {
    if (img.bytes.empty()) return false;

    if (isJpeg(img.bytes)) {
        int w = 0, h = 0, comps = 0;
        if (!jpegInfo(img.bytes, w, h, comps)) return false;
        // CMYK JPEGs need an /Decode inversion dance that varies by producer;
        // rather than guess wrong, leave them out.
        if (comps != 1 && comps != 3) return false;
        out.data = img.bytes;
        out.filter = "DCTDecode";
        out.colorSpace = comps == 1 ? "DeviceGray" : "DeviceRGB";
        out.width = w;
        out.height = h;
        return true;
    }
    if (isPng(img.bytes)) return decodePng(img.bytes, out);
    return false;
}
