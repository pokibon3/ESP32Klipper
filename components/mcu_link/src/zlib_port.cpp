// zlib inflate for the data dictionary (see msgproto.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/msgproto.hpp"

#include <cstdlib>

#ifdef ESP_PLATFORM
#include "miniz.h" // ROM copy of miniz (tinfl)
#else
#include <zlib.h>
#endif

namespace kl {

bool
zlib_inflate(const uint8_t *data, size_t len, std::string *out)
{
    // The dictionary of a full RP2040 build is ~25 KB uncompressed; grow
    // the buffer until it fits.
    for (size_t cap = 16 * 1024; cap <= 256 * 1024; cap *= 2) {
        out->resize(cap);
#ifdef ESP_PLATFORM
        // tinfl_decompress_mem_to_mem() keeps its ~11 KB state on the
        // stack; allocate it on the heap instead
        tinfl_decompressor *d = (tinfl_decompressor *)malloc(sizeof(*d));
        if (!d)
            break;
        tinfl_init(d);
        size_t in_size = len, out_size = cap;
        tinfl_status st = tinfl_decompress(
            d, data, &in_size, (mz_uint8 *)&(*out)[0], (mz_uint8 *)&(*out)[0],
            &out_size, TINFL_FLAG_PARSE_ZLIB_HEADER
            | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        free(d);
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT)
            continue;
        if (st != TINFL_STATUS_DONE)
            break;
        out->resize(out_size);
        out->shrink_to_fit();
        return true;
#else
        uLongf n = cap;
        int ret = uncompress((Bytef *)&(*out)[0], &n, data, len);
        if (ret == Z_BUF_ERROR)
            continue;
        if (ret != Z_OK)
            break;
        out->resize(n);
        out->shrink_to_fit();
        return true;
#endif
    }
    out->clear();
    return false;
}

} // namespace kl
