#pragma once
/* 极简 PNG 编码器：8bit 灰度，zlib stored 块（无压缩），无外部依赖。
 * 输出 std::vector<uint8_t>，失败返回空 vector。 */

#include <cstdint>
#include <cstddef>
#include <vector>

namespace cam {

class PngEncoder {
public:
    PngEncoder() = default;

    /* gray 为 w*h 字节的灰度像素，成功返回 PNG 字节流，失败返回空 */
    std::vector<uint8_t> encodeGray(const uint8_t* gray, int w, int h);

private:
    void ensureCrcTable();
    static uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc,
                          const uint32_t tab[256]);
    static uint32_t adler32(const uint8_t* data, size_t len);
    static void putU32(uint8_t* p, uint32_t v);
    static size_t writeChunk(uint8_t* buf, const char type[4],
                             const uint8_t* data, uint32_t len,
                             const uint32_t tab[256]);

    uint32_t crcTab_[256] = {0};
    bool     crcReady_    = false;
};

}  /* namespace cam */
