#include "bmff.h"

/* Decoder configuration framing can be checked without decoding video/image pixels.
 * NAL payload semantics belong to the codec backend, not the SSPS runtime. */
ss_status_t bm_codec_config(bm_context *c, uint32_t type, const uint8_t *data, size_t size,
                            uint64_t offset) {
    bm_cursor r = {c, data, size, 0, offset, false};
    if (type == BM_FOUR('h', 'v', 'c', 'C')) {
        if (size < 23)
            return bm_host(c, offset, "truncated hvcC decoder configuration");
        if (data[0] != 1)
            return bm_error(c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, offset, 0,
                            "unknown hvcC version");
        r.pos = 22;
        uint32_t arrays = (uint32_t)bm_get(&r, 1);
        for (uint32_t i = 0; i < arrays && !r.failed; ++i) {
            (void)bm_get(&r, 1);
            uint32_t nals = (uint32_t)bm_get(&r, 2);
            for (uint32_t j = 0; j < nals && !r.failed; ++j) {
                size_t n = (size_t)bm_get(&r, 2);
                if (!n)
                    return bm_host(c, offset, "empty hvcC NAL unit");
                (void)bm_take(&r, n);
            }
        }
        return bm_done(&r);
    }
    if (type == BM_FOUR('a', 'v', 'c', 'C')) {
        if (size < 7)
            return bm_host(c, offset, "truncated avcC decoder configuration");
        if (data[0] != 1)
            return bm_error(c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, offset, 0,
                            "unknown avcC version");
        r.pos = 5;
        uint32_t sets = (uint32_t)bm_get(&r, 1) & 31;
        for (unsigned group = 0; group < 2; ++group) {
            for (uint32_t i = 0; i < sets && !r.failed; ++i) {
                size_t n = (size_t)bm_get(&r, 2);
                (void)bm_take(&r, n);
            }
            if (!group)
                sets = (uint32_t)bm_get(&r, 1);
        }
        /* High-profile extensions follow the base SPS/PPS arrays. */
        if (!r.failed && r.pos != r.size) {
            uint8_t profile = data[1];
            if (profile != 100 && profile != 110 && profile != 122 && profile != 144 &&
                profile != 244 && profile != 44 && profile != 83 && profile != 86 &&
                profile != 118 && profile != 128 && profile != 138 && profile != 139 &&
                profile != 134 && profile != 135)
                return bm_host(c, offset, "unexpected avcC trailing fields");
            (void)bm_take(&r, 3);
            sets = (uint32_t)bm_get(&r, 1);
            for (uint32_t i = 0; i < sets && !r.failed; ++i) {
                size_t n = (size_t)bm_get(&r, 2);
                (void)bm_take(&r, n);
            }
        }
        return bm_done(&r);
    }
    if (type == BM_FOUR('a', 'v', '1', 'C')) {
        if (size < 4)
            return bm_host(c, offset, "truncated av1C decoder configuration");
        if (data[0] != 0x81)
            return bm_error(c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, offset, 0,
                            "unknown av1C version");
    }
    return SS_OK;
}
