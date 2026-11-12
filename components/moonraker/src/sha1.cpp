// SHA-1 (FIPS 180-1) and Base64 for the WebSocket handshake.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "moonraker/sha1.hpp"

#include <cstring>

namespace kl {

namespace {
inline uint32_t
rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}
} // namespace

void
sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    const uint8_t *p = (const uint8_t *)data;
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t block[64];
    for (size_t off = 0; off < total; off += 64) {
        for (int i = 0; i < 64; i++) {
            size_t k = off + i;
            if (k < len)
                block[i] = p[k];
            else if (k == len)
                block[i] = 0x80;
            else if (k >= total - 8)
                block[i] = (uint8_t)(bits >> ((total - 1 - k) * 8));
            else
                block[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16
                   | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(h[i] >> (24 - j * 8));
}

std::string
base64_encode(const uint8_t *d, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < len)
            v |= d[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < len ? tbl[v & 63] : '=';
    }
    return out;
}

std::string
websocket_accept(const std::string &key)
{
    std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t h[20];
    sha1(s.data(), s.size(), h);
    return base64_encode(h, 20);
}

} // namespace kl
