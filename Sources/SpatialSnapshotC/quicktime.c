#include "bmff.h"

typedef struct qt_sample {
    uint64_t offset, size, dts, pts;
    uint32_t duration, description, decode_index;
    bool sync;
} qt_sample;
typedef struct qt_track {
    uint32_t box, tkhd, mdia, mdhd, minf, stbl, stsd, tref, entry;
    uint32_t id, handler, timescale, language, flags, width, height, entry_count;
    uint64_t duration, track_duration, decode_duration;
    uint32_t key, referent;
    qt_sample *samples;
    size_t count;
    bool candidate, spatial, reordered;
} qt_track;
typedef struct qt_file {
    bm_context *c;
    uint32_t moov, mvhd, timescale;
    uint64_t duration, expanded;
    qt_track *tracks;
    size_t count;
    qt_track *metadata, *video;
    size_t key_count;
} qt_file;

static ss_status_t qt_unsupported(qt_file *q, uint64_t off, uint32_t id, const char *message) {
    return bm_error(q->c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, off, id, message);
}
static ss_status_t qt_full(qt_file *q, uint32_t box, unsigned version, bm_cursor *r, uint32_t *vf) {
    BM_TRY(bm_load(q->c, box, r));
    *vf = (uint32_t)bm_get(r, 4);
    if (r->failed)
        return bm_host(q->c, r->offset, "truncated QuickTime FullBox");
    if ((*vf >> 24) > version)
        return qt_unsupported(q, r->offset, 0, "unsupported QuickTime atom version");
    return SS_OK;
}
static qt_track *qt_find(qt_file *q, uint32_t id) {
    for (size_t i = 0; i < q->count; ++i)
        if (q->tracks[i].id == id)
            return &q->tracks[i];
    return NULL;
}
static ss_status_t qt_header(qt_file *q, uint32_t b, bool movie, uint32_t *scale,
                             uint64_t *duration, uint32_t *language) {
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, b, 1, &r, &vf));
    unsigned v = vf >> 24;
    size_t size = movie ? (v ? 112u : 100u) : (v ? 36u : 24u);
    if (r.size != size)
        return bm_host(q->c, r.offset, "invalid movie/media header field widths");
    *scale = bm_u32(r.data + (v ? 20 : 12));
    *duration = v ? bm_u64(r.data + 24) : bm_u32(r.data + 16);
    if (!*scale)
        return bm_host(q->c, r.offset, "zero timescale");
    if (language && !movie)
        *language = bm_u16(r.data + (v ? 32 : 20));
    return SS_OK;
}
static ss_status_t qt_track_header(qt_file *q, qt_track *t) {
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, t->tkhd, 1, &r, &vf));
    unsigned v = vf >> 24;
    if (r.size != (v ? 96u : 84u))
        return bm_host(q->c, r.offset, "invalid track header field widths");
    t->id = bm_u32(r.data + (v ? 20 : 12));
    t->flags = vf & 0xffffff;
    t->track_duration = v ? bm_u64(r.data + 28) : bm_u32(r.data + 20);
    if (!t->id)
        return bm_host(q->c, r.offset, "zero track ID");
    return SS_OK;
}
static bool qt_matrix(const uint8_t *p) {
    static const uint32_t identity[] = {65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824};
    for (unsigned i = 0; i < 9; ++i)
        if (bm_u32(p + i * 4) != identity[i])
            return false;
    return true;
}
static ss_status_t qt_description(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, t->stsd, 0, &r, &vf));
    t->entry_count = (uint32_t)bm_get(&r, 4);
    if (r.failed)
        return bm_host(c, r.offset, "truncated stsd entry count");
    if (t->entry_count > 256)
        return bm_limit(c, r.offset, "sample-description count limit");
    BM_TRY(bm_children(c, t->stsd, r.offset + r.pos, r.offset + r.size, 7, false));
    t->entry = c->boxes[t->stsd].first;
    uint32_t n = 0;
    for (uint32_t b = t->entry; b != BM_NONE; b = c->boxes[b].next) {
        ++n;
        BM_TRY(bm_load(c, b, &r));
        if (r.size < 8)
            return bm_host(c, r.offset, "truncated sample description");
        size_t prefix = t->handler == BM_FOUR('v', 'i', 'd', 'e')         ? 78
                        : c->boxes[b].type == BM_FOUR('m', 'e', 'b', 'x') ? 8
                                                                          : 0;
        if (prefix) {
            if (r.size < prefix)
                return bm_host(c, r.offset, "truncated visual/metadata sample entry");
            BM_TRY(bm_children(c, b, r.offset + prefix, r.offset + r.size, 8, false));
            if (t->handler == BM_FOUR('v', 'i', 'd', 'e')) {
                if (bm_u16(r.data + 8))
                    return qt_unsupported(q, r.offset, t->id,
                                          "unknown visual sample-entry version");
                uint32_t format = c->boxes[b].type, needed = 0;
                if (format == BM_FOUR('a', 'v', 'c', '1') || format == BM_FOUR('a', 'v', 'c', '3'))
                    needed = BM_FOUR('a', 'v', 'c', 'C');
                if (format == BM_FOUR('h', 'v', 'c', '1') || format == BM_FOUR('h', 'e', 'v', '1'))
                    needed = BM_FOUR('h', 'v', 'c', 'C');
                if (format == BM_FOUR('a', 'v', '0', '1'))
                    needed = BM_FOUR('a', 'v', '1', 'C');
                if (needed) {
                    uint32_t cfg;
                    BM_TRY(bm_unique(c, b, needed, true, &cfg));
                    bm_cursor config;
                    BM_TRY(bm_load(c, cfg, &config));
                    BM_TRY(bm_codec_config(c, needed, config.data, config.size, config.offset));
                }
            }
        }
    }
    if (n != t->entry_count)
        return bm_host(c, c->boxes[t->stsd].offset, "stsd entry count mismatch");
    return SS_OK;
}
static ss_status_t qt_data_references(qt_file *q, qt_track *t, bool metadata) {
    bm_context *c = q->c;
    uint32_t dinf, dref;
    BM_TRY(bm_unique(c, t->minf, BM_FOUR('d', 'i', 'n', 'f'), true, &dinf));
    BM_TRY(bm_unique(c, dinf, BM_FOUR('d', 'r', 'e', 'f'), true, &dref));
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, dref, 0, &r, &vf));
    uint32_t count = (uint32_t)bm_get(&r, 4), actual = 0;
    if (count > 65536)
        return bm_limit(c, r.offset, "data reference count limit");
    for (uint32_t b = c->boxes[dref].first; b != BM_NONE; b = c->boxes[b].next)
        ++actual;
    if (actual != count)
        return bm_host(c, r.offset, "dref entry count mismatch");
    for (uint32_t e = t->entry; e != BM_NONE; e = c->boxes[e].next) {
        BM_TRY(bm_load(c, e, &r));
        uint32_t index = bm_u16(r.data + 6);
        if (!index || index > count)
            return bm_host(c, r.offset, "sample entry data reference index is invalid");
        uint32_t b = c->boxes[dref].first;
        for (uint32_t i = 1; i < index; ++i)
            b = c->boxes[b].next;
        BM_TRY(qt_full(q, b, 0, &r, &vf));
        if (!(vf & 1)) {
            if (metadata)
                bm_check(c, false, r.offset, t->id, "SSPS metadata data reference is external");
            else
                return bm_host(c, r.offset, "media samples are not self-contained in host mdat");
        }
    }
    return SS_OK;
}
static ss_status_t qt_sizes(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    uint32_t sz, z2;
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 's', 'z'), false, &sz));
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 'z', '2'), false, &z2));
    if ((sz == BM_NONE) == (z2 == BM_NONE))
        return bm_host(c, c->boxes[t->stbl].offset, "need exactly one stsz or stz2");
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, sz != BM_NONE ? sz : z2, 0, &r, &vf));
    uint32_t constant = (uint32_t)bm_get(&r, 4), count = (uint32_t)bm_get(&r, 4);
    if (r.failed)
        return bm_host(c, r.offset, "truncated sample size table");
    if (count > 20000000 - q->expanded)
        return bm_limit(c, r.offset, "expanded sample table limit");
    if (t->candidate && count > 10000000)
        return bm_limit(c, r.offset, "metadata sample count limit");
    q->expanded += count;
    unsigned bits = sz != BM_NONE ? 32 : constant & 255;
    if (z2 != BM_NONE && ((constant >> 8) || (bits != 4 && bits != 8 && bits != 16)))
        return bm_host(c, r.offset, "invalid stz2 field size/reserved bits");
    uint64_t required = sz != BM_NONE && constant ? 0 : ((uint64_t)count * bits + 7) / 8;
    if (required != r.size - r.pos)
        return bm_host(c, r.offset, "sample size table length mismatch");
    t->samples = ssi_alloc(c->memory, (size_t)count * sizeof(*t->samples));
    if (!t->samples)
        return c->memory->failure;
    memset(t->samples, 0, (size_t)count * sizeof(*t->samples));
    t->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = constant;
        if (sz == BM_NONE || !constant) {
            if (bits == 4)
                n = (r.data[r.pos + i / 2] >> (i & 1 ? 0 : 4)) & 15u;
            else
                n = (uint32_t)bm_get(&r, bits / 8);
        }
        t->samples[i].size = n;
        t->samples[i].sync = true;
        t->samples[i].decode_index = i;
    }
    return SS_OK;
}
static ss_status_t qt_times(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    uint32_t stts, ctts;
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 't', 's'), true, &stts));
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('c', 't', 't', 's'), false, &ctts));
    bm_cursor r;
    uint32_t vf;
    BM_TRY(qt_full(q, stts, 0, &r, &vf));
    uint32_t count = (uint32_t)bm_get(&r, 4);
    size_t index = 0;
    uint64_t time = 0;
    if (r.failed || count > (r.size - r.pos) / 8 || (uint64_t)count * 8 != r.size - r.pos)
        return bm_host(c, r.offset, "stts entry length mismatch");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = (uint32_t)bm_get(&r, 4), delta = (uint32_t)bm_get(&r, 4);
        if (!n || n > t->count - index)
            return bm_host(c, r.offset, "stts sample count mismatch");
        if ((uint64_t)n * delta > UINT64_MAX - time)
            return bm_host(c, r.offset, "stts duration overflow");
        for (uint32_t j = 0; j < n; ++j) {
            t->samples[index].dts = t->samples[index].pts = time;
            t->samples[index++].duration = delta;
            time += delta;
        }
    }
    if (index != t->count)
        return bm_host(c, r.offset, "stts does not cover all samples");
    t->decode_duration = time;
    if (ctts != BM_NONE) {
        BM_TRY(qt_full(q, ctts, 1, &r, &vf));
        count = (uint32_t)bm_get(&r, 4);
        index = 0;
        if (r.failed || (uint64_t)count * 8 != r.size - r.pos)
            return bm_host(c, r.offset, "ctts entry length mismatch");
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = (uint32_t)bm_get(&r, 4);
            const uint8_t *p = bm_take(&r, 4);
            int64_t offset = vf >> 24 ? bm_i32(p) : (int64_t)bm_u32(p);
            if (!n || n > t->count - index) {
                bm_check(c, false, r.offset, t->id, "ctts does not exactly cover video samples");
                return SS_MALFORMED;
            }
            for (uint32_t j = 0; j < n; ++j) {
                qt_sample *s = &t->samples[index++];
                if ((offset < 0 && s->dts < (uint64_t)-offset) ||
                    (offset >= 0 && (uint64_t)offset > UINT64_MAX - s->dts)) {
                    bm_check(c, false, r.offset, t->id,
                             "negative or overflowing video presentation timestamp");
                    return SS_MALFORMED;
                }
                s->pts = offset < 0 ? s->dts - (uint64_t)-offset : s->dts + (uint64_t)offset;
            }
        }
        if (index != t->count) {
            bm_check(c, false, r.offset, t->id, "ctts sample coverage mismatch");
            return SS_MALFORMED;
        }
    }
    uint32_t stss;
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 's', 's'), false, &stss));
    if (stss != BM_NONE) {
        BM_TRY(qt_full(q, stss, 0, &r, &vf));
        count = (uint32_t)bm_get(&r, 4);
        if (r.failed || (uint64_t)count * 4 != r.size - r.pos)
            return bm_host(c, r.offset, "stss length mismatch");
        for (size_t i = 0; i < t->count; ++i)
            t->samples[i].sync = false;
        uint32_t prev = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = (uint32_t)bm_get(&r, 4);
            if (n <= prev || n > t->count)
                return bm_host(c, r.offset, "invalid stss sample index");
            t->samples[n - 1].sync = true;
            prev = n;
        }
    }
    return SS_OK;
}
static ss_status_t qt_offsets(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    uint32_t stco, co64, stsc;
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 'c', 'o'), false, &stco));
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('c', 'o', '6', '4'), false, &co64));
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 's', 'c'), true, &stsc));
    if ((stco == BM_NONE) == (co64 == BM_NONE))
        return bm_host(c, 0, "need exactly one stco or co64");
    bm_cursor offsets, chunks;
    uint32_t vf;
    BM_TRY(qt_full(q, stco != BM_NONE ? stco : co64, 0, &offsets, &vf));
    uint32_t chunk_count = (uint32_t)bm_get(&offsets, 4);
    unsigned width = stco != BM_NONE ? 4 : 8;
    if (offsets.failed || (uint64_t)chunk_count * width != offsets.size - offsets.pos)
        return bm_host(c, offsets.offset, "chunk offset count mismatch");
    if (chunk_count > 20000000)
        return bm_limit(c, offsets.offset, "chunk count limit");
    BM_TRY(qt_full(q, stsc, 0, &chunks, &vf));
    uint32_t runs = (uint32_t)bm_get(&chunks, 4);
    if (chunks.failed || (uint64_t)runs * 12 != chunks.size - chunks.pos || runs > chunk_count)
        return bm_host(c, chunks.offset, "stsc length/count mismatch");
    if (!runs && (chunk_count || t->count))
        return bm_host(c, chunks.offset, "empty stsc for nonempty track");
    size_t sample = 0;
    uint32_t expected_chunk = 1;
    for (uint32_t i = 0; i < runs; ++i) {
        uint32_t first = (uint32_t)bm_get(&chunks, 4), per = (uint32_t)bm_get(&chunks, 4);
        uint32_t description = (uint32_t)bm_get(&chunks, 4);
        uint64_t next = i + 1 < runs ? bm_u32(chunks.data + chunks.pos) : (uint64_t)chunk_count + 1;
        if (first != expected_chunk || next <= first || next > (uint64_t)chunk_count + 1 || !per ||
            !description || description > t->entry_count ||
            (next - first) * per > t->count - sample)
            return bm_host(c, chunks.offset, "invalid stsc run or sample-description index");
        for (uint64_t j = first; j < next; ++j) {
            uint64_t offset = bm_get(&offsets, width);
            for (uint32_t k = 0; k < per; ++k) {
                qt_sample *s = &t->samples[sample++];
                s->offset = offset;
                s->description = description;
                if (!bm_in_mdat(c, offset, s->size))
                    return bm_host(c, offset, "sample bytes outside mdat payload");
                offset += s->size;
            }
        }
        expected_chunk = (uint32_t)next;
    }
    if (sample != t->count)
        return bm_host(c, chunks.offset, "stsc does not cover all samples");
    return SS_OK;
}
static ss_status_t qt_key(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    if (t->handler != BM_FOUR('m', 'e', 't', 'a'))
        return SS_OK;
    for (uint32_t e = t->entry; e != BM_NONE; e = c->boxes[e].next) {
        if (c->boxes[e].type != BM_FOUR('m', 'e', 'b', 'x'))
            continue;
        for (uint32_t keys = c->boxes[e].first; keys != BM_NONE; keys = c->boxes[keys].next) {
            if (c->boxes[keys].type != BM_FOUR('k', 'e', 'y', 's'))
                continue;
            BM_TRY(bm_children(c, keys, c->boxes[keys].payload,
                               c->boxes[keys].offset + c->boxes[keys].size, 9, false));
            size_t active = 0;
            for (uint32_t k = c->boxes[keys].first; k != BM_NONE; k = c->boxes[k].next) {
                ++active;
                if (++q->key_count > 65536)
                    return bm_limit(c, c->boxes[k].offset, "metadata key count limit");
                BM_TRY(bm_children(c, k, c->boxes[k].payload, c->boxes[k].offset + c->boxes[k].size,
                                   10, false));
                bool match = false;
                for (uint32_t d = c->boxes[k].first; d != BM_NONE; d = c->boxes[d].next) {
                    if (c->boxes[d].type != BM_FOUR('k', 'e', 'y', 'd'))
                        continue;
                    bm_cursor r;
                    BM_TRY(bm_load(c, d, &r));
                    if (r.size == 4 + strlen(SS_QT_METADATA_KEY) &&
                        bm_u32(r.data) == BM_FOUR('m', 'd', 't', 'a') &&
                        !memcmp(r.data + 4, SS_QT_METADATA_KEY, strlen(SS_QT_METADATA_KEY)))
                        match = true;
                }
                if (!match)
                    continue;
                if (t->candidate)
                    bm_check(c, false, c->boxes[k].offset, t->id,
                             "duplicate SpatialSnapshot metadata key");
                t->candidate = true;
                t->key = c->boxes[k].type;
                bm_check(c, t->entry_count == 1, c->boxes[e].offset, t->id,
                         "SSPS stsd must contain exactly one mebx");
                bm_check(c, bm_count(c, e, BM_FOUR('k', 'e', 'y', 's')) == 1, c->boxes[e].offset,
                         t->id, "mebx requires exactly one keys table");
                bm_check(c, t->key && t->key != UINT32_MAX, c->boxes[k].offset, t->id,
                         "reserved local key ID");
                bm_check(c, bm_count(c, k, BM_FOUR('k', 'e', 'y', 'd')) == 1, c->boxes[k].offset,
                         t->id, "keyd cardinality mismatch");
                bm_check(c, bm_count(c, k, BM_FOUR('d', 't', 'y', 'p')) == 1, c->boxes[k].offset,
                         t->id, "dtyp cardinality mismatch");
                bm_check(c, !bm_count(c, k, BM_FOUR('l', 'o', 'c', 'a')), c->boxes[k].offset, t->id,
                         "SSPS key has locale");
                bm_check(c, !bm_count(c, e, BM_FOUR('b', 't', 'r', 't')), c->boxes[e].offset, t->id,
                         "SSPS mebx contains btrt");
                uint32_t dtyp = bm_find(c, k, BM_FOUR('d', 't', 'y', 'p'));
                if (dtyp != BM_NONE) {
                    bm_cursor r;
                    BM_TRY(bm_load(c, dtyp, &r));
                    bool raw = r.size == 4 + strlen(SS_QT_METADATA_DATATYPE) &&
                               bm_u32(r.data) == 1 &&
                               !memcmp(r.data + 4, SS_QT_METADATA_DATATYPE,
                                       strlen(SS_QT_METADATA_DATATYPE));
                    bm_check(c, raw, r.offset, t->id,
                             "SSPS datatype must be exact raw-data declaration");
                    t->spatial = raw;
                }
            }
            if (t->candidate)
                bm_check(c, active == 1, c->boxes[keys].offset, t->id,
                         "SSPS keys must contain exactly one active key");
        }
    }
    return SS_OK;
}
static ss_status_t qt_parse(qt_file *q, bm_context *c) {
    memset(q, 0, sizeof(*q));
    q->c = c;
    uint32_t ftyp;
    bm_cursor r;
    /* Brand failures belong to the binding domain, independent of SSPS discovery. */
    size_t ftyp_count = bm_count(c, 0, BM_FOUR('f', 't', 'y', 'p'));
    bm_check(c, ftyp_count == 1, 0, 0, "QuickTime requires exactly one ftyp");
    ftyp = bm_find(c, 0, BM_FOUR('f', 't', 'y', 'p'));
    if (ftyp != BM_NONE) {
        BM_TRY(bm_load(c, ftyp, &r));
        if (r.size < 8 || r.size % 4)
            return bm_host(c, r.offset, "invalid ftyp length");
        bool brand = false;
        for (size_t i = 8; i < r.size; i += 4)
            brand |= bm_u32(r.data + i) == BM_FOUR('q', 't', ' ', ' ');
        bm_check(c, bm_u32(r.data) == BM_FOUR('q', 't', ' ', ' '), r.offset, 0,
                 "major brand is not qt");
        bm_check(c, brand, r.offset, 0, "qt compatible brand is missing");
    }
    BM_TRY(bm_unique(c, 0, BM_FOUR('m', 'o', 'o', 'v'), true, &q->moov));
    BM_TRY(bm_unique(c, q->moov, BM_FOUR('m', 'v', 'h', 'd'), true, &q->mvhd));
    BM_TRY(qt_header(q, q->mvhd, true, &q->timescale, &q->duration, NULL));
    bm_check(c, q->timescale == 1000000000, c->boxes[q->mvhd].offset, 0,
             "movie timescale is not 1 GHz");
    for (size_t i = 1; i < c->count; ++i)
        if (c->boxes[i].type == BM_FOUR('m', 'o', 'o', 'f') ||
            c->boxes[i].type == BM_FOUR('m', 'f', 'r', 'a') ||
            c->boxes[i].type == BM_FOUR('m', 'v', 'e', 'x'))
            bm_check(c, false, c->boxes[i].offset, 0, "fragmented movies are prohibited");
    q->count = bm_count(c, q->moov, BM_FOUR('t', 'r', 'a', 'k'));
    if (q->count > 1024)
        return bm_limit(c, 0, "track count exceeds 1024");
    if (!q->count)
        return bm_host(c, 0, "movie has no tracks");
    q->tracks = ssi_alloc(c->memory, q->count * sizeof(*q->tracks));
    if (!q->tracks)
        return c->memory->failure;
    memset(q->tracks, 0, q->count * sizeof(*q->tracks));
    size_t idx = 0;
    for (uint32_t b = c->boxes[q->moov].first; b != BM_NONE; b = c->boxes[b].next) {
        if (c->boxes[b].type != BM_FOUR('t', 'r', 'a', 'k'))
            continue;
        qt_track *t = &q->tracks[idx++];
        t->box = b;
        uint32_t handler;
        BM_TRY(bm_unique(c, b, BM_FOUR('t', 'k', 'h', 'd'), true, &t->tkhd));
        BM_TRY(qt_track_header(q, t));
        for (size_t i = 0; i + 1 < idx; ++i)
            if (q->tracks[i].id == t->id)
                return bm_host(c, c->boxes[b].offset, "duplicate track ID");
        BM_TRY(bm_unique(c, b, BM_FOUR('m', 'd', 'i', 'a'), true, &t->mdia));
        BM_TRY(bm_unique(c, b, BM_FOUR('t', 'r', 'e', 'f'), false, &t->tref));
        BM_TRY(bm_unique(c, t->mdia, BM_FOUR('m', 'd', 'h', 'd'), true, &t->mdhd));
        BM_TRY(qt_header(q, t->mdhd, false, &t->timescale, &t->duration, &t->language));
        BM_TRY(bm_unique(c, t->mdia, BM_FOUR('h', 'd', 'l', 'r'), true, &handler));
        BM_TRY(bm_load(c, handler, &r));
        if (r.size < 24)
            return bm_host(c, r.offset, "truncated track handler");
        t->handler = bm_u32(r.data + 8);
        BM_TRY(bm_unique(c, t->mdia, BM_FOUR('m', 'i', 'n', 'f'), true, &t->minf));
        BM_TRY(bm_unique(c, t->minf, BM_FOUR('s', 't', 'b', 'l'), true, &t->stbl));
        BM_TRY(bm_unique(c, t->stbl, BM_FOUR('s', 't', 's', 'd'), true, &t->stsd));
        BM_TRY(qt_description(q, t));
        BM_TRY(qt_key(q, t));
        BM_TRY(qt_data_references(q, t, t->candidate));
        BM_TRY(qt_sizes(q, t));
        BM_TRY(qt_times(q, t));
        BM_TRY(qt_offsets(q, t));
    }
    /* Validate the complete track-ID graph, including unrelated tracks. */
    for (size_t i = 0; i < q->count; ++i) {
        qt_track *t = &q->tracks[i];
        if (t->tref == BM_NONE)
            continue;
        size_t references = 0;
        for (uint32_t b = c->boxes[t->tref].first; b != BM_NONE; b = c->boxes[b].next) {
            BM_TRY(bm_load(c, b, &r));
            if (r.size % 4)
                return bm_host(c, r.offset, "track reference length is not a multiple of 4");
            references += r.size / 4;
            if (references > 65536)
                return bm_limit(c, r.offset, "track reference count limit");
            while (r.pos < r.size) {
                uint32_t id = (uint32_t)bm_get(&r, 4);
                if (!qt_find(q, id) || id == t->id)
                    return bm_host(c, r.offset, "dangling/self track reference");
            }
        }
    }
    return SS_OK;
}
static void qt_clear(qt_file *q) {
    if (q->tracks)
        for (size_t i = 0; i < q->count; ++i)
            ssi_free(q->tracks[i].samples);
    ssi_free(q->tracks);
}
static ss_status_t qt_track_constraints(qt_file *q, qt_track *t, bool meta) {
    bm_context *c = q->c;
    bm_cursor r;
    BM_TRY(bm_load(c, t->tkhd, &r));
    unsigned v = r.data[0];
    size_t matrix = v ? 52 : 40;
    bm_check(c, (t->flags & 7) == 7, r.offset, t->id,
             "track enabled/in-movie/in-preview flags must all be set");
    bm_check(c, !bm_u16(r.data + matrix - 8), r.offset, t->id, "track layer must be zero");
    bm_check(c, !bm_u16(r.data + matrix - 6), r.offset, t->id,
             "track alternate group must be zero");
    bm_check(c, qt_matrix(r.data + matrix), r.offset, t->id, "track matrix must be exact identity");
    bm_check(c,
             !bm_count(c, t->box, BM_FOUR('e', 'd', 't', 's')) &&
                 !bm_count(c, t->box, BM_FOUR('e', 'l', 's', 't')),
             r.offset, t->id, "bound track has edit list");
    bm_check(c, t->timescale == 1000000000, c->boxes[t->mdhd].offset, t->id,
             "bound track timescale is not 1 GHz");
    bm_check(c, t->entry_count == 1, c->boxes[t->stsd].offset, t->id,
             "bound stsd must have exactly one entry");
    uint32_t tw = bm_u32(r.data + matrix + 36), th = bm_u32(r.data + matrix + 40);
    if (meta) {
        bm_check(c, !tw && !th, r.offset, t->id, "metadata track dimensions are nonzero");
        bm_check(c, !bm_u16(r.data + matrix - 4), r.offset, t->id, "metadata volume is nonzero");
        static const uint32_t forbidden[] = {
            BM_FOUR('c', 't', 't', 's'), BM_FOUR('s', 't', 's', 's'), BM_FOUR('s', 'd', 't', 'p'),
            BM_FOUR('s', 'g', 'p', 'd'), BM_FOUR('s', 'b', 'g', 'p')};
        for (size_t i = 0; i < sizeof(forbidden) / sizeof(*forbidden); ++i)
            bm_check(c, !bm_count(c, t->stbl, forbidden[i]), c->boxes[t->stbl].offset, t->id,
                     "metadata has prohibited composition/sync/dependency table");
        bm_check(c,
                 bm_count(c, t->minf, BM_FOUR('g', 'm', 'h', 'd')) == 1 &&
                     !bm_count(c, t->minf, BM_FOUR('v', 'm', 'h', 'd')) &&
                     !bm_count(c, t->minf, BM_FOUR('s', 'm', 'h', 'd')),
                 c->boxes[t->minf].offset, t->id,
                 "metadata minf must contain gmhd and no vmhd/smhd");
        uint32_t gmhd = bm_find(c, t->minf, BM_FOUR('g', 'm', 'h', 'd'));
        bool one = bm_count(c, gmhd, BM_FOUR('g', 'm', 'i', 'n')) == 1;
        bm_check(c, one, c->boxes[t->minf].offset, t->id,
                 "metadata gmhd requires exactly one gmin");
        if (one) {
            BM_TRY(bm_load(c, bm_find(c, gmhd, BM_FOUR('g', 'm', 'i', 'n')), &r));
            bm_check(c, r.size == 16 && ssi_zero(r.data, r.size), r.offset, t->id,
                     "gmin fields must all be zero");
        }
        if (t->count > 10000000)
            return bm_limit(c, 0, "metadata sample count limit");
    } else {
        if (t->entry == BM_NONE)
            return SS_MALFORMED;
        BM_TRY(bm_load(c, t->entry, &r));
        t->width = bm_u16(r.data + 24);
        t->height = bm_u16(r.data + 26);
        if (!bm_check(c, t->width && t->height && t->width <= 16384 && t->height <= 16384, r.offset,
                      t->id, "video raster out of range"))
            return SS_MALFORMED;
        bm_check(c, tw == t->width << 16 && th == t->height << 16, r.offset, t->id,
                 "tkhd dimensions differ from full raster");
        bm_check(c, !bm_count(c, t->entry, BM_FOUR('c', 'l', 'a', 'p')), r.offset, t->id,
                 "video sample entry has clap");
        uint32_t pasp;
        BM_TRY(bm_unique(c, t->entry, BM_FOUR('p', 'a', 's', 'p'), false, &pasp));
        if (pasp != BM_NONE) {
            BM_TRY(bm_load(c, pasp, &r));
            if (r.size != 8)
                return bm_host(c, r.offset, "invalid video pasp length");
            bm_check(c, bm_u32(r.data) && bm_u32(r.data) == bm_u32(r.data + 4), r.offset, t->id,
                     "video pixels are not square");
        }
        bm_check(c,
                 !bm_count(c, t->box, BM_FOUR('t', 'a', 'p', 't')) &&
                     !bm_count(c, t->box, BM_FOUR('c', 'l', 'i', 'p')) &&
                     !bm_count(c, t->box, BM_FOUR('m', 'a', 't', 't')),
                 c->boxes[t->box].offset, t->id,
                 "video track has aperture, clip, or matte remapping");
    }
    return SS_OK;
}
static int sample_compare(const void *a, const void *b) {
    uint64_t x = ((const qt_sample *)a)->pts, y = ((const qt_sample *)b)->pts;
    return (x > y) - (x < y);
}
static ss_status_t qt_presentation(qt_file *q, qt_track *t) {
    bm_context *c = q->c;
    if (!t->count) {
        bm_check(c, false, 0, t->id, "bound video has no presentation frames");
        return SS_MALFORMED;
    }
    int64_t least = INT64_MAX, greatest = INT64_MIN;
    for (size_t i = 0; i < t->count; ++i) {
        qt_sample *s = &t->samples[i];
        uint64_t delta = s->pts >= s->dts ? s->pts - s->dts : s->dts - s->pts;
        if (delta > INT64_MAX)
            return bm_host(c, 0, "composition delta overflow");
        int64_t off = s->pts >= s->dts ? (int64_t)delta : -(int64_t)delta;
        if (off < least)
            least = off;
        if (off > greatest)
            greatest = off;
        if (i && s->pts < t->samples[i - 1].pts)
            t->reordered = true;
    }
    bool ctts = bm_count(c, t->stbl, BM_FOUR('c', 't', 't', 's')) != 0;
    bm_check(c, !ctts || t->reordered, c->boxes[t->stbl].offset, t->id,
             "ctts is present without frame reordering");
    qsort(t->samples, t->count, sizeof(*t->samples), sample_compare);
    bm_check(c, t->samples[0].pts == 0, 0, t->id, "first video presentation PTS is not zero");
    for (size_t i = 1; i < t->count; ++i)
        bm_check(c, t->samples[i - 1].pts < t->samples[i].pts, t->samples[i].offset, t->id,
                 "duplicate video presentation PTS");
    qt_sample *last = &t->samples[t->count - 1];
    if (last->pts > UINT64_MAX - last->duration) {
        bm_check(c, false, 0, t->id, "presentation end overflow");
        return SS_MALFORMED;
    }
    uint64_t end = last->pts + last->duration;
    uint32_t cslg;
    BM_TRY(bm_unique(c, t->stbl, BM_FOUR('c', 's', 'l', 'g'), false, &cslg));
    if (cslg != BM_NONE) {
        bm_cursor r;
        uint32_t vf;
        BM_TRY(qt_full(q, cslg, 1, &r, &vf));
        unsigned n = vf >> 24 ? 8 : 4;
        if (r.size != 4 + 5 * n)
            return bm_host(c, r.offset, "invalid cslg length");
        int64_t fields[5];
        for (unsigned i = 0; i < 5; ++i)
            fields[i] = n == 8 ? bm_i64(r.data + 4 + i * n) : bm_i32(r.data + 4 + i * n);
        bm_check(c,
                 fields[0] == 0 && fields[1] == least && fields[2] == greatest && fields[3] == 0 &&
                     end <= INT64_MAX && fields[4] == (int64_t)end,
                 r.offset, t->id, "cslg contradicts direct stts/ctts presentation timeline");
    }
    return SS_OK;
}
static bool qt_discover(qt_file *q, bool required) {
    size_t count = 0;
    for (size_t i = 0; i < q->count; ++i)
        if (q->tracks[i].candidate) {
            ++count;
            q->metadata = &q->tracks[i];
        }
    if (required || count)
        bm_check(q->c, count == 1, 0, 0,
                 "movie must have exactly one SpatialSnapshot metadata track");
    if (count != 1)
        return false;
    qt_track *m = q->metadata;
    bm_context *c = q->c;
    bool one = bm_count(c, m->tref, BM_FOUR('c', 'd', 's', 'c')) == 1;
    bm_check(c, one, c->boxes[m->box].offset, m->id, "metadata requires one cdsc track reference");
    if (!one)
        return false;
    bm_cursor r;
    if (bm_load(c, bm_find(c, m->tref, BM_FOUR('c', 'd', 's', 'c')), &r) != SS_OK)
        return false;
    if (!bm_check(c, r.size == 4, r.offset, m->id, "cdsc must contain exactly one track ID"))
        return false;
    m->referent = bm_u32(r.data);
    q->video = qt_find(q, m->referent);
    if (!bm_check(c, q->video && q->video->handler == BM_FOUR('v', 'i', 'd', 'e'), r.offset, m->id,
                  "metadata cdsc target is not a video track")) {
        q->video = NULL;
        return false;
    }
    return m->spatial;
}
static ss_status_t qt_correspondence(qt_file *q, const ss_document_t *doc) {
    bm_context *c = q->c;
    qt_track *v = q->video;
    bm_check(c, doc->info.kind == SS_VIDEO, 0, 0, "QuickTime binding requires SSPS VIDEO");
    bm_check(c, v->count == doc->frame_count, 0, v->id, "video/camera sample counts differ");
    bm_check(c, q->duration == doc->info.duration_ns, c->boxes[q->mvhd].offset, 0,
             "movie duration differs from SSPS");
    bm_check(c, v->track_duration == doc->info.duration_ns, c->boxes[v->tkhd].offset, v->id,
             "video tkhd duration differs from SSPS");
    bm_check(c, v->duration == doc->info.duration_ns && v->decode_duration == doc->info.duration_ns,
             c->boxes[v->mdhd].offset, v->id, "video mdhd/decode duration differs from SSPS");
    for (size_t i = 0; i < q->count; ++i)
        bm_check(c, q->tracks[i].track_duration <= doc->info.duration_ns, 0, q->tracks[i].id,
                 "track presentation exceeds movie duration");
    for (size_t i = 0; i < SSI_MIN(v->count, doc->frame_count); ++i) {
        const ss_camera_t *camera = &doc->frames[i].camera;
        bm_check(c, v->samples[i].pts == camera->timestamp_ns, v->samples[i].offset, v->id,
                 "video PTS and SSPS camera timestamp differ");
        bm_check(c, v->width == camera->raster_width && v->height == camera->raster_height, 0,
                 v->id, "video and SSPS raster dimensions differ");
    }
    if (v->count) {
        qt_sample *last = &v->samples[v->count - 1];
        bm_check(c,
                 last->pts < doc->info.duration_ns &&
                     last->duration == doc->info.duration_ns - last->pts,
                 last->offset, v->id, "last presented video frame does not end at SSPS duration");
    }
    return SS_OK;
}
static ss_status_t qt_extract(qt_file *q, ss_container_t *out) {
    bm_context *c = q->c;
    if (!qt_discover(q, true))
        return c->result != SS_OK ? c->result : SS_MALFORMED;
    qt_track *m = q->metadata, *v = q->video;
    BM_TRY(qt_track_constraints(q, m, true));
    BM_TRY(qt_track_constraints(q, v, false));
    BM_TRY(qt_presentation(q, v));
    if (!m->count) {
        bm_check(c, false, 0, m->id, "no metadata samples");
        return SS_MALFORMED;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < m->count; ++i) {
        qt_sample *s = &m->samples[i];
        uint8_t header[8];
        if (!bm_check(c, s->size >= 8, s->offset, m->id, "metadata sample lacks value atom"))
            return SS_MALFORMED;
        BM_TRY(bm_read(c, s->offset, header, 8));
        bool atom =
            bm_u32(header) >= 8 && bm_u32(header) == s->size && bm_u32(header + 4) == m->key;
        bm_check(c, atom, s->offset, m->id,
                 "metadata sample must be one correctly typed ordinary-size value atom");
        if (s->size - 8 > UINT64_C(1073741824))
            return bm_limit(c, s->offset, "packet bundle exceeds 1 GiB processing limit");
        if (s->size - 8 > SS_QT_MAX_RECONSTRUCTED_SSPS_BYTES - total)
            return bm_limit(c, s->offset, "reconstructed SSPS exceeds 128 GiB");
        total += s->size - 8;
        bm_check(c, s->duration > 0, s->offset, m->id, "metadata sample duration is zero");
    }
    if (total > SIZE_MAX)
        return bm_limit(c, 0, "SSPS exceeds address space");
    uint8_t *data = ssi_alloc(c->memory, (size_t)total);
    if (!data)
        return c->memory->failure;
    size_t pos = 0;
    for (size_t i = 0; i < m->count; ++i) {
        qt_sample *s = &m->samples[i];
        ss_status_t status = bm_read(c, s->offset + 8, data + pos, (size_t)s->size - 8);
        if (status != SS_OK) {
            ssi_free(data);
            return status;
        }
        pos += (size_t)s->size - 8;
    }
    BM_TRY(bm_parse_ssps(c, data, (size_t)total, m->samples[0].offset + 8, &out->document));
    const ss_document_t *doc = out->document;
    BM_TRY(qt_correspondence(q, doc));
    bm_check(c, m->count == doc->frame_count, 0, m->id, "metadata/camera sample counts differ");
    bm_check(c,
             m->duration == doc->info.duration_ns && m->track_duration == doc->info.duration_ns &&
                 m->decode_duration == doc->info.duration_ns,
             0, m->id, "metadata duration differs from SSPS");
    out->samples = ssi_alloc(c->memory, m->count * sizeof(*out->samples));
    if (!out->samples)
        return c->memory->failure;
    pos = 0;
    size_t packet = 0;
    for (size_t i = 0; i < m->count; ++i) {
        qt_sample *s = &m->samples[i];
        size_t end = pos + (size_t)s->size - 8;
        uint64_t expected_duration =
            i + 1 < m->count
                ? m->samples[i + 1].pts - s->pts
                : (doc->info.duration_ns >= s->pts ? doc->info.duration_ns - s->pts : 0);
        bm_check(c,
                 s->duration == expected_duration && expected_duration &&
                     s->pts < doc->info.duration_ns,
                 s->offset, m->id, "metadata presentation interval mismatch");
        if (i < doc->frame_count)
            bm_check(c, doc->frames[i].camera.timestamp_ns == s->pts, s->offset, m->id,
                     "metadata timestamp and camera differ");
        size_t cursor = i ? pos : ssi_u16(doc->data + 12);
        bool nonend = false;
        while (packet < doc->packet_count && doc->packets[packet].info.byte_offset < end) {
            const ss_packet_info_t *p = &doc->packets[packet++].info;
            uint64_t packet_end = p->byte_offset + p->header_size + p->stored_payload_size;
            bm_check(c, p->byte_offset == cursor && packet_end <= end, s->offset, m->id,
                     "SSPS packet is split by metadata sample boundary");
            bool is_end = p->type == SS_PACKET_STREAM_END;
            bm_check(c, is_end ? i + 1 == m->count : p->timestamp_ns == s->pts, s->offset, m->id,
                     "packet bundle contains a packet belonging to another timestamp");
            if (!is_end)
                nonend = true;
            cursor = (size_t)packet_end;
        }
        bm_check(c, cursor == end && nonend, s->offset, m->id,
                 "packet bundle is not an exact complete timestamp partition");
        ss_container_sample_t *sample = &out->samples[i];
        *sample = (ss_container_sample_t)SS_INIT(ss_container_sample_t);
        sample->timestamp_ns = s->pts;
        sample->duration_ns = s->duration;
        sample->bundle_offset = s->offset + 8;
        sample->bundle_size = s->size - 8;
        sample->ssps_offset = pos;
        if (i < v->count) {
            sample->media_offset = v->samples[i].offset;
            sample->media_size = v->samples[i].size;
            sample->media_decode_index = v->samples[i].decode_index;
        }
        pos = end;
    }
    if (c->result != SS_OK)
        return c->result;
    out->info.media_id = v->id;
    out->info.metadata_id = m->id;
    out->info.local_key_id = m->key;
    out->info.raster_width = v->width;
    out->info.raster_height = v->height;
    out->info.duration_ns = doc->info.duration_ns;
    out->info.sample_count = m->count;
    return SS_OK;
}
ss_status_t bm_qt_open(bm_context *c, ss_container_t *out) {
    qt_file q;
    ss_status_t s = qt_parse(&q, c);
    if (s == SS_OK)
        s = qt_extract(&q, out);
    qt_clear(&q);
    return s;
}

static ss_status_t qt_write_mvhd(ssi_buffer_t *out, uint64_t duration, uint32_t next_id) {
    uint8_t p[112] = {0};
    p[0] = 1;
    bm_w32(p + 20, 1000000000);
    bm_w64(p + 24, duration);
    bm_w32(p + 32, 65536);
    bm_w16(p + 36, 256);
    bm_w32(p + 48, 65536);
    bm_w32(p + 64, 65536);
    bm_w32(p + 80, 1073741824);
    bm_w32(p + 108, next_id);
    return bm_raw_box(out, BM_FOUR('m', 'v', 'h', 'd'), p, sizeof(p));
}
static ss_status_t qt_write_tkhd(ssi_buffer_t *out, uint32_t id, uint64_t duration, uint32_t w,
                                 uint32_t h) {
    uint8_t p[96] = {0};
    p[0] = 1;
    p[3] = 7;
    bm_w32(p + 20, id);
    bm_w64(p + 28, duration);
    bm_w32(p + 52, 65536);
    bm_w32(p + 68, 65536);
    bm_w32(p + 84, 1073741824);
    bm_w32(p + 88, w << 16);
    bm_w32(p + 92, h << 16);
    return bm_raw_box(out, BM_FOUR('t', 'k', 'h', 'd'), p, sizeof(p));
}
static ss_status_t qt_write_mdhd(ssi_buffer_t *out, uint64_t duration) {
    uint8_t p[36] = {0};
    p[0] = 1;
    bm_w32(p + 20, 1000000000);
    bm_w64(p + 24, duration);
    return bm_raw_box(out, BM_FOUR('m', 'd', 'h', 'd'), p, sizeof(p));
}
static ss_status_t qt_write_handler(ssi_buffer_t *out, uint32_t type) {
    uint8_t p[25] = {0};
    bm_w32(p + 4, BM_FOUR('m', 'h', 'l', 'r'));
    bm_w32(p + 8, type);
    return bm_raw_box(out, BM_FOUR('h', 'd', 'l', 'r'), p, sizeof(p));
}
static ss_status_t qt_write_dinf(ssi_buffer_t *out) {
    size_t dinf, dref, url;
    BM_TRY(bm_begin(out, BM_FOUR('d', 'i', 'n', 'f'), &dinf));
    BM_TRY(bm_full(out, BM_FOUR('d', 'r', 'e', 'f'), 0, &dref));
    BM_TRY(bm_put(out, 1, 4));
    BM_TRY(bm_full(out, BM_FOUR('u', 'r', 'l', ' '), 1, &url));
    BM_TRY(bm_end(out, url));
    BM_TRY(bm_end(out, dref));
    return bm_end(out, dinf);
}
static ss_status_t qt_write_mebx(ssi_buffer_t *out, uint32_t key) {
    size_t mebx, keys, local, keyd, dtyp;
    BM_TRY(bm_begin(out, BM_FOUR('m', 'e', 'b', 'x'), &mebx));
    BM_TRY(bm_put(out, 1, 8));
    BM_TRY(bm_begin(out, BM_FOUR('k', 'e', 'y', 's'), &keys));
    BM_TRY(bm_begin(out, key, &local));
    BM_TRY(bm_begin(out, BM_FOUR('k', 'e', 'y', 'd'), &keyd));
    BM_TRY(bm_put(out, BM_FOUR('m', 'd', 't', 'a'), 4));
    BM_TRY(ssi_buffer_append(out, SS_QT_METADATA_KEY, strlen(SS_QT_METADATA_KEY)));
    BM_TRY(bm_end(out, keyd));
    BM_TRY(bm_begin(out, BM_FOUR('d', 't', 'y', 'p'), &dtyp));
    BM_TRY(bm_put(out, 1, 4));
    BM_TRY(ssi_buffer_append(out, SS_QT_METADATA_DATATYPE, strlen(SS_QT_METADATA_DATATYPE)));
    BM_TRY(bm_end(out, dtyp));
    BM_TRY(bm_end(out, local));
    BM_TRY(bm_end(out, keys));
    return bm_end(out, mebx);
}
static ss_status_t qt_write_tables(ssi_buffer_t *out, const qt_sample *s, size_t n, bool video) {
    if (n > UINT32_MAX)
        return SS_BINDING_UNREPRESENTABLE;
    size_t stts;
    BM_TRY(bm_full(out, BM_FOUR('s', 't', 't', 's'), 0, &stts));
    size_t runs = 0;
    for (size_t i = 0; i < n; ++i)
        runs += !i || s[i].duration != s[i - 1].duration;
    BM_TRY(bm_put(out, runs, 4));
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && s[j].duration == s[i].duration)
            ++j;
        BM_TRY(bm_put(out, j - i, 4));
        BM_TRY(bm_put(out, s[i].duration, 4));
        i = j;
    }
    BM_TRY(bm_end(out, stts));
    bool offsets = false, all_sync = true;
    for (size_t i = 0; i < n; ++i) {
        offsets |= s[i].dts != s[i].pts;
        all_sync &= s[i].sync;
    }
    if (video && offsets) {
        size_t ctts;
        BM_TRY(bm_full(out, BM_FOUR('c', 't', 't', 's'), 0x01000000, &ctts));
        BM_TRY(bm_put(out, n, 4));
        for (size_t i = 0; i < n; ++i) {
            uint64_t delta = s[i].pts >= s[i].dts ? s[i].pts - s[i].dts : s[i].dts - s[i].pts;
            if (delta > (s[i].pts >= s[i].dts ? (uint64_t)INT32_MAX : UINT64_C(2147483648)))
                return SS_BINDING_UNREPRESENTABLE;
            int64_t signed_delta = s[i].pts >= s[i].dts ? (int64_t)delta : -(int64_t)delta;
            BM_TRY(bm_put(out, 1, 4));
            BM_TRY(bm_put(out, (uint32_t)signed_delta, 4));
        }
        BM_TRY(bm_end(out, ctts));
    }
    if (video && !all_sync) {
        size_t stss, sync = 0;
        for (size_t i = 0; i < n; ++i)
            sync += s[i].sync;
        BM_TRY(bm_full(out, BM_FOUR('s', 't', 's', 's'), 0, &stss));
        BM_TRY(bm_put(out, sync, 4));
        for (size_t i = 0; i < n; ++i)
            if (s[i].sync)
                BM_TRY(bm_put(out, i + 1, 4));
        BM_TRY(bm_end(out, stss));
    }
    size_t stsc;
    BM_TRY(bm_full(out, BM_FOUR('s', 't', 's', 'c'), 0, &stsc));
    BM_TRY(bm_put(out, n ? 1 : 0, 4));
    if (n) {
        BM_TRY(bm_put(out, 1, 4));
        BM_TRY(bm_put(out, 1, 4));
        BM_TRY(bm_put(out, 1, 4));
    }
    BM_TRY(bm_end(out, stsc));
    size_t stsz;
    BM_TRY(bm_full(out, BM_FOUR('s', 't', 's', 'z'), 0, &stsz));
    BM_TRY(bm_put(out, 0, 4));
    BM_TRY(bm_put(out, n, 4));
    for (size_t i = 0; i < n; ++i) {
        if (s[i].size > UINT32_MAX)
            return SS_BINDING_UNREPRESENTABLE;
        BM_TRY(bm_put(out, s[i].size, 4));
    }
    BM_TRY(bm_end(out, stsz));
    size_t co64;
    BM_TRY(bm_full(out, BM_FOUR('c', 'o', '6', '4'), 0, &co64));
    BM_TRY(bm_put(out, n, 4));
    for (size_t i = 0; i < n; ++i)
        BM_TRY(bm_put(out, s[i].offset, 8));
    return bm_end(out, co64);
}
static ss_status_t qt_write_track(ssi_buffer_t *out, uint32_t id, uint32_t video_id,
                                  uint64_t duration, uint32_t width, uint32_t height,
                                  const void *entry, size_t entry_size, const qt_sample *samples,
                                  size_t count, uint32_t key) {
    bool video = entry != NULL;
    size_t trak, mdia, minf, stbl, stsd;
    BM_TRY(bm_begin(out, BM_FOUR('t', 'r', 'a', 'k'), &trak));
    BM_TRY(qt_write_tkhd(out, id, duration, width, height));
    if (!video) {
        size_t tref, cdsc;
        BM_TRY(bm_begin(out, BM_FOUR('t', 'r', 'e', 'f'), &tref));
        BM_TRY(bm_begin(out, BM_FOUR('c', 'd', 's', 'c'), &cdsc));
        BM_TRY(bm_put(out, video_id, 4));
        BM_TRY(bm_end(out, cdsc));
        BM_TRY(bm_end(out, tref));
    }
    BM_TRY(bm_begin(out, BM_FOUR('m', 'd', 'i', 'a'), &mdia));
    BM_TRY(qt_write_mdhd(out, duration));
    BM_TRY(
        qt_write_handler(out, video ? BM_FOUR('v', 'i', 'd', 'e') : BM_FOUR('m', 'e', 't', 'a')));
    BM_TRY(bm_begin(out, BM_FOUR('m', 'i', 'n', 'f'), &minf));
    uint8_t neutral[16] = {0};
    if (video) {
        neutral[3] = 1;
        BM_TRY(bm_raw_box(out, BM_FOUR('v', 'm', 'h', 'd'), neutral, 12));
    } else {
        size_t gmhd;
        BM_TRY(bm_begin(out, BM_FOUR('g', 'm', 'h', 'd'), &gmhd));
        BM_TRY(bm_raw_box(out, BM_FOUR('g', 'm', 'i', 'n'), neutral, 16));
        BM_TRY(bm_end(out, gmhd));
    }
    BM_TRY(qt_write_dinf(out));
    BM_TRY(bm_begin(out, BM_FOUR('s', 't', 'b', 'l'), &stbl));
    BM_TRY(bm_full(out, BM_FOUR('s', 't', 's', 'd'), 0, &stsd));
    BM_TRY(bm_put(out, 1, 4));
    if (video)
        BM_TRY(ssi_buffer_append(out, entry, entry_size));
    else
        BM_TRY(qt_write_mebx(out, key));
    BM_TRY(bm_end(out, stsd));
    BM_TRY(qt_write_tables(out, samples, count, video));
    BM_TRY(bm_end(out, stbl));
    BM_TRY(bm_end(out, minf));
    BM_TRY(bm_end(out, mdia));
    return bm_end(out, trak);
}
static ss_status_t qt_bundles(bm_context *c, const ss_document_t *doc, uint32_t key,
                              ssi_buffer_t *out, qt_sample **samples) {
    if (doc->info.kind != SS_VIDEO) {
        bm_check(c, false, 0, 0, "QuickTime writer requires SSPS VIDEO");
        return SS_MALFORMED;
    }
    if (doc->frame_count > 10000000)
        return bm_limit(c, 0, "metadata sample count limit");
    if (doc->size > SS_QT_MAX_RECONSTRUCTED_SSPS_BYTES)
        return bm_limit(c, 0, "SSPS reconstruction size limit");
    qt_sample *s = ssi_alloc(c->memory, doc->frame_count * sizeof(*s));
    if (!s)
        return c->memory->failure;
    *samples = s;
    memset(s, 0, doc->frame_count * sizeof(*s));
    size_t packet = 0, begin = 0;
    for (size_t i = 0; i < doc->frame_count; ++i) {
        uint64_t time = doc->frames[i].camera.timestamp_ns;
        uint64_t next = i + 1 < doc->frame_count ? doc->frames[i + 1].camera.timestamp_ns
                                                 : doc->info.duration_ns;
        if (next <= time || next - time > UINT32_MAX)
            return bm_error(c, SS_DOMAIN_BINDING_UNREPRESENTABLE, SS_BINDING_UNREPRESENTABLE, 0, 0,
                            "camera interval cannot fit QuickTime stts duration");
        while (packet < doc->packet_count && doc->packets[packet].info.timestamp_ns == time)
            ++packet;
        if (packet < doc->packet_count && doc->packets[packet].info.type != SS_PACKET_STREAM_END &&
            doc->packets[packet].info.timestamp_ns != next)
            return bm_error(c, SS_DOMAIN_BINDING_UNREPRESENTABLE, SS_BINDING_UNREPRESENTABLE, 0, 0,
                            "SSPS extension timestamp has no corresponding camera/video frame");
        size_t end =
            i + 1 == doc->frame_count ? doc->size : (size_t)doc->packets[packet].info.byte_offset;
        size_t bundle = end - begin;
        if (bundle > UINT32_MAX - 8u)
            return bm_error(c, SS_DOMAIN_BINDING_UNREPRESENTABLE, SS_BINDING_UNREPRESENTABLE, 0, 0,
                            "packet bundle cannot fit ordinary 32-bit value atom");
        if (bundle > UINT64_C(1073741824))
            return bm_limit(c, 0, "packet bundle processing limit");
        s[i] = (qt_sample){.offset = out->size,
                           .size = bundle + 8,
                           .pts = time,
                           .dts = time,
                           .duration = (uint32_t)(next - time),
                           .description = 1,
                           .decode_index = (uint32_t)i,
                           .sync = true};
        BM_TRY(bm_put(out, bundle + 8, 4));
        BM_TRY(bm_put(out, key, 4));
        BM_TRY(ssi_buffer_append(out, doc->data + begin, bundle));
        begin = end;
    }
    return SS_OK;
}
static ss_status_t qt_copy_track(qt_file *q, qt_track *t, uint32_t removed_id, ssi_buffer_t *out) {
    bm_context *c = q->c;
    size_t trak;
    BM_TRY(bm_begin(out, BM_FOUR('t', 'r', 'a', 'k'), &trak));
    for (uint32_t b = c->boxes[t->box].first; b != BM_NONE; b = c->boxes[b].next) {
        if (b != t->tref || !removed_id) {
            size_t start = out->size;
            BM_TRY(bm_copy(c, out, c->boxes[b].offset, c->boxes[b].size));
            if (b == t->tkhd && t == q->video) {
                size_t payload = start + (size_t)(c->boxes[b].payload - c->boxes[b].offset);
                out->data[payload + 3] |= 7;
            }
            continue;
        }
        size_t tref;
        BM_TRY(bm_begin(out, BM_FOUR('t', 'r', 'e', 'f'), &tref));
        for (uint32_t ref = c->boxes[b].first; ref != BM_NONE; ref = c->boxes[ref].next) {
            bm_cursor r;
            BM_TRY(bm_load(c, ref, &r));
            size_t entry;
            BM_TRY(bm_begin(out, c->boxes[ref].type, &entry));
            while (r.pos < r.size) {
                uint32_t id = (uint32_t)bm_get(&r, 4);
                if (id != removed_id)
                    BM_TRY(bm_put(out, id, 4));
            }
            if (out->size == entry + 8)
                out->size = entry;
            else
                BM_TRY(bm_end(out, entry));
        }
        if (out->size == tref + 8)
            out->size = tref;
        else
            BM_TRY(bm_end(out, tref));
    }
    return bm_end(out, trak);
}
static ss_status_t qt_rewrite_impl(qt_file *q, uint32_t id, const ss_document_t *doc,
                                   ssi_buffer_t *out) {
    bm_context *c = q->c;
    bool exists = qt_discover(q, doc == NULL);
    if (c->result != SS_OK)
        return c->result;
    if (!doc && !exists)
        return SS_NOT_FOUND;
    uint32_t old_id = q->metadata ? q->metadata->id : 0;
    if (doc) {
        if (id)
            q->video = qt_find(q, id);
        else if (!q->video) {
            for (size_t i = 0; i < q->count; ++i)
                if (q->tracks[i].handler == BM_FOUR('v', 'i', 'd', 'e')) {
                    if (q->video)
                        return SS_INVALID_ARGUMENT;
                    q->video = &q->tracks[i];
                }
        }
        if (!q->video || q->video->handler != BM_FOUR('v', 'i', 'd', 'e'))
            return SS_INVALID_ARGUMENT;
        q->video->flags |= 7; /* Writer construction sets all required presentation flags. */
        BM_TRY(qt_track_constraints(q, q->video, false));
        BM_TRY(qt_presentation(q, q->video));
        BM_TRY(qt_correspondence(q, doc));
        if (c->result != SS_OK)
            return c->result;
    }
    uint32_t new_id = 1;
    while (qt_find(q, new_id))
        ++new_id;
    if (old_id)
        new_id = old_id;
    if (doc && q->count == 1024 && !old_id)
        return bm_limit(c, 0, "no room for metadata track");
    /* Preserve all original absolute chunk offsets. Replace only the old moov with free,
     * append new metadata mdat and a complete replacement moov. */
    BM_TRY(bm_copy(c, out, 0, c->size));
    for (uint32_t b = c->boxes[0].first; b != BM_NONE; b = c->boxes[b].next) {
        if (!bm_u32(out->data + c->boxes[b].offset)) {
            if (c->boxes[b].size > UINT32_MAX)
                return SS_BINDING_UNREPRESENTABLE;
            bm_w32(out->data + c->boxes[b].offset, (uint32_t)c->boxes[b].size);
        }
    }
    bm_box moov = c->boxes[q->moov];
    bm_w32(out->data + moov.offset + 4, BM_FOUR('f', 'r', 'e', 'e'));
    memset(out->data + moov.payload, 0, (size_t)(moov.offset + moov.size - moov.payload));
    qt_sample *metadata = NULL;
    ss_status_t status = SS_OK;
    if (doc) {
        size_t mdat = out->size;
        status = bm_put(out, 1, 4);
        if (status == SS_OK)
            status = bm_put(out, BM_FOUR('m', 'd', 'a', 't'), 4);
        if (status == SS_OK)
            status = bm_put(out, 0, 8);
        if (status == SS_OK)
            status = qt_bundles(c, doc, 1, out, &metadata);
        if (status != SS_OK) {
            ssi_free(metadata);
            return status;
        }
        bm_w64(out->data + mdat + 8, out->size - mdat);
    }
    size_t start;
    status = bm_begin(out, BM_FOUR('m', 'o', 'o', 'v'), &start);
    for (uint32_t b = c->boxes[q->moov].first; b != BM_NONE && status == SS_OK;
         b = c->boxes[b].next) {
        if (c->boxes[b].type == BM_FOUR('t', 'r', 'a', 'k')) {
            qt_track *t = NULL;
            for (size_t i = 0; i < q->count; ++i)
                if (q->tracks[i].box == b)
                    t = &q->tracks[i];
            if (t != q->metadata)
                status = qt_copy_track(q, t, old_id, out);
        } else {
            size_t offset = out->size;
            status = bm_copy(c, out, c->boxes[b].offset, c->boxes[b].size);
            if (status == SS_OK && b == q->mvhd && doc) {
                uint32_t next = new_id + 1;
                while (next && qt_find(q, next))
                    ++next;
                bm_w32(out->data + offset + c->boxes[b].size - 4, next);
            }
        }
    }
    if (status == SS_OK && doc)
        status = qt_write_track(out, new_id, q->video->id, doc->info.duration_ns, 0, 0, NULL, 0,
                                metadata, doc->frame_count, 1);
    if (status == SS_OK)
        status = bm_end(out, start);
    ssi_free(metadata);
    return status;
}
ss_status_t bm_qt_rewrite(bm_context *c, uint32_t id, const ss_document_t *doc, ssi_buffer_t *out) {
    qt_file q;
    ss_status_t s = qt_parse(&q, c);
    if (s == SS_OK)
        s = qt_rewrite_impl(&q, id, doc, out);
    qt_clear(&q);
    return s;
}
static ss_status_t qt_mux_impl(bm_context *c, const void *entry, size_t entry_size,
                               const ss_encoded_video_sample_t *samples, size_t count,
                               const ss_document_t *doc, ssi_buffer_t *out, qt_sample *video,
                               qt_sample **metadata) {
    const uint8_t *bytes = entry;
    if (entry_size < 86 || entry_size > UINT32_MAX || bm_u32(bytes) != entry_size ||
        bm_u16(bytes + 14) != 1)
        return SS_INVALID_ARGUMENT;
    if (count != doc->frame_count)
        return SS_INVALID_ARGUMENT;
    uint8_t ftyp[] = {'q', 't', ' ', ' ', 0, 0, 0, 0, 'q', 't', ' ', ' '};
    BM_TRY(bm_raw_box(out, BM_FOUR('f', 't', 'y', 'p'), ftyp, sizeof(ftyp)));
    size_t mdat = out->size;
    BM_TRY(bm_put(out, 1, 4));
    BM_TRY(bm_put(out, BM_FOUR('m', 'd', 'a', 't'), 4));
    BM_TRY(bm_put(out, 0, 8));
    uint64_t dts = 0;
    for (size_t i = 0; i < count; ++i) {
        const ss_encoded_video_sample_t *s = &samples[i];
        if (!SSI_VALID(s, ss_encoded_video_sample_t) || (!s->bytes && s->size) ||
            !s->decode_duration_ns || s->is_sync > 1)
            return SS_INVALID_ARGUMENT;
        if (s->size > UINT32_MAX || s->decode_duration_ns > UINT64_MAX - dts)
            return SS_BINDING_UNREPRESENTABLE;
        video[i] = (qt_sample){.offset = out->size,
                               .size = s->size,
                               .pts = s->presentation_timestamp_ns,
                               .dts = dts,
                               .duration = s->decode_duration_ns,
                               .description = 1,
                               .decode_index = (uint32_t)i,
                               .sync = !!s->is_sync};
        BM_TRY(ssi_buffer_append(out, s->bytes, s->size));
        dts += s->decode_duration_ns;
    }
    BM_TRY(qt_bundles(c, doc, 1, out, metadata));
    bm_w64(out->data + mdat + 8, out->size - mdat);
    size_t moov;
    BM_TRY(bm_begin(out, BM_FOUR('m', 'o', 'o', 'v'), &moov));
    BM_TRY(qt_write_mvhd(out, doc->info.duration_ns, 3));
    BM_TRY(qt_write_track(out, 1, 0, doc->info.duration_ns, bm_u16(bytes + 32), bm_u16(bytes + 34),
                          entry, entry_size, video, count, 0));
    BM_TRY(qt_write_track(out, 2, 1, doc->info.duration_ns, 0, 0, NULL, 0, *metadata, count, 1));
    return bm_end(out, moov);
}
ss_status_t bm_qt_mux(bm_context *c, const void *entry, size_t entry_size,
                      const ss_encoded_video_sample_t *samples, size_t count,
                      const ss_document_t *doc, ssi_buffer_t *out) {
    if (count > 10000000)
        return bm_limit(c, 0, "video/metadata sample count limit");
    qt_sample *v = ssi_alloc(c->memory, count * sizeof(*v)), *m = NULL;
    if (!v)
        return c->memory->failure;
    ss_status_t s = qt_mux_impl(c, entry, entry_size, samples, count, doc, out, v, &m);
    ssi_free(v);
    ssi_free(m);
    return s;
}

static ss_status_t qt_trim_impl(qt_file *q, ss_container_t *source, uint64_t start, uint64_t end,
                                const ss_writer_options_t *options, ssi_buffer_t *out) {
    bm_context *c = q->c;
    qt_track *v = q->video;
    if (start >= end || end > source->document->info.duration_ns ||
        ssi_find_frame(source->document, start) == SIZE_MAX)
        return SS_INVALID_ARGUMENT;
    if (q->count != 2 || bm_count(c, v->stbl, BM_FOUR('s', 'g', 'p', 'd')) ||
        bm_count(c, v->stbl, BM_FOUR('s', 'b', 'g', 'p')))
        return qt_unsupported(q, 0, v->id,
                              "portable trim requires one video track without dependency groups");
    size_t first = ssi_find_frame(source->document, start), count = 0;
    while (first + count < v->count && v->samples[first + count].pts < end) {
        if (!v->samples[first + count].sync)
            return bm_error(c, SS_DOMAIN_VIDEO_DECODER_UNAVAILABLE, SS_DECODER_UNAVAILABLE, 0,
                            v->id, "trim requires video decode/re-encode for inter-frame samples");
        ++count;
    }
    ss_writer_t *writer = NULL;
    ss_status_t status = ss_document_trim(source->document, start, end, options, &writer);
    if (status != SS_OK)
        return status;
    ss_document_t *doc = NULL;
    /* The writer owns these validated bytes until the end of this transaction. */
    status =
        ssi_document_parse(c->memory, writer->buffer.data, writer->buffer.size, false, &doc, NULL);
    qt_sample *samples = NULL, *metadata = NULL;
    if (status == SS_OK) {
        samples = ssi_alloc(c->memory, count * sizeof(*samples));
        if (!samples)
            status = c->memory->failure;
    }
    uint8_t ftyp[] = {'q', 't', ' ', ' ', 0, 0, 0, 0, 'q', 't', ' ', ' '};
    size_t mdat = 0, moov = 0;
    if (status == SS_OK)
        status = bm_raw_box(out, BM_FOUR('f', 't', 'y', 'p'), ftyp, sizeof(ftyp));
    if (status == SS_OK) {
        mdat = out->size;
        status = bm_put(out, 1, 4);
    }
    if (status == SS_OK)
        status = bm_put(out, BM_FOUR('m', 'd', 'a', 't'), 4);
    if (status == SS_OK)
        status = bm_put(out, 0, 8);
    for (size_t i = 0; i < count && status == SS_OK; ++i) {
        qt_sample s = v->samples[first + i];
        uint64_t next = i + 1 < count ? v->samples[first + i + 1].pts : end;
        samples[i] = s;
        samples[i].offset = out->size;
        samples[i].pts = samples[i].dts = s.pts - start;
        if (next <= s.pts || next - s.pts > UINT32_MAX) {
            status = SS_BINDING_UNREPRESENTABLE;
            break;
        }
        samples[i].duration = (uint32_t)(next - s.pts);
        status = bm_copy(c, out, s.offset, s.size);
    }
    if (status == SS_OK)
        status = qt_bundles(c, doc, 1, out, &metadata);
    if (status == SS_OK) {
        bm_w64(out->data + mdat + 8, out->size - mdat);
        status = bm_begin(out, BM_FOUR('m', 'o', 'o', 'v'), &moov);
    }
    if (status == SS_OK)
        status = qt_write_mvhd(out, end - start, 3);
    ssi_buffer_t entry = {.memory = c->memory};
    if (status == SS_OK)
        status = bm_copy(c, &entry, c->boxes[v->entry].offset, c->boxes[v->entry].size);
    if (status == SS_OK)
        status = qt_write_track(out, 1, 0, end - start, v->width, v->height, entry.data, entry.size,
                                samples, count, 0);
    if (status == SS_OK)
        status = qt_write_track(out, 2, 1, end - start, 0, 0, NULL, 0, metadata, count, 1);
    if (status == SS_OK)
        status = bm_end(out, moov);
    ssi_buffer_clear(&entry);
    ssi_free(samples);
    ssi_free(metadata);
    ss_document_release(doc);
    ss_writer_release(writer);
    return status;
}
ss_status_t bm_qt_trim(bm_context *c, uint64_t start, uint64_t end,
                       const ss_writer_options_t *options, ssi_buffer_t *out) {
    qt_file q;
    ss_status_t status = qt_parse(&q, c);
    ss_container_t source = {0};
    source.info = (ss_container_info_t)SS_INIT(ss_container_info_t);
    if (status == SS_OK)
        status = qt_extract(&q, &source);
    if (status == SS_OK)
        status = qt_trim_impl(&q, &source, start, end, options, out);
    ss_document_release(source.document);
    ssi_free(source.samples);
    qt_clear(&q);
    return status;
}
