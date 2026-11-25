// Назначение: конвертация текстур Source 1 (.vtf, включая DXT1/DXT5) и подготовка экспорта Source 2 (.vtex).
#pragma once

#include "VfCore.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(VF_HAS_STB_IMAGE_WRITE)
#include "stb_image_write.h"
#endif

namespace VfTexForge {

namespace fs = std::filesystem;

#pragma pack(push, 1)
struct VtfHeader {
    char signature[4];
    uint32_t version[2];
    uint32_t headerSize;
    uint16_t width;
    uint16_t height;
    uint32_t flags;
    uint16_t frames;
    uint16_t firstFrame;
    uint8_t pad0[4];
    float reflectivity[3];
    uint8_t pad1[4];
    float bumpmapScale;
    uint32_t highResFormat;
    uint8_t mipmapCount;
    uint32_t lowResFormat;
    uint8_t lowResWidth;
    uint8_t lowResHeight;
};
#pragma pack(pop)

enum FormatId : uint32_t {
    FMT_RGBA8888 = 0,
    FMT_ABGR8888 = 1,
    FMT_RGB888   = 2,
    FMT_BGR888   = 3,
    FMT_BGRA8888 = 12,
    FMT_DXT1     = 13,
    FMT_DXT5     = 15
};

class DxtDecoder {
public:
    static void decodeDxt1Block(const uint8_t* block, uint8_t rgbaOut[16 * 4]) {
        uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
        uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
        uint32_t bits = static_cast<uint32_t>(block[4]) |
                        (static_cast<uint32_t>(block[5]) << 8) |
                        (static_cast<uint32_t>(block[6]) << 16) |
                        (static_cast<uint32_t>(block[7]) << 24);

        uint8_t pal[4][4];
        rgb565ToRgb(c0, pal[0][0], pal[0][1], pal[0][2]); pal[0][3] = 255;
        rgb565ToRgb(c1, pal[1][0], pal[1][1], pal[1][2]); pal[1][3] = 255;

        if (c0 > c1) {
            for (int ch = 0; ch < 3; ++ch) {
                pal[2][ch] = static_cast<uint8_t>((2 * pal[0][ch] + pal[1][ch]) / 3);
                pal[3][ch] = static_cast<uint8_t>((pal[0][ch] + 2 * pal[1][ch]) / 3);
            }
            pal[2][3] = 255;
            pal[3][3] = 255;
        } else {
            for (int ch = 0; ch < 3; ++ch) {
                pal[2][ch] = static_cast<uint8_t>((pal[0][ch] + pal[1][ch]) / 2);
                pal[3][ch] = 0;
            }
            pal[2][3] = 255;
            pal[3][3] = 0;
        }

        for (int i = 0; i < 16; ++i) {
            uint32_t idx = (bits >> (2 * i)) & 0x3;
            rgbaOut[i * 4 + 0] = pal[idx][0];
            rgbaOut[i * 4 + 1] = pal[idx][1];
            rgbaOut[i * 4 + 2] = pal[idx][2];
            rgbaOut[i * 4 + 3] = pal[idx][3];
        }
    }

    static void decodeDxt5Block(const uint8_t* block, uint8_t rgbaOut[16 * 4]) {
        uint8_t a0 = block[0];
        uint8_t a1 = block[1];
        uint64_t alphaBits = 0;
        for (int i = 0; i < 6; ++i) {
            alphaBits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
        }

        uint8_t alphaPal[8];
        alphaPal[0] = a0;
        alphaPal[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i <= 6; ++i) {
                alphaPal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
            }
        } else {
            for (int i = 1; i <= 4; ++i) {
                alphaPal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
            }
            alphaPal[6] = 0;
            alphaPal[7] = 255;
        }

        decodeDxt1Block(block + 8, rgbaOut);
        for (int i = 0; i < 16; ++i) {
            uint32_t aIdx = static_cast<uint32_t>((alphaBits >> (3 * i)) & 0x7);
            rgbaOut[i * 4 + 3] = alphaPal[aIdx];
        }
    }

private:
    static void rgb565ToRgb(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
        uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1F);
        uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3F);
        uint8_t b5 = static_cast<uint8_t>(c & 0x1F);
        r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
        b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    }
};

class TextureProcessor {
public:
    explicit TextureProcessor(VfCore::LogSink& log) : log_(log) {}

    bool convertVtf(const fs::path& vtfPath, const fs::path& outDir) {
        std::ifstream in(vtfPath, std::ios::binary);
        if (!in.is_open()) {
            log_.write(VfCore::Level::Error, "VfTexForge: не открыть VTF " + vtfPath.string());
            return false;
        }

        VtfHeader hdr{};
        in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (std::string(hdr.signature, 3) != "VTF" || hdr.width == 0 || hdr.height == 0 || hdr.mipmapCount == 0) {
            log_.write(VfCore::Level::Error, "VfTexForge: повреждён заголовок VTF " + vtfPath.filename().string());
            return false;
        }

        size_t lowResBytes = 0;
        if (hdr.lowResWidth > 0 && hdr.lowResHeight > 0 && hdr.lowResFormat == FMT_DXT1) {
            size_t bw = std::max<size_t>(1, (hdr.lowResWidth + 3) / 4);
            size_t bh = std::max<size_t>(1, (hdr.lowResHeight + 3) / 4);
            lowResBytes = bw * bh * 8;
        }

        in.seekg(static_cast<std::streamoff>(hdr.headerSize + lowResBytes), std::ios::beg);

        auto calcMipBytes = [&](uint16_t w, uint16_t h, uint32_t fmt) -> size_t {
            switch (fmt) {
                case FMT_RGBA8888:
                case FMT_ABGR8888:
                case FMT_BGRA8888:
                    return static_cast<size_t>(w) * h * 4;
                case FMT_RGB888:
                case FMT_BGR888:
                    return static_cast<size_t>(w) * h * 3;
                case FMT_DXT1: {
                    size_t bw = std::max<size_t>(1, (w + 3) / 4);
                    size_t bh = std::max<size_t>(1, (h + 3) / 4);
                    return bw * bh * 8;
                }
                case FMT_DXT5: {
                    size_t bw = std::max<size_t>(1, (w + 3) / 4);
                    size_t bh = std::max<size_t>(1, (h + 3) / 4);
                    return bw * bh * 16;
                }
                default:
                    return 0;
            }
        };

        size_t topMipBytes = calcMipBytes(hdr.width, hdr.height, hdr.highResFormat);
        if (topMipBytes == 0) {
            log_.write(VfCore::Level::Warn,
                "VfTexForge: формат VTF " + std::to_string(hdr.highResFormat) + " пока не поддержан.");
            return false;
        }

        size_t skipBytes = 0;
        for (int mip = static_cast<int>(hdr.mipmapCount) - 1; mip > 0; --mip) {
            uint16_t mw = std::max<uint16_t>(1, hdr.width >> mip);
            uint16_t mh = std::max<uint16_t>(1, hdr.height >> mip);
            skipBytes += calcMipBytes(mw, mh, hdr.highResFormat) * hdr.frames;
        }
        in.seekg(static_cast<std::streamoff>(skipBytes), std::ios::cur);

        std::vector<uint8_t> raw(topMipBytes);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(topMipBytes));
        if (!in) {
            log_.write(VfCore::Level::Error, "VfTexForge: обрезанные данные пикселей в " + vtfPath.filename().string());
            return false;
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(hdr.width) * hdr.height * 4, 255);

        if (hdr.highResFormat == FMT_RGBA8888) {
            rgba = std::move(raw);
        } else if (hdr.highResFormat == FMT_BGRA8888) {
            rgba = std::move(raw);
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        } else if (hdr.highResFormat == FMT_ABGR8888) {
            rgba = std::move(raw);
            for (size_t i = 0; i < rgba.size(); i += 4) {
                uint8_t a = rgba[i], b = rgba[i + 1], g = rgba[i + 2], r = rgba[i + 3];
                rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
            }
        } else if (hdr.highResFormat == FMT_RGB888 || hdr.highResFormat == FMT_BGR888) {
            bool isBgr = (hdr.highResFormat == FMT_BGR888);
            for (size_t p = 0, n = static_cast<size_t>(hdr.width) * hdr.height; p < n; ++p) {
                uint8_t c0 = raw[p * 3 + 0];
                uint8_t c1 = raw[p * 3 + 1];
                uint8_t c2 = raw[p * 3 + 2];
                rgba[p * 4 + 0] = isBgr ? c2 : c0;
                rgba[p * 4 + 1] = c1;
                rgba[p * 4 + 2] = isBgr ? c0 : c2;
                rgba[p * 4 + 3] = 255;
            }
        } else if (hdr.highResFormat == FMT_DXT1 || hdr.highResFormat == FMT_DXT5) {
            size_t bw = std::max<size_t>(1, (hdr.width + 3) / 4);
            size_t bh = std::max<size_t>(1, (hdr.height + 3) / 4);
            size_t stride = (hdr.highResFormat == FMT_DXT1) ? 8 : 16;

            for (size_t by = 0; by < bh; ++by) {
                for (size_t bx = 0; bx < bw; ++bx) {
                    const uint8_t* blk = raw.data() + (by * bw + bx) * stride;
                    uint8_t decoded[64];
                    if (hdr.highResFormat == FMT_DXT1) {
                        DxtDecoder::decodeDxt1Block(blk, decoded);
                    } else {
                        DxtDecoder::decodeDxt5Block(blk, decoded);
                    }
                    for (int py = 0; py < 4; ++py) {
                        size_t y = by * 4 + py;
                        if (y >= hdr.height) break;
                        for (int px = 0; px < 4; ++px) {
                            size_t x = bx * 4 + px;
                            if (x >= hdr.width) break;
                            size_t dstIdx = (y * hdr.width + x) * 4;
                            size_t srcIdx = static_cast<size_t>(py * 4 + px) * 4;
                            for (int c = 0; c < 4; ++c) rgba[dstIdx + c] = decoded[srcIdx + c];
                        }
                    }
                }
            }
        }

        fs::create_directories(outDir);
#if defined(VF_HAS_STB_IMAGE_WRITE)
        fs::path outPng = outDir / (vtfPath.stem().string() + ".png");
        if (!stbi_write_png(outPng.string().c_str(), hdr.width, hdr.height, 4, rgba.data(), hdr.width * 4)) {
            log_.write(VfCore::Level::Error, "VfTexForge: не удалось записать " + outPng.string());
            return false;
        }
        log_.write(VfCore::Level::Info, "VfTexForge: сохранено " + outPng.filename().string());
#else
        fs::path outPpm = outDir / (vtfPath.stem().string() + ".ppm");
        std::ofstream ppm(outPpm, std::ios::binary);
        ppm << "P6\n" << hdr.width << " " << hdr.height << "\n255\n";
        for (size_t p = 0, n = static_cast<size_t>(hdr.width) * hdr.height; p < n; ++p) {
            ppm.write(reinterpret_cast<const char*>(&rgba[p * 4]), 3);
        }
        log_.write(VfCore::Level::Info,
            "VfTexForge: декодировано и сохранено " + outPpm.filename().string() +
            " (подключите VF_HAS_STB_IMAGE_WRITE для PNG)");
#endif
        return true;
    }

    void convertVtex(const fs::path& vtexPath, const fs::path& outDir) {
        fs::create_directories(outDir);
        fs::path outPng = outDir / (vtexPath.stem().string() + ".png");
        std::string cmd = "Decompiler.exe -i \"" + vtexPath.string() + "\" -o \"" + outPng.string() + "\"";
        log_.write(VfCore::Level::Info, "VfTexForge (Source 2): команда экспорта -> " + cmd);
    }

private:
    VfCore::LogSink& log_;
};

}