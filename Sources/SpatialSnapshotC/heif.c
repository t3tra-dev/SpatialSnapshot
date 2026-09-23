#include "bmff.h"

typedef struct hf_extent {
    uint64_t offset, size;
} hf_extent;
typedef struct hf_ref {
    uint32_t from, to, type, order;
} hf_ref;
typedef struct hf_assoc {
    uint32_t property, next;
    bool essential;
} hf_assoc;
typedef struct hf_item {
    uint32_t id, type, box, flags, method, data_reference, protection;
    uint32_t assoc, width, height;
    size_t extent, extent_count, ref, ref_count;
    uint64_t size, output_offset;
    const char *name, *mime, *encoding;
    bool located, candidate, spatial, properties_checked, implicit_extent;
} hf_item;
typedef struct hf_file {
    bm_context *c;
    uint32_t meta, pitm, iinf, iloc, iref, iprp, idat, primary, spatial;
    hf_item *items;
    size_t count;
    hf_extent *extents;
    size_t extent_count, extent_capacity;
    hf_ref *refs;
    size_t ref_count, ref_capacity;
    hf_assoc *assocs;
    size_t assoc_count, assoc_capacity;
    uint32_t *properties;
    size_t property_count;
} hf_file;

ss_status_t bm_exif_orientation(bm_context *c, const uint8_t *p, size_t size, uint32_t id);
ss_status_t bm_xmp_orientation(bm_context *c, const uint8_t *p, size_t size, uint32_t id);

static ss_status_t hf_unsupported(hf_file *h, uint64_t offset, uint32_t id, const char *s) {
    return bm_error(h->c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, offset, id, s);
}
static int item_compare(const void *a, const void *b) {
    uint32_t x = ((const hf_item *)a)->id, y = ((const hf_item *)b)->id;
    return (x > y) - (x < y);
}
static int ref_compare(const void *a, const void *b) {
    const hf_ref *x = a, *y = b;
    if (x->from != y->from)
        return (x->from > y->from) - (x->from < y->from);
    if (x->type != y->type)
        return (x->type > y->type) - (x->type < y->type);
    return (x->order > y->order) - (x->order < y->order);
}
static hf_item *hf_find(hf_file *h, uint32_t id) {
    size_t lo = 0, hi = h->count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (h->items[m].id < id)
            lo = m + 1;
        else
            hi = m;
    }
    return lo < h->count && h->items[lo].id == id ? &h->items[lo] : NULL;
}
static ss_status_t hf_version(hf_file *h, bm_cursor *r, unsigned max, uint32_t *vf) {
    *vf = (uint32_t)bm_get(r, 4);
    if (r->failed)
        return bm_host(h->c, r->offset, "truncated FullBox header");
    if ((*vf >> 24) > max)
        return hf_unsupported(h, r->offset, 0, "unsupported HEIF box version");
    return SS_OK;
}
static ss_status_t hf_items(hf_file *h) {
    bm_context *c = h->c;
    bm_cursor r;
    uint32_t vf;
    BM_TRY(bm_load(c, h->iinf, &r));
    BM_TRY(hf_version(h, &r, 1, &vf));
    uint64_t n = bm_get(&r, vf >> 24 ? 4 : 2);
    if (n > 65536)
        return bm_limit(c, r.offset, "HEIF item count exceeds 65536");
    if (n != bm_count(c, h->iinf, BM_FOUR('i', 'n', 'f', 'e')))
        return bm_host(c, r.offset, "iinf entry count mismatch");
    h->items = ssi_alloc(c->memory, (size_t)n * sizeof(*h->items));
    if (!h->items)
        return c->memory->failure;
    memset(h->items, 0, (size_t)n * sizeof(*h->items));
    h->count = (size_t)n;
    size_t index = 0;
    for (uint32_t b = c->boxes[h->iinf].first; b != BM_NONE; b = c->boxes[b].next) {
        if (c->boxes[b].type != BM_FOUR('i', 'n', 'f', 'e'))
            return bm_host(c, c->boxes[b].offset, "non-infe iinf entry");
        BM_TRY(bm_load(c, b, &r));
        BM_TRY(hf_version(h, &r, 3, &vf));
        hf_item *it = &h->items[index++];
        it->box = b;
        it->flags = vf & 0xffffff;
        it->assoc = BM_NONE;
        it->id = (uint32_t)bm_get(&r, vf >> 24 == 3 ? 4 : 2);
        it->protection = (uint32_t)bm_get(&r, 2);
        it->name = it->mime = it->encoding = "";
        if ((vf >> 24) >= 2)
            it->type = (uint32_t)bm_get(&r, 4);
        size_t len;
        bm_string(&r, &it->name, &len);
        if ((vf >> 24) < 2 || it->type == BM_FOUR('m', 'i', 'm', 'e')) {
            bm_string(&r, &it->mime, &len);
            /* content_encoding is optional in the host format; absence means empty. */
            if (r.pos < r.size)
                bm_string(&r, &it->encoding, &len);
        } else if (it->type == BM_FOUR('u', 'r', 'i', ' ')) {
            const char *uri;
            bm_string(&r, &uri, &len);
        }
        if ((vf >> 24) >= 2)
            BM_TRY(bm_done(&r));
        else if (r.failed)
            return bm_host(c, r.offset, "truncated legacy infe");
        if (!it->id)
            return bm_host(c, r.offset, "zero item ID");
        it->candidate =
            it->type == BM_FOUR('m', 'i', 'm', 'e') && !strcmp(it->mime, SS_HEIF_CONTENT_TYPE);
        it->spatial = it->candidate && !strcmp(it->name, "SpatialSnapshot") && !*it->encoding &&
                      !it->protection && !(it->flags & 1);
    }
    qsort(h->items, h->count, sizeof(*h->items), item_compare);
    for (size_t i = 1; i < h->count; ++i)
        if (h->items[i - 1].id == h->items[i].id)
            return bm_host(c, 0, "duplicate item ID");
    return SS_OK;
}
static ss_status_t hf_locations(hf_file *h) {
    bm_context *c = h->c;
    bm_cursor r;
    uint32_t vf;
    BM_TRY(bm_load(c, h->iloc, &r));
    BM_TRY(hf_version(h, &r, 2, &vf));
    unsigned v = vf >> 24;
    uint32_t widths = (uint32_t)bm_get(&r, 2);
    unsigned os = widths >> 12, ls = (widths >> 8) & 15, bs = (widths >> 4) & 15;
    unsigned ix = v ? widths & 15 : 0;
    if ((os != 0 && os != 4 && os != 8) || (ls != 0 && ls != 4 && ls != 8) ||
        (bs != 0 && bs != 4 && bs != 8) || (ix != 0 && ix != 4 && ix != 8))
        return bm_host(c, r.offset, "invalid iloc integer widths");
    uint64_t n = bm_get(&r, v == 2 ? 4 : 2);
    if (n > 65536)
        return bm_limit(c, r.offset, "iloc item count limit");
    for (uint64_t i = 0; i < n && !r.failed; ++i) {
        uint32_t id = (uint32_t)bm_get(&r, v == 2 ? 4 : 2);
        hf_item *it = hf_find(h, id);
        if (!it || it->located)
            return bm_host(c, r.offset + r.pos, "iloc unknown or repeated item ID");
        it->located = true;
        uint32_t method = v ? (uint32_t)bm_get(&r, 2) : 0;
        if (method & 0xfff0)
            return bm_host(c, r.offset + r.pos, "iloc reserved bits are nonzero");
        it->method = method;
        it->data_reference = (uint32_t)bm_get(&r, 2);
        uint64_t base = bm_get(&r, bs);
        it->extent_count = (size_t)bm_get(&r, 2);
        it->extent = h->extent_count;
        if (it->extent_count > (r.size - r.pos) / (ix + os + ls ? ix + os + ls : 1))
            if (ix + os + ls)
                return bm_host(c, r.offset + r.pos, "truncated iloc extents");
        BM_TRY(ssi_grow(c->memory, (void **)&h->extents, &h->extent_capacity,
                        h->extent_count + it->extent_count, sizeof(*h->extents)));
        for (size_t j = 0; j < it->extent_count && !r.failed; ++j) {
            (void)bm_get(&r, ix);
            uint64_t off = bm_get(&r, os), len = bm_get(&r, ls);
            if (!len)
                it->implicit_extent = true;
            if (off > UINT64_MAX - base)
                return bm_host(c, r.offset + r.pos, "iloc base + offset overflow");
            off += base;
            if (!it->data_reference && method == 1) {
                if (h->idat == BM_NONE)
                    return bm_host(c, r.offset, "iloc method 1 without idat");
                bm_box *b = &c->boxes[h->idat];
                uint64_t available = b->offset + b->size - b->payload;
                if (off > available || len > available - off)
                    return bm_host(c, r.offset, "idat extent outside payload");
                if (!len)
                    len = available - off;
                off += b->payload;
            } else if (!it->data_reference && method == 0) {
                if (off > c->size || len > c->size - off)
                    return bm_host(c, r.offset, "iloc extent outside file");
                if (!len)
                    len = c->size - off;
            } else if (method > 2)
                return hf_unsupported(h, r.offset, id, "unknown item construction method");
            if (len > UINT64_MAX - it->size)
                return bm_host(c, r.offset, "item length sum overflow");
            h->extents[h->extent_count++] = (hf_extent){off, len};
            it->size += len;
        }
    }
    return bm_done(&r);
}
static ss_status_t hf_references(hf_file *h) {
    if (h->iref == BM_NONE)
        return SS_OK;
    bm_context *c = h->c;
    bm_cursor r;
    uint32_t vf;
    BM_TRY(bm_load(c, h->iref, &r));
    BM_TRY(hf_version(h, &r, 1, &vf));
    unsigned width = vf >> 24 ? 4 : 2;
    for (uint32_t b = c->boxes[h->iref].first; b != BM_NONE; b = c->boxes[b].next) {
        BM_TRY(bm_load(c, b, &r));
        uint32_t from = (uint32_t)bm_get(&r, width), count = (uint32_t)bm_get(&r, 2);
        if (!hf_find(h, from))
            return bm_host(c, r.offset, "reference source item is missing");
        if (count > 1048576 - h->ref_count)
            return bm_limit(c, r.offset, "HEIF reference count limit");
        BM_TRY(ssi_grow(c->memory, (void **)&h->refs, &h->ref_capacity, h->ref_count + count,
                        sizeof(*h->refs)));
        for (uint32_t i = 0; i < count && !r.failed; ++i) {
            uint32_t to = (uint32_t)bm_get(&r, width);
            if (!hf_find(h, to))
                return bm_host(c, r.offset, "reference target item is missing");
            h->refs[h->ref_count] = (hf_ref){from, to, c->boxes[b].type, (uint32_t)h->ref_count};
            ++h->ref_count;
        }
        BM_TRY(bm_done(&r));
    }
    if (h->ref_count)
        qsort(h->refs, h->ref_count, sizeof(*h->refs), ref_compare);
    for (size_t i = 0; i < h->ref_count; ++i) {
        hf_item *it = hf_find(h, h->refs[i].from);
        if (!it->ref_count)
            it->ref = i;
        ++it->ref_count;
    }
    return SS_OK;
}
static ss_status_t hf_properties(hf_file *h) {
    bm_context *c = h->c;
    if (h->iprp == BM_NONE)
        return SS_OK;
    uint32_t ipco;
    BM_TRY(bm_unique(c, h->iprp, BM_FOUR('i', 'p', 'c', 'o'), true, &ipco));
    for (uint32_t b = c->boxes[ipco].first; b != BM_NONE; b = c->boxes[b].next)
        ++h->property_count;
    if (h->property_count > 32767)
        return bm_host(c, c->boxes[ipco].offset, "property index cannot fit ipma");
    h->properties = ssi_alloc(c->memory, (h->property_count + 1) * sizeof(uint32_t));
    if (!h->properties)
        return c->memory->failure;
    size_t idx = 1;
    for (uint32_t b = c->boxes[ipco].first; b != BM_NONE; b = c->boxes[b].next)
        h->properties[idx++] = b;
    for (uint32_t b = c->boxes[h->iprp].first; b != BM_NONE; b = c->boxes[b].next) {
        if (c->boxes[b].type != BM_FOUR('i', 'p', 'm', 'a'))
            continue;
        bm_cursor r;
        uint32_t vf;
        BM_TRY(bm_load(c, b, &r));
        BM_TRY(hf_version(h, &r, 1, &vf));
        if (vf & 0xfffffe)
            return hf_unsupported(h, r.offset, 0, "unknown ipma flags");
        uint64_t count = bm_get(&r, 4);
        if (count > 65536)
            return bm_limit(c, r.offset, "ipma item count limit");
        uint32_t previous = 0;
        for (uint64_t j = 0; j < count && !r.failed; ++j) {
            uint32_t id = (uint32_t)bm_get(&r, vf >> 24 ? 4 : 2);
            hf_item *it = hf_find(h, id);
            if (!it || id <= previous)
                return bm_host(c, r.offset + r.pos, "ipma IDs missing, repeated, or unsorted");
            previous = id;
            uint32_t n = (uint32_t)bm_get(&r, 1);
            BM_TRY(ssi_grow(c->memory, (void **)&h->assocs, &h->assoc_capacity, h->assoc_count + n,
                            sizeof(*h->assocs)));
            for (uint32_t k = 0; k < n && !r.failed; ++k) {
                uint32_t v = (uint32_t)bm_get(&r, vf & 1 ? 2 : 1);
                uint32_t mask = vf & 1 ? 0x7fff : 0x7f, property = v & mask;
                if (property > h->property_count)
                    return bm_host(c, r.offset, "ipma references nonexistent property");
                if (!property) {
                    if (v & ~mask)
                        return bm_host(c, r.offset,
                                       "essential ipma association has property index zero");
                    continue;
                }
                h->assocs[h->assoc_count] = (hf_assoc){property, it->assoc, !!(v & ~mask)};
                it->assoc = (uint32_t)h->assoc_count++;
            }
        }
        BM_TRY(bm_done(&r));
    }
    uint8_t seen[4096];
    for (size_t i = 0; i < h->count; ++i) {
        memset(seen, 0, sizeof(seen));
        for (uint32_t a = h->items[i].assoc; a != BM_NONE; a = h->assocs[a].next) {
            uint32_t p = h->assocs[a].property;
            if (seen[p / 8] & (1u << (p % 8)))
                return bm_host(c, 0, "duplicate property association");
            seen[p / 8] |= (uint8_t)(1u << (p % 8));
        }
    }
    return SS_OK;
}
static ss_status_t hf_parse(hf_file *h, bm_context *c) {
    memset(h, 0, sizeof(*h));
    h->c = c;
    h->spatial = 0;
    uint32_t ftyp, handler;
    bm_cursor r;
    uint32_t vf;
    BM_TRY(bm_unique(c, 0, BM_FOUR('f', 't', 'y', 'p'), true, &ftyp));
    BM_TRY(bm_load(c, ftyp, &r));
    if (r.size < 8 || r.size % 4)
        return bm_host(c, r.offset, "invalid ftyp layout");
    BM_TRY(bm_unique(c, 0, BM_FOUR('m', 'e', 't', 'a'), true, &h->meta));
    uint8_t meta_header[4];
    BM_TRY(bm_read(c, c->boxes[h->meta].payload, meta_header, 4));
    vf = bm_u32(meta_header);
    if (vf)
        return hf_unsupported(h, c->boxes[h->meta].offset, 0, "unsupported meta version/flags");
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('h', 'd', 'l', 'r'), true, &handler));
    BM_TRY(bm_load(c, handler, &r));
    if (r.size < 24)
        return bm_host(c, r.offset, "truncated meta handler");
    bm_check(c, bm_u32(r.data + 8) == BM_FOUR('p', 'i', 'c', 't'), r.offset, 0,
             "root meta handler is not pict");
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('p', 'i', 't', 'm'), true, &h->pitm));
    BM_TRY(bm_load(c, h->pitm, &r));
    BM_TRY(hf_version(h, &r, 1, &vf));
    h->primary = (uint32_t)bm_get(&r, vf >> 24 ? 4 : 2);
    BM_TRY(bm_done(&r));
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('i', 'i', 'n', 'f'), true, &h->iinf));
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('i', 'l', 'o', 'c'), true, &h->iloc));
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('i', 'r', 'e', 'f'), false, &h->iref));
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('i', 'p', 'r', 'p'), false, &h->iprp));
    BM_TRY(bm_unique(c, h->meta, BM_FOUR('i', 'd', 'a', 't'), false, &h->idat));
    BM_TRY(hf_items(h));
    if (!hf_find(h, h->primary)) {
        bm_check(c, false, c->boxes[h->pitm].offset, h->primary, "primary image item is missing");
        return SS_MALFORMED;
    }
    BM_TRY(hf_locations(h));
    BM_TRY(hf_references(h));
    return hf_properties(h);
}
static void hf_clear(hf_file *h) {
    ssi_free(h->items);
    ssi_free(h->extents);
    ssi_free(h->refs);
    ssi_free(h->assocs);
    ssi_free(h->properties);
}
static bool hf_describes(hf_file *h, hf_item *it) {
    for (size_t j = it->ref; j < it->ref + it->ref_count; ++j)
        if (h->refs[j].type == BM_FOUR('c', 'd', 's', 'c') && h->refs[j].to == h->primary)
            return true;
    return false;
}
static bool hf_discover(hf_file *h, bool required) {
    size_t candidates = 0, found = 0;
    for (size_t i = 0; i < h->count; ++i) {
        hf_item *it = &h->items[i];
        if (!it->candidate)
            continue;
        ++candidates;
        bm_check(h->c, it->spatial, h->c->boxes[it->box].offset, it->id,
                 "SSPS infe name, encoding, protection, or hidden flag mismatch");
        size_t cdsc = 0;
        uint32_t target = 0;
        for (size_t j = it->ref; j < it->ref + it->ref_count; ++j)
            if (h->refs[j].type == BM_FOUR('c', 'd', 's', 'c')) {
                ++cdsc;
                target = h->refs[j].to;
            }
        bool relation = cdsc == 1 && target == h->primary;
        bm_check(h->c, relation, h->c->boxes[it->box].offset, it->id,
                 "SSPS outgoing cdsc must identify exactly the primary image");
        if (it->spatial && relation) {
            ++found;
            h->spatial = it->id;
        }
    }
    if (required || candidates)
        bm_check(h->c, found == 1 && candidates == 1, 0, 0, "HEIF requires exactly one SSPS item");
    return found == 1 && candidates == 1;
}
static bool hf_coded(uint32_t type) {
    switch (type) {
    case BM_FOUR('h', 'v', 'c', '1'):
    case BM_FOUR('h', 'e', 'v', '1'):
    case BM_FOUR('a', 'v', '0', '1'):
    case BM_FOUR('a', 'v', 'c', '1'):
    case BM_FOUR('a', 'v', 'c', '3'):
    case BM_FOUR('v', 'v', 'c', '1'):
    case BM_FOUR('v', 'v', 'i', '1'):
    case BM_FOUR('j', 'p', 'e', 'g'):
    case BM_FOUR('j', '2', 'k', '1'):
    case BM_FOUR('u', 'n', 'c', 'v'):
        return true;
    default:
        return false;
    }
}
static ss_status_t hf_raster(hf_file *h, hf_item *it, bool tile) {
    bm_context *c = h->c;
    if (!it->located || !it->size)
        return bm_host(c, 0, "image item has no payload location");
    if (it->properties_checked)
        return SS_OK;
    it->properties_checked = true;
    unsigned ispe_count = 0, config_count = 0;
    uint32_t config = 0;
    for (uint32_t a = it->assoc; a != BM_NONE; a = h->assocs[a].next) {
        uint32_t b = h->properties[h->assocs[a].property], type = c->boxes[b].type;
        /* Opaque optional properties never need payload allocation. */
        switch (type) {
        case BM_FOUR('c', 'o', 'l', 'r'):
        case BM_FOUR('p', 'i', 'x', 'i'):
        case BM_FOUR('a', 'u', 'x', 'C'):
        case BM_FOUR('c', 'l', 'l', 'i'):
        case BM_FOUR('m', 'd', 'c', 'v'):
        case BM_FOUR('j', 'p', 'g', 'C'):
        case BM_FOUR('c', 'm', 'p', 'd'):
        case BM_FOUR('u', 'd', 'e', 's'):
            continue;
        case BM_FOUR('i', 's', 'p', 'e'):
        case BM_FOUR('i', 'r', 'o', 't'):
        case BM_FOUR('i', 'm', 'i', 'r'):
        case BM_FOUR('r', 'l', 'o', 'c'):
        case BM_FOUR('c', 'l', 'a', 'p'):
        case BM_FOUR('p', 'a', 's', 'p'):
        case BM_FOUR('h', 'v', 'c', 'C'):
        case BM_FOUR('a', 'v', '1', 'C'):
        case BM_FOUR('a', 'v', 'c', 'C'):
        case BM_FOUR('v', 'v', 'c', 'C'):
        case BM_FOUR('j', '2', 'k', 'H'):
        case BM_FOUR('u', 'n', 'c', 'C'):
            break;
        default:
            if (h->assocs[a].essential)
                hf_unsupported(h, c->boxes[b].offset, it->id, "unknown essential image property");
            continue;
        }
        bm_cursor r;
        BM_TRY(bm_load(c, b, &r));
        switch (type) {
        case BM_FOUR('i', 's', 'p', 'e'):
            if (r.size != 12)
                return bm_host(c, r.offset, "invalid ispe length");
            if (bm_u32(r.data))
                return hf_unsupported(h, r.offset, it->id, "unknown ispe version/flags");
            it->width = bm_u32(r.data + 4);
            it->height = bm_u32(r.data + 8);
            ++ispe_count;
            break;
        case BM_FOUR('i', 'r', 'o', 't'):
        case BM_FOUR('i', 'm', 'i', 'r'):
        case BM_FOUR('r', 'l', 'o', 'c'):
            bm_check(c, false, r.offset, it->id, "image has a prohibited presentation transform");
            break;
        case BM_FOUR('c', 'l', 'a', 'p'):
            /* Check after dimensions have been discovered, below. */
            if (r.size != 32)
                return bm_host(c, r.offset, "invalid clap length");
            if (tile)
                bm_check(c, false, r.offset, it->id, "grid tile has clap");
            break;
        case BM_FOUR('p', 'a', 's', 'p'):
            if (r.size != 8)
                return bm_host(c, r.offset, "invalid pasp length");
            bm_check(c, bm_u32(r.data) && bm_u32(r.data) == bm_u32(r.data + 4), r.offset, it->id,
                     "image pixels are not square");
            break;
        case BM_FOUR('h', 'v', 'c', 'C'):
        case BM_FOUR('a', 'v', '1', 'C'):
        case BM_FOUR('a', 'v', 'c', 'C'):
        case BM_FOUR('v', 'v', 'c', 'C'):
        case BM_FOUR('j', '2', 'k', 'H'):
        case BM_FOUR('u', 'n', 'c', 'C'):
            BM_TRY(bm_codec_config(c, type, r.data, r.size, r.offset));
            config = type;
            ++config_count;
            break;
        case BM_FOUR('c', 'o', 'l', 'r'):
        case BM_FOUR('p', 'i', 'x', 'i'):
        case BM_FOUR('a', 'u', 'x', 'C'):
        case BM_FOUR('c', 'l', 'l', 'i'):
        case BM_FOUR('m', 'd', 'c', 'v'):
        case BM_FOUR('j', 'p', 'g', 'C'):
        case BM_FOUR('c', 'm', 'p', 'd'):
        case BM_FOUR('u', 'd', 'e', 's'):
            break;
        default:
            if (h->assocs[a].essential)
                hf_unsupported(h, r.offset, it->id, "unknown essential image property");
        }
    }
    bm_check(c, ispe_count == 1 && it->width && it->height, c->boxes[it->box].offset, it->id,
             "image requires exactly one nonempty ispe");
    for (uint32_t a = it->assoc; a != BM_NONE; a = h->assocs[a].next) {
        uint32_t b = h->properties[h->assocs[a].property];
        if (c->boxes[b].type != BM_FOUR('c', 'l', 'a', 'p'))
            continue;
        bm_cursor r;
        BM_TRY(bm_load(c, b, &r));
        bool identity = bm_u32(r.data + 4) && bm_u32(r.data + 12) && bm_u32(r.data + 20) &&
                        bm_u32(r.data + 28) &&
                        (uint64_t)it->width * bm_u32(r.data + 4) == bm_u32(r.data) &&
                        (uint64_t)it->height * bm_u32(r.data + 12) == bm_u32(r.data + 8) &&
                        !bm_u32(r.data + 16) && !bm_u32(r.data + 24);
        bm_check(c, identity && !tile, r.offset, it->id,
                 "clean aperture changes presentation raster");
    }
    uint32_t needed = 0;
    switch (it->type) {
    case BM_FOUR('h', 'v', 'c', '1'):
    case BM_FOUR('h', 'e', 'v', '1'):
        needed = BM_FOUR('h', 'v', 'c', 'C');
        break;
    case BM_FOUR('a', 'v', '0', '1'):
        needed = BM_FOUR('a', 'v', '1', 'C');
        break;
    case BM_FOUR('a', 'v', 'c', '1'):
    case BM_FOUR('a', 'v', 'c', '3'):
        needed = BM_FOUR('a', 'v', 'c', 'C');
        break;
    case BM_FOUR('v', 'v', 'c', '1'):
    case BM_FOUR('v', 'v', 'i', '1'):
        needed = BM_FOUR('v', 'v', 'c', 'C');
        break;
    case BM_FOUR('j', '2', 'k', '1'):
        needed = BM_FOUR('j', '2', 'k', 'H');
        break;
    case BM_FOUR('u', 'n', 'c', 'v'):
        needed = BM_FOUR('u', 'n', 'c', 'C');
        break;
    }
    if (needed && (config != needed || config_count != 1))
        return bm_host(c, c->boxes[it->box].offset,
                       "missing or inconsistent image decoder configuration");
    return SS_OK;
}
static ss_status_t hf_item_bytes(hf_file *h, hf_item *it, uint8_t **out) {
    if (!it->located || !it->size)
        return bm_host(h->c, 0, "image/metadata item has no data");
    if (it->method > 1 || it->data_reference)
        return hf_unsupported(h, 0, it->id,
                              "item data requires external or item-derived construction");
    if (it->size > SIZE_MAX)
        return bm_limit(h->c, 0, "item exceeds address space");
    uint8_t *p = ssi_alloc(h->c->memory, (size_t)it->size);
    if (!p)
        return h->c->memory->failure;
    size_t pos = 0;
    for (size_t i = it->extent; i < it->extent + it->extent_count; ++i) {
        hf_extent e = h->extents[i];
        ss_status_t s = bm_read(h->c, e.offset, p + pos, (size_t)e.size);
        if (s != SS_OK) {
            ssi_free(p);
            return s;
        }
        pos += (size_t)e.size;
    }
    *out = p;
    return SS_OK;
}
static ss_status_t hf_presentation(hf_file *h) {
    bm_context *c = h->c;
    hf_item *primary = hf_find(h, h->primary);
    bool grid = primary->type == BM_FOUR('g', 'r', 'i', 'd');
    if (!grid && !hf_coded(primary->type)) {
        if (primary->type == BM_FOUR('i', 'd', 'e', 'n') ||
            primary->type == BM_FOUR('i', 'o', 'v', 'l') ||
            primary->type == BM_FOUR('t', 'm', 'a', 'p') ||
            primary->type == BM_FOUR('m', 'i', 'm', 'e'))
            bm_check(c, false, 0, primary->id, "primary image is neither coded nor grid");
        else
            return hf_unsupported(h, 0, primary->id, "unknown primary image item semantics");
    }
    BM_TRY(hf_raster(h, primary, false));
    if (grid) {
        uint8_t *data = NULL;
        BM_TRY(hf_item_bytes(h, primary, &data));
        bool valid = primary->size >= 8 && data[0] == 0 && !(data[1] & 0xfe);
        uint32_t rows = 0, cols = 0, width = 0, height = 0;
        if (valid) {
            rows = (uint32_t)data[2] + 1;
            cols = (uint32_t)data[3] + 1;
            bool large = data[1] & 1;
            valid = primary->size == (large ? 12u : 8u);
            if (valid) {
                width = large ? bm_u32(data + 4) : bm_u16(data + 4);
                height = large ? bm_u32(data + 8) : bm_u16(data + 6);
            }
        }
        ssi_free(data);
        if (!valid)
            return bm_host(c, 0, "invalid grid image descriptor");
        bm_check(c, width == primary->width && height == primary->height, 0, primary->id,
                 "grid descriptor and ispe differ");
        uint32_t tw = 0, th = 0, tiles = 0;
        for (size_t j = primary->ref; j < primary->ref + primary->ref_count; ++j) {
            hf_ref ref = h->refs[j];
            if (ref.type != BM_FOUR('d', 'i', 'm', 'g'))
                continue;
            hf_item *tile = hf_find(h, ref.to);
            ++tiles;
            if (!bm_check(c, hf_coded(tile->type), 0, tile->id, "grid tile must be a coded image"))
                continue;
            BM_TRY(hf_raster(h, tile, true));
            if (!tw) {
                tw = tile->width;
                th = tile->height;
            }
            if (tile->width != tw || tile->height != th)
                return bm_host(c, 0, "grid tile dimensions differ");
        }
        if (tiles != rows * cols || width > (uint64_t)tw * cols || height > (uint64_t)th * rows ||
            width <= (uint64_t)tw * (cols - 1) || height <= (uint64_t)th * (rows - 1))
            return bm_host(c, 0, "grid dimensions or dimg tile count are inconsistent");
    }
    for (size_t i = 0; i < h->count; ++i) {
        hf_item *it = &h->items[i];
        if (!hf_describes(h, it))
            continue;
        bool exif = it->type == BM_FOUR('E', 'x', 'i', 'f');
        bool xmp =
            it->type == BM_FOUR('m', 'i', 'm', 'e') && !strcmp(it->mime, "application/rdf+xml");
        if (!exif && !xmp)
            continue;
        if (it->protection || *it->encoding)
            return hf_unsupported(h, 0, it->id,
                                  "cannot determine encoded/protected orientation metadata");
        uint8_t *p = NULL;
        BM_TRY(hf_item_bytes(h, it, &p));
        ss_status_t s = exif ? bm_exif_orientation(c, p, (size_t)it->size, it->id)
                             : bm_xmp_orientation(c, p, (size_t)it->size, it->id);
        ssi_free(p);
        if (s != SS_OK && s != SS_MALFORMED && s != SS_UNSUPPORTED)
            return s;
    }
    return SS_OK;
}
static ss_status_t hf_spatial_location(hf_file *h, hf_extent *extent) {
    bm_context *c = h->c;
    hf_item *it = hf_find(h, h->spatial);
    bool location = it->located && !it->data_reference && it->method == 0 &&
                    it->extent_count == 1 && it->size && !it->implicit_extent;
    bm_check(c, location, 0, it->id,
             "SSPS iloc must use one nonempty self-contained method-0 extent");
    if (!location)
        return SS_MALFORMED;
    hf_extent e = h->extents[it->extent];
    if (e.size > SS_HEIF_MAX_SSPS_BYTES)
        return bm_limit(c, e.offset, "HEIF SSPS exceeds 2 GiB");
    if (!bm_check(c, bm_in_mdat(c, e.offset, e.size), e.offset, it->id,
                  "SSPS extent is outside one mdat payload"))
        return SS_MALFORMED;
    for (size_t i = 0; i < h->count; ++i) {
        hf_item *other = &h->items[i];
        if (other == it)
            continue;
        if (other->method == 2)
            return hf_unsupported(
                h, 0, other->id, "cannot prove SSPS disjointness through item-offset construction");
        if (other->data_reference)
            continue;
        for (size_t j = other->extent; j < other->extent + other->extent_count; ++j) {
            hf_extent x = h->extents[j];
            bm_check(c, !x.size || x.offset >= e.offset + e.size || e.offset >= x.offset + x.size,
                     x.offset, other->id, "SSPS extent overlaps another item");
        }
    }
    *extent = e;
    return SS_OK;
}
static ss_status_t hf_extract(hf_file *h, ss_container_t *out) {
    bm_context *c = h->c;
    bool discovered = hf_discover(h, true);
    BM_TRY(hf_presentation(h));
    if (!discovered)
        return c->result;
    hf_item *it = hf_find(h, h->spatial), *primary = hf_find(h, h->primary);
    hf_extent e;
    BM_TRY(hf_spatial_location(h, &e));
    uint8_t *data = NULL;
    BM_TRY(hf_item_bytes(h, it, &data));
    BM_TRY(bm_parse_ssps(c, data, (size_t)it->size, e.offset, &out->document));
    bm_check(c, out->document->info.kind == SS_STILL, e.offset, it->id, "HEIF requires SSPS STILL");
    bm_check(c,
             out->document->frames[0].camera.raster_width == primary->width &&
                 out->document->frames[0].camera.raster_height == primary->height,
             e.offset, it->id, "HEIF and SSPS raster dimensions differ");
    if (c->result != SS_OK)
        return c->result;
    out->info.media_id = primary->id;
    out->info.metadata_id = it->id;
    out->info.raster_width = primary->width;
    out->info.raster_height = primary->height;
    out->info.sample_count = 1;
    out->samples = ssi_alloc(c->memory, sizeof(*out->samples));
    if (!out->samples)
        return c->memory->failure;
    out->samples[0] = (ss_container_sample_t)SS_INIT(ss_container_sample_t);
    out->samples[0].bundle_offset = e.offset;
    out->samples[0].bundle_size = e.size;
    /* A grid or multi-extent primary has no single encoded media range. */
    if (primary->extent_count == 1 && primary->type != BM_FOUR('g', 'r', 'i', 'd')) {
        out->samples[0].media_offset = h->extents[primary->extent].offset;
        out->samples[0].media_size = h->extents[primary->extent].size;
    }
    return SS_OK;
}
ss_status_t bm_heif_open(bm_context *c, ss_container_t *out) {
    hf_file h;
    ss_status_t s = hf_parse(&h, c);
    if (s == SS_OK)
        s = hf_extract(&h, out);
    hf_clear(&h);
    return s;
}

static ss_status_t hf_write_refs(hf_file *h, const ss_document_t *doc, uint32_t new_id,
                                 ssi_buffer_t *out) {
    size_t start;
    BM_TRY(bm_full(out, BM_FOUR('i', 'r', 'e', 'f'), 0x01000000, &start));
    for (size_t i = 0; i < h->ref_count;) {
        size_t end = i + 1;
        while (end < h->ref_count && h->refs[end].from == h->refs[i].from &&
               h->refs[end].type == h->refs[i].type)
            ++end;
        size_t n = 0;
        for (size_t j = i; j < end; ++j)
            n += h->refs[j].from != h->spatial && h->refs[j].to != h->spatial;
        if (n) {
            if (n > UINT16_MAX)
                return SS_BINDING_UNREPRESENTABLE;
            size_t ref;
            BM_TRY(bm_begin(out, h->refs[i].type, &ref));
            BM_TRY(bm_put(out, h->refs[i].from, 4));
            BM_TRY(bm_put(out, n, 2));
            for (size_t j = i; j < end; ++j)
                if (h->refs[j].from != h->spatial && h->refs[j].to != h->spatial)
                    BM_TRY(bm_put(out, h->refs[j].to, 4));
            BM_TRY(bm_end(out, ref));
        }
        i = end;
    }
    if (doc) {
        size_t ref;
        BM_TRY(bm_begin(out, BM_FOUR('c', 'd', 's', 'c'), &ref));
        BM_TRY(bm_put(out, new_id, 4));
        BM_TRY(bm_put(out, 1, 2));
        BM_TRY(bm_put(out, h->primary, 4));
        BM_TRY(bm_end(out, ref));
    }
    return bm_end(out, start);
}
static ss_status_t hf_write_properties(hf_file *h, ssi_buffer_t *out) {
    if (h->iprp == BM_NONE)
        return SS_OK;
    size_t start;
    BM_TRY(bm_begin(out, BM_FOUR('i', 'p', 'r', 'p'), &start));
    for (uint32_t b = h->c->boxes[h->iprp].first; b != BM_NONE; b = h->c->boxes[b].next) {
        if (h->c->boxes[b].type == BM_FOUR('i', 'p', 'm', 'a'))
            continue;
        BM_TRY(bm_copy(h->c, out, h->c->boxes[b].offset, h->c->boxes[b].size));
    }
    size_t ipma;
    BM_TRY(bm_full(out, BM_FOUR('i', 'p', 'm', 'a'), 0x01000001, &ipma));
    uint32_t count = 0;
    for (size_t i = 0; i < h->count; ++i)
        count += h->items[i].id != h->spatial && h->items[i].assoc != BM_NONE;
    BM_TRY(bm_put(out, count, 4));
    for (size_t i = 0; i < h->count; ++i) {
        hf_item *it = &h->items[i];
        if (it->id == h->spatial || it->assoc == BM_NONE)
            continue;
        uint32_t associations[255];
        size_t n = 0;
        for (uint32_t a = it->assoc; a != BM_NONE; a = h->assocs[a].next) {
            if (n == 255)
                return SS_BINDING_UNREPRESENTABLE;
            associations[n++] = h->assocs[a].property | (h->assocs[a].essential ? 0x8000u : 0);
        }
        BM_TRY(bm_put(out, it->id, 4));
        BM_TRY(bm_put(out, n, 1));
        /* Associations were prepended during parse; restore original property order. */
        while (n)
            BM_TRY(bm_put(out, associations[--n], 2));
    }
    BM_TRY(bm_end(out, ipma));
    return bm_end(out, start);
}
/* ImageIO emits irot=0 even for a newly encoded, unrotated raster. A writer may
 * remove that no-op association before attaching SSPS. Readers remain strict;
 * an already bound input is never repaired, and nonzero/reserved rotations are
 * left for the normal presentation validation to reject. Unassociated ipco
 * properties are harmless and keep all remaining property indices stable. */
static ss_status_t hf_remove_identity_rotations(hf_file *h) {
    for (size_t i = 0; i < h->count; ++i) {
        uint32_t *link = &h->items[i].assoc;
        while (*link != BM_NONE) {
            hf_assoc *a = &h->assocs[*link];
            uint32_t box = h->properties[a->property];
            if (h->c->boxes[box].type == BM_FOUR('i', 'r', 'o', 't')) {
                bm_cursor r;
                BM_TRY(bm_load(h->c, box, &r));
                if (r.size == 1 && r.data[0] == 0) {
                    *link = a->next;
                    continue;
                }
            }
            link = &a->next;
        }
    }
    return SS_OK;
}
static ss_status_t hf_rewrite_impl(hf_file *h, const ss_document_t *doc, ssi_buffer_t *out) {
    bm_context *c = h->c;
    hf_discover(h, doc == NULL);
    if (doc && !h->spatial && c->result == SS_OK)
        BM_TRY(hf_remove_identity_rotations(h));
    BM_TRY(hf_presentation(h));
    if (h->spatial) {
        hf_extent existing;
        BM_TRY(hf_spatial_location(h, &existing));
    }
    if (c->result != SS_OK)
        return c->result;
    hf_item *primary = hf_find(h, h->primary);
    if (doc) {
        bm_check(c, doc->info.kind == SS_STILL, 0, 0, "HEIF writer requires SSPS STILL");
        bm_check(c,
                 doc->frames[0].camera.raster_width == primary->width &&
                     doc->frames[0].camera.raster_height == primary->height,
                 0, primary->id, "source image and SSPS raster differ");
        if (doc->size > SS_HEIF_MAX_SSPS_BYTES)
            return bm_limit(c, 0, "HEIF SSPS exceeds 2 GiB");
        if (c->result != SS_OK)
            return c->result;
    }
    /* Groups and movie tracks can carry item offsets/references outside iloc/iref. Until a
     * rewrite rule is known, fail atomically instead of copying a potentially stale graph. */
    if (bm_find(c, 0, BM_FOUR('m', 'o', 'o', 'v')) != BM_NONE ||
        bm_find(c, h->meta, BM_FOUR('g', 'r', 'p', 'l')) != BM_NONE)
        return hf_unsupported(h, 0, 0,
                              "HEIF rewrite of movie/entity-group relationships is unsupported");
    uint32_t new_id = h->spatial;
    if (doc && !new_id) {
        if (h->count == 65536)
            return bm_limit(c, 0, "no room for SSPS item");
        new_id = 1;
        while (hf_find(h, new_id))
            ++new_id;
    }
    for (uint32_t b = c->boxes[0].first; b != BM_NONE; b = c->boxes[b].next) {
        if (b == h->meta || c->boxes[b].type == BM_FOUR('m', 'd', 'a', 't') ||
            c->boxes[b].type == BM_FOUR('f', 'r', 'e', 'e') ||
            c->boxes[b].type == BM_FOUR('s', 'k', 'i', 'p'))
            continue;
        BM_TRY(bm_copy(c, out, c->boxes[b].offset, c->boxes[b].size));
    }
    /* Always use an extended mdat so every host-sized item remains representable. */
    size_t mdat = out->size;
    BM_TRY(bm_put(out, 1, 4));
    BM_TRY(bm_put(out, BM_FOUR('m', 'd', 'a', 't'), 4));
    BM_TRY(bm_put(out, 0, 8));
    for (size_t i = 0; i < h->count; ++i) {
        hf_item *it = &h->items[i];
        if (it->id == h->spatial)
            continue;
        if (it->method > 1 || it->data_reference || it->protection)
            return hf_unsupported(h, 0, it->id, "cannot remux protected/external/item-offset data");
        it->output_offset = out->size;
        for (size_t j = it->extent; j < it->extent + it->extent_count; ++j)
            BM_TRY(bm_copy(c, out, h->extents[j].offset, h->extents[j].size));
    }
    uint64_t ssps_offset = out->size;
    if (doc)
        BM_TRY(ssi_buffer_append(out, doc->data, doc->size));
    bm_w64(out->data + mdat + 8, out->size - mdat);
    size_t meta;
    BM_TRY(bm_full(out, BM_FOUR('m', 'e', 't', 'a'), 0, &meta));
    for (uint32_t b = c->boxes[h->meta].first; b != BM_NONE; b = c->boxes[b].next) {
        if (b == h->iinf || b == h->iloc || b == h->iref || b == h->idat || b == h->iprp)
            continue;
        BM_TRY(bm_copy(c, out, c->boxes[b].offset, c->boxes[b].size));
    }
    size_t iinf;
    BM_TRY(bm_full(out, BM_FOUR('i', 'i', 'n', 'f'), 0x01000000, &iinf));
    BM_TRY(bm_put(out, h->count - (h->spatial ? 1u : 0u) + (doc ? 1u : 0u), 4));
    for (size_t i = 0; i < h->count; ++i)
        if (h->items[i].id != h->spatial) {
            bm_box b = c->boxes[h->items[i].box];
            BM_TRY(bm_copy(c, out, b.offset, b.size));
        }
    if (doc) {
        size_t infe;
        BM_TRY(bm_full(out, BM_FOUR('i', 'n', 'f', 'e'), 0x03000000, &infe));
        BM_TRY(bm_put(out, new_id, 4));
        BM_TRY(bm_put(out, 0, 2));
        BM_TRY(bm_put(out, BM_FOUR('m', 'i', 'm', 'e'), 4));
        BM_TRY(ssi_buffer_append(out, "SpatialSnapshot", sizeof("SpatialSnapshot")));
        BM_TRY(ssi_buffer_append(out, SS_HEIF_CONTENT_TYPE, sizeof(SS_HEIF_CONTENT_TYPE)));
        BM_TRY(bm_put(out, 0, 1));
        BM_TRY(bm_end(out, infe));
    }
    BM_TRY(bm_end(out, iinf));
    size_t iloc;
    BM_TRY(bm_full(out, BM_FOUR('i', 'l', 'o', 'c'), 0x02000000, &iloc));
    BM_TRY(bm_put(out, 0x8800, 2));
    size_t locations = doc ? 1 : 0;
    for (size_t i = 0; i < h->count; ++i)
        locations += h->items[i].id != h->spatial && h->items[i].located;
    BM_TRY(bm_put(out, locations, 4));
    for (size_t i = 0; i < h->count + (doc ? 1u : 0u); ++i) {
        hf_item *it = i < h->count ? &h->items[i] : NULL;
        if (it && (it->id == h->spatial || !it->located))
            continue;
        BM_TRY(bm_put(out, it ? it->id : new_id, 4));
        BM_TRY(bm_put(out, 0, 2));
        BM_TRY(bm_put(out, 0, 2));
        bool nonempty = !it || it->size;
        BM_TRY(bm_put(out, nonempty ? 1 : 0, 2));
        if (nonempty) {
            BM_TRY(bm_put(out, it ? it->output_offset : ssps_offset, 8));
            BM_TRY(bm_put(out, it ? it->size : doc->size, 8));
        }
    }
    BM_TRY(bm_end(out, iloc));
    BM_TRY(hf_write_refs(h, doc, new_id, out));
    BM_TRY(hf_write_properties(h, out));
    return bm_end(out, meta);
}
ss_status_t bm_heif_rewrite(bm_context *c, const ss_document_t *doc, ssi_buffer_t *out) {
    hf_file h;
    ss_status_t s = hf_parse(&h, c);
    if (s == SS_OK)
        s = hf_rewrite_impl(&h, doc, out);
    hf_clear(&h);
    return s;
}
