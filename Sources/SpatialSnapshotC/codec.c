#include "internal.h"

uint32_t ss_crc32c(const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & (0u - (crc & 1u)));
    }
    return crc ^ UINT32_MAX;
}

static bool lz4_length(const uint8_t *src, size_t n, size_t *input, size_t *length, size_t limit) {
    unsigned b;
    do {
        if (*input == n)
            return false;
        b = src[(*input)++];
        if (b > limit - *length)
            return false;
        *length += b;
    } while (b == 255);
    return true;
}

/* Independent raw blocks; overlapping matches are intentionally copied
 * forward byte-by-byte. Never read an unproduced byte or use a dictionary. */
ss_status_t ssi_lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t raw) {
    size_t in = 0, out = 0, last_match_start = SIZE_MAX;
    while (in < n) {
        uint8_t token = src[in++];
        size_t literals = token >> 4;
        if (literals > raw - out)
            return SS_MALFORMED;
        if (literals == 15 && !lz4_length(src, n, &in, &literals, raw - out))
            return SS_MALFORMED;
        if (literals > n - in || literals > raw - out)
            return SS_MALFORMED;
        if (literals)
            memcpy(dst + out, src + in, literals);
        in += literals;
        out += literals;
        if (in == n) {
            if (out != raw)
                return SS_MALFORMED;
            if (last_match_start != SIZE_MAX &&
                (literals < 5 || raw < 12 || last_match_start > raw - 12))
                return SS_MALFORMED;
            return SS_OK;
        }
        if (n - in < 2)
            return SS_MALFORMED;
        size_t offset = ssi_u16(src + in);
        in += 2;
        if (!offset || offset > out)
            return SS_MALFORMED;
        size_t match = (token & 15u) + 4u;
        if (match > raw - out)
            return SS_MALFORMED;
        if ((token & 15u) == 15 && !lz4_length(src, n, &in, &match, raw - out))
            return SS_MALFORMED;
        last_match_start = out;
        for (size_t j = 0; j < match; ++j)
            dst[out + j] = dst[out + j - offset];
        out += match;
    }
    return SS_MALFORMED; /* A final literal-only sequence is required. */
}

/* Literal-only LZ4 blocks are fully conforming and deterministic. This
 * baseline deliberately prioritizes a small auditable encoder over ratio. */
ss_status_t ssi_lz4_encode(ssi_buffer_t *out, const uint8_t *src, size_t n) {
    uint8_t token = (uint8_t)(SSI_MIN(n, 15) << 4);
    ss_status_t s = ssi_buffer_append(out, &token, 1);
    if (s != SS_OK)
        return s;
    if (n >= 15) {
        size_t remaining = n - 15;
        while (remaining >= 255) {
            uint8_t b = 255;
            s = ssi_buffer_append(out, &b, 1);
            if (s != SS_OK)
                return s;
            remaining -= 255;
        }
        uint8_t b = (uint8_t)remaining;
        s = ssi_buffer_append(out, &b, 1);
        if (s != SS_OK)
            return s;
    }
    return ssi_buffer_append(out, src, n);
}

ss_status_t ssi_packet_raw(const ss_document_t *d, const ssi_packet_t *p, uint8_t **out) {
    *out = NULL;
    if (!ssi_known(p->info.type))
        return SS_UNSUPPORTED;
    size_t n = p->info.raw_payload_size;
    if (n > d->memory->options.max_packet_raw_bytes)
        return SS_RESOURCE_LIMIT;
    if (!n)
        return SS_OK;
    uint8_t *raw = ssi_alloc(d->memory, n);
    if (!raw)
        return d->memory->failure;
    const uint8_t *stored = d->data + (size_t)p->info.byte_offset + p->info.header_size;
    ss_status_t s = SS_OK;
    if (p->info.flags & SS_FLAG_LZ4)
        s = ssi_lz4_decode(stored, p->info.stored_payload_size, raw, n);
    else
        memcpy(raw, stored, n);
    if (s != SS_OK) {
        ssi_free(raw);
        return s;
    }
    *out = raw;
    return SS_OK;
}

ss_status_t ssi_emit_packet(ssi_buffer_t *out, uint64_t seq, uint64_t time, uint16_t type,
                            const uint8_t *raw, size_t n) {
    if (n > SSPS_MAX_PACKET_BYTES || !ssi_known(type))
        return SS_INVALID_ARGUMENT;
    if (n > out->memory->options.max_packet_raw_bytes)
        return SS_RESOURCE_LIMIT;
    bool compressed = type == SS_PACKET_GEOMETRY_PUT || type == SS_PACKET_DEPTH_SAMPLE;
    ssi_buffer_t block = {.memory = out->memory};
    const uint8_t *payload = raw;
    size_t stored = n;
    ss_status_t s = SS_OK;
    if (compressed) {
        s = ssi_lz4_encode(&block, raw, n);
        if (s != SS_OK)
            goto done;
        payload = block.data;
        stored = block.size;
    }
    if (stored > SSPS_MAX_PACKET_BYTES) {
        s = SS_RESOURCE_LIMIT;
        goto done;
    }
    uint8_t header[48] = {0};
    memcpy(header, "SSPK", 4);
    ssi_w16(header + 4, 48);
    ssi_w16(header + 6, type);
    ssi_w32(header + 8, SS_FLAG_CRITICAL | (compressed ? SS_FLAG_LZ4 : 0));
    ssi_w64(header + 12, seq);
    ssi_w64(header + 20, time);
    ssi_w32(header + 28, (uint32_t)stored);
    ssi_w32(header + 32, (uint32_t)n);
    ssi_w32(header + 36, ss_crc32c(payload, stored));
    ssi_w32(header + 40, ss_crc32c(header, sizeof(header)));
    size_t original = out->size;
    s = ssi_buffer_append(out, header, sizeof(header));
    if (s == SS_OK)
        s = ssi_buffer_append(out, payload, stored);
    if (s != SS_OK)
        out->size = original;
done:
    ssi_buffer_clear(&block);
    return s;
}

static bool intrinsics(uint32_t width, uint32_t height, float fx, float fy, float cx, float cy,
                       uint32_t limit) {
    return width >= 1 && width <= limit && height >= 1 && height <= limit && isfinite(fx) &&
           isfinite(fy) && isfinite(cx) && isfinite(cy) && fx > 0 && fy > 0 && cx >= 0 && cy >= 0 &&
           (double)cx <= width - 1 && (double)cy <= height - 1;
}
ss_status_t ssi_camera_validate(const ss_camera_t *c, bool first, bool canonical) {
    if (!SSI_VALID(c, ss_camera_t))
        return SS_INVALID_ARGUMENT;
    if (!intrinsics(c->raster_width, c->raster_height, c->fx, c->fy, c->cx, c->cy, 16384))
        return SS_MALFORMED;
    if (!isfinite(c->tx) || !isfinite(c->ty) || !isfinite(c->tz) || !isfinite(c->qx) ||
        !isfinite(c->qy) || !isfinite(c->qz) || !isfinite(c->qw))
        return SS_MALFORMED;
    double x = c->qx, y = c->qy, z = c->qz, w = c->qw;
    double norm = sqrt(x * x + y * y + z * z + w * w);
    if (fabs(norm - 1) > 1e-4)
        return SS_MALFORMED;
    if (canonical && (w < 0 || (w == 0 && (x < 0 || (x == 0 && (y < 0 || (y == 0 && z < 0)))))))
        return SS_MALFORMED;
    if (first && (c->timestamp_ns != 0 || c->tx != 0 || c->ty != 0 || c->tz != 0 || c->qx != 0 ||
                  c->qy != 0 || c->qz != 0 || c->qw != 1))
        return SS_MALFORMED;
    return SS_OK;
}
ss_status_t ssi_camera_decode(const uint8_t *p, size_t n, uint64_t time, ss_camera_t *c) {
    if (n != 64 || !ssi_zero(p + 52, 12))
        return SS_MALFORMED;
    ss_camera_init(c);
    c->timestamp_ns = time;
    c->raster_width = ssi_u32(p);
    c->raster_height = ssi_u32(p + 4);
    c->fx = ssi_f32(p + 8);
    c->fy = ssi_f32(p + 12);
    c->cx = ssi_f32(p + 16);
    c->cy = ssi_f32(p + 20);
    c->tx = ssi_f32(p + 24);
    c->ty = ssi_f32(p + 28);
    c->tz = ssi_f32(p + 32);
    c->qx = ssi_f32(p + 36);
    c->qy = ssi_f32(p + 40);
    c->qz = ssi_f32(p + 44);
    c->qw = ssi_f32(p + 48);
    return ssi_camera_validate(c, false, true);
}
void ssi_camera_encode(uint8_t p[64], const ss_camera_t *c) {
    memset(p, 0, 64);
    ssi_w32(p, c->raster_width);
    ssi_w32(p + 4, c->raster_height);
    ssi_wf32(p + 8, c->fx);
    ssi_wf32(p + 12, c->fy);
    ssi_wf32(p + 16, c->cx);
    ssi_wf32(p + 20, c->cy);
    ssi_wf32(p + 24, c->tx);
    ssi_wf32(p + 28, c->ty);
    ssi_wf32(p + 32, c->tz);
    double norm = sqrt((double)c->qx * c->qx + (double)c->qy * c->qy + (double)c->qz * c->qz +
                       (double)c->qw * c->qw);
    double sign =
        (c->qw < 0 ||
         (c->qw == 0 && (c->qx < 0 || (c->qx == 0 && (c->qy < 0 || (c->qy == 0 && c->qz < 0))))))
            ? -1
            : 1;
    ssi_wf32(p + 36, (float)(sign * c->qx / norm));
    ssi_wf32(p + 40, (float)(sign * c->qy / norm));
    ssi_wf32(p + 44, (float)(sign * c->qz / norm));
    ssi_wf32(p + 48, (float)(sign * c->qw / norm));
}

ss_status_t ssi_depth_validate_raw(const uint8_t *p, size_t n) {
    if (n < 32)
        return SS_MALFORMED;
    uint32_t w = ssi_u32(p), h = ssi_u32(p + 4);
    if (!intrinsics(w, h, ssi_f32(p + 8), ssi_f32(p + 12), ssi_f32(p + 16), ssi_f32(p + 20), 4096))
        return SS_MALFORMED;
    uint64_t pixels64 = (uint64_t)w * h;
    if (pixels64 > 4194304 || !ssi_zero(p + 24, 8))
        return SS_MALFORMED;
    size_t pixels = (size_t)pixels64;
    if (n != 32 + 2 * pixels + (pixels + 3) / 4)
        return SS_MALFORMED;
    const uint8_t *confidence = p + 32 + 2 * pixels;
    for (size_t i = 0; i < pixels; ++i) {
        unsigned c = (confidence[i >> 2] >> (2 * (i & 3))) & 3u;
        if ((ssi_u16(p + 32 + 2 * i) == 0) != (c == 0))
            return SS_MALFORMED;
    }
    if ((pixels & 3) && (confidence[pixels >> 2] >> (2 * (pixels & 3))) != 0)
        return SS_MALFORMED;
    return SS_OK;
}
ss_status_t ssi_depth_encode(ssi_buffer_t *b, const ss_depth_view_t *d) {
    if (!SSI_VALID(d, ss_depth_view_t) || !d->depth_mm || !d->confidence ||
        !intrinsics(d->width, d->height, d->fx, d->fy, d->cx, d->cy, 4096))
        return SS_INVALID_ARGUMENT;
    uint64_t n64 = (uint64_t)d->width * d->height;
    if (n64 > 4194304)
        return SS_INVALID_ARGUMENT;
    size_t n = (size_t)n64, size = 32 + 2 * n + (n + 3) / 4;
    if (size > b->memory->options.max_packet_raw_bytes)
        return SS_RESOURCE_LIMIT;
    uint8_t *raw = ssi_alloc(b->memory, size);
    if (!raw)
        return b->memory->failure;
    memset(raw, 0, size);
    ssi_w32(raw, d->width);
    ssi_w32(raw + 4, d->height);
    ssi_wf32(raw + 8, d->fx);
    ssi_wf32(raw + 12, d->fy);
    ssi_wf32(raw + 16, d->cx);
    ssi_wf32(raw + 20, d->cy);
    for (size_t i = 0; i < n; ++i) {
        if (d->confidence[i] > 3 || ((d->depth_mm[i] == 0) != (d->confidence[i] == 0))) {
            ssi_free(raw);
            return SS_INVALID_ARGUMENT;
        }
        ssi_w16(raw + 32 + 2 * i, d->depth_mm[i]);
        raw[32 + 2 * n + (i >> 2)] |= (uint8_t)(d->confidence[i] << (2 * (i & 3)));
    }
    ss_status_t s = ssi_buffer_append(b, raw, size);
    ssi_free(raw);
    return s;
}
