#include "png_encoder.hpp"

#include <cstring>

namespace cam {

void PngEncoder::ensureCrcTable()
{
    if (crcReady_) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTab_[n] = c;
    }
    crcReady_ = true;
}

uint32_t PngEncoder::crc32(const uint8_t* data, size_t len, uint32_t crc,
                           const uint32_t tab[256])
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t PngEncoder::adler32(const uint8_t* data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PngEncoder::putU32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 写一个 PNG chunk：长度 + 类型 + 数据 + CRC32，返回写入字节数 */
size_t PngEncoder::writeChunk(uint8_t* buf, const char type[4],
                              const uint8_t* data, uint32_t len,
                              const uint32_t tab[256])
{
    putU32(buf, len);
    std::memcpy(buf + 4, type, 4);
    if (len > 0) std::memcpy(buf + 8, data, len);
    uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(type), 4, 0, tab);
    if (len > 0) crc = crc32(data, len, crc, tab);
    putU32(buf + 8 + len, crc);
    return 8 + len + 4;
}

std::vector<uint8_t> PngEncoder::encodeGray(const uint8_t* gray, int w, int h)
{
    if (gray == nullptr || w <= 0 || h <= 0) return {};

    const size_t rawLine = static_cast<size_t>(w) + 1;   /* 行首补 0 filter 字节 */
    const size_t rawLen  = rawLine * static_cast<size_t>(h);
    const size_t blkCnt  = rawLen / 65535 + 1;
    const size_t zlen    = 2 + rawLen + blkCnt * 5 + 4;
    const size_t need    = 8 + 25 + (8 + zlen + 4) + 12;

    std::vector<uint8_t> out(need);
    std::vector<uint8_t> raw(rawLen);
    const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    ensureCrcTable();

    /* 拼接原始扫描线：每行前加 1 字节 filter=0（None） */
    size_t rawPos = 0;
    for (int y = 0; y < h; y++) {
        raw[rawPos++] = 0;
        std::memcpy(raw.data() + rawPos, gray + static_cast<size_t>(y) * w,
                    static_cast<size_t>(w));
        rawPos += static_cast<size_t>(w);
    }

    size_t pos = 0;
    std::memcpy(out.data(), sig, 8);
    pos = 8;

    uint8_t ihdr[13];
    putU32(ihdr, static_cast<uint32_t>(w));
    putU32(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8]  = 8;    /* bit depth */
    ihdr[9]  = 0;    /* color type: 0=grayscale */
    ihdr[10] = 0;    /* compression: deflate */
    ihdr[11] = 0;    /* filter: adaptive */
    ihdr[12] = 0;    /* interlace: none */
    pos += writeChunk(out.data() + pos, "IHDR", ihdr, sizeof(ihdr), crcTab_);

    /* IDAT：先写 chunk 头（长度+类型），再把 zlib 流直接写入数据区，最后补 CRC */
    {
        putU32(out.data() + pos, static_cast<uint32_t>(zlen));
        std::memcpy(out.data() + pos + 4, "IDAT", 4);
        pos += 8;
        const size_t dataStart = pos;
        out[pos++] = 0x78;
        out[pos++] = 0x01;
        rawPos = 0;
        while (rawPos < rawLen) {
            size_t n    = rawLen - rawPos;
            size_t left = n > 65535 ? 65535 : n;
            out[pos++] = static_cast<uint8_t>((rawPos + left >= rawLen) ? 0x01 : 0x00);
            out[pos++] = static_cast<uint8_t>(left & 0xFF);
            out[pos++] = static_cast<uint8_t>((left >> 8) & 0xFF);
            out[pos++] = static_cast<uint8_t>(~left & 0xFF);
            out[pos++] = static_cast<uint8_t>((~left >> 8) & 0xFF);
            std::memcpy(out.data() + pos, raw.data() + rawPos, left);
            pos += left;
            rawPos += left;
        }
        const uint32_t adler = adler32(raw.data(), rawLen);
        putU32(out.data() + pos, adler);
        pos += 4;
        uint32_t crc = crc32(reinterpret_cast<const uint8_t*>("IDAT"), 4, 0, crcTab_);
        crc = crc32(out.data() + dataStart, pos - dataStart, crc, crcTab_);
        putU32(out.data() + pos, crc);
        pos += 4;
    }

    pos += writeChunk(out.data() + pos, "IEND", nullptr, 0, crcTab_);
    out.resize(pos);
    return out;
}

}  /* namespace cam */
