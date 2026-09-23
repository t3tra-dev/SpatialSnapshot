#include "bmff.h"

uint16_t bm_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
uint32_t bm_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint64_t bm_u64(const uint8_t *p) {
    return (uint64_t)bm_u32(p) << 32 | bm_u32(p + 4);
}
int64_t bm_i32(const uint8_t *p) {
    uint32_t u = bm_u32(p);
    return u <= INT32_MAX ? (int64_t)u : -1 - (int64_t)(UINT32_MAX - u);
}
int64_t bm_i64(const uint8_t *p) {
    uint64_t u = bm_u64(p);
    return u <= INT64_MAX ? (int64_t)u : -1 - (int64_t)(UINT64_MAX - u);
}
void bm_w16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}
void bm_w32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
void bm_w64(uint8_t *p, uint64_t n) {
    bm_w32(p, (uint32_t)(n >> 32));
    bm_w32(p + 4, (uint32_t)n);
}

void ss_binding_options_init(ss_binding_options_t *o) {
    if (!o)
        return;
    *o = (ss_binding_options_t)SS_INIT(ss_binding_options_t);
    ss_open_options_init(&o->resources);
}
const char *ss_binding_domain_string(ss_binding_domain_t d) {
    static const char *const names[] = {"NONE",
                                        "HEIF_MALFORMED",
                                        "QUICKTIME_MALFORMED",
                                        "BINDING_MALFORMED",
                                        "BINDING_UNREPRESENTABLE",
                                        "SSPS_MALFORMED",
                                        "UNSUPPORTED_BINDING",
                                        "UNSUPPORTED_SSPS",
                                        "RESOURCE_LIMIT",
                                        "IMAGE_DECODER_UNAVAILABLE",
                                        "VIDEO_DECODER_UNAVAILABLE",
                                        "IO_ERROR"};
    return d < sizeof(names) / sizeof(*names) ? names[d] : "UNKNOWN_DOMAIN";
}
ss_status_t bm_error(bm_context *c, ss_binding_domain_t domain, ss_status_t status, uint64_t offset,
                     uint32_t id, const char *message) {
    if (c->result == SS_OK)
        c->result = status;
    if (c->options.diagnostic) {
        ss_binding_diagnostic_t d = SS_INIT(ss_binding_diagnostic_t);
        d.domain = domain;
        d.status = status;
        d.file_offset = offset;
        d.ssps_offset = UINT64_MAX;
        d.entity_id = id;
        size_t n = strlen(message);
        memcpy(d.message, message, SSI_MIN(n, sizeof(d.message) - 1));
        c->options.diagnostic(c->options.diagnostic_context, &d);
    }
    return status;
}
bool bm_check(bm_context *c, bool condition, uint64_t offset, uint32_t id, const char *message) {
    if (!condition)
        bm_error(c, SS_DOMAIN_BINDING_MALFORMED, SS_MALFORMED, offset, id, message);
    return condition;
}
ss_status_t bm_host(bm_context *c, uint64_t offset, const char *message) {
    return bm_error(c, c->host_domain, SS_MALFORMED, offset, 0, message);
}
ss_status_t bm_limit(bm_context *c, uint64_t offset, const char *message) {
    return bm_error(c, SS_DOMAIN_RESOURCE_LIMIT, SS_RESOURCE_LIMIT, offset, 0, message);
}
ss_status_t bm_read(bm_context *c, uint64_t offset, void *data, size_t size) {
    if (offset > c->size || size > c->size - offset)
        return bm_host(c, offset, "byte range outside host file");
    if (!size)
        return SS_OK;
    ss_status_t s = c->io.read_at(c->io.context, offset, data, size);
    if (s != SS_OK)
        return bm_error(c, SS_DOMAIN_IO_ERROR, SS_IO_ERROR, offset, 0, "host read failed");
    return SS_OK;
}
ss_status_t bm_load(bm_context *c, uint32_t index, bm_cursor *r) {
    if (index == BM_NONE || index >= c->count)
        return bm_host(c, 0, "missing required box");
    bm_box *b = &c->boxes[index];
    uint64_t n = b->offset + b->size - b->payload;
    if (n > SIZE_MAX)
        return bm_limit(c, b->offset, "box payload exceeds address space");
    if (!b->data) {
        b->data = ssi_alloc(c->memory, (size_t)n);
        if (!b->data)
            return bm_error(c, SS_DOMAIN_RESOURCE_LIMIT, c->memory->failure, b->offset, 0,
                            "box allocation limit");
        BM_TRY(bm_read(c, b->payload, b->data, (size_t)n));
    }
    *r = (bm_cursor){c, b->data, (size_t)n, 0, b->payload, false};
    return SS_OK;
}
const uint8_t *bm_take(bm_cursor *r, size_t n) {
    if (r->failed || n > r->size - r->pos) {
        r->failed = true;
        return NULL;
    }
    const uint8_t *p = r->data + r->pos;
    r->pos += n;
    return p;
}
uint64_t bm_get(bm_cursor *r, unsigned n) {
    if (n > 8) {
        r->failed = true;
        return 0;
    }
    const uint8_t *p = bm_take(r, n);
    uint64_t v = 0;
    if (p)
        for (unsigned i = 0; i < n; ++i)
            v = v << 8 | p[i];
    return v;
}
bool bm_string(bm_cursor *r, const char **s, size_t *n) {
    if (r->failed)
        return false;
    const uint8_t *p = r->data + r->pos;
    const uint8_t *end = memchr(p, 0, r->size - r->pos);
    if (!end) {
        r->failed = true;
        return false;
    }
    *s = (const char *)p;
    *n = (size_t)(end - p);
    r->pos += *n + 1;
    return true;
}
ss_status_t bm_done(bm_cursor *r) {
    return r->failed || r->pos != r->size ? bm_host(r->ctx, r->offset + r->pos,
                                                    "truncated fields or unexpected trailing bytes")
                                          : SS_OK;
}

/* Only recurse into known containers, in their defined parent context. Unknown boxes are
 * opaque, including a key whose file-local ID happens to spell a container FourCC. */
static int container_prefix(uint32_t type, uint32_t parent) {
    if (parent == BM_FOUR('i', 'p', 'c', 'o') || parent == BM_FOUR('i', 'r', 'e', 'f') ||
        parent == BM_FOUR('t', 'r', 'e', 'f'))
        return -1;
    switch (type) {
    case BM_FOUR('m', 'o', 'o', 'v'):
    case BM_FOUR('t', 'r', 'a', 'k'):
    case BM_FOUR('m', 'd', 'i', 'a'):
    case BM_FOUR('m', 'i', 'n', 'f'):
    case BM_FOUR('s', 't', 'b', 'l'):
    case BM_FOUR('g', 'm', 'h', 'd'):
    case BM_FOUR('d', 'i', 'n', 'f'):
    case BM_FOUR('t', 'r', 'e', 'f'):
    case BM_FOUR('e', 'd', 't', 's'):
    case BM_FOUR('u', 'd', 't', 'a'):
    case BM_FOUR('t', 'a', 'p', 't'):
    case BM_FOUR('i', 'p', 'r', 'p'):
    case BM_FOUR('i', 'p', 'c', 'o'):
    case BM_FOUR('m', 'o', 'o', 'f'):
    case BM_FOUR('t', 'r', 'a', 'f'):
    case BM_FOUR('m', 'f', 'r', 'a'):
    case BM_FOUR('m', 'v', 'e', 'x'):
        return 0;
    case BM_FOUR('m', 'e', 't', 'a'):
    case BM_FOUR('i', 'r', 'e', 'f'):
        return 4;
    case BM_FOUR('d', 'r', 'e', 'f'):
        return 8;
    case BM_FOUR('i', 'i', 'n', 'f'):
        return -2;
    default:
        return -1;
    }
}
ss_status_t bm_children(bm_context *c, uint32_t parent, uint64_t start, uint64_t end,
                        uint32_t depth, bool recurse) {
    if (start < end && depth > 64)
        return bm_limit(c, start, "atom nesting depth exceeds 64");
    uint32_t previous = BM_NONE;
    while (start < end) {
        uint8_t h[32];
        if (end - start < 8)
            return bm_host(c, start, "incomplete box header");
        BM_TRY(bm_read(c, start, h, 8));
        uint64_t n = bm_u32(h), head = 8;
        uint32_t type = bm_u32(h + 4);
        if (n == 1) {
            if (end - start < 16)
                return bm_host(c, start, "incomplete extended box size");
            BM_TRY(bm_read(c, start + 8, h + 8, 8));
            n = bm_u64(h + 8);
            head = 16;
        } else if (n == 0) {
            /* ISO size zero extends to file EOF, never merely to an enclosing box. */
            if (end != c->size)
                return bm_host(c, start, "size-zero box cannot end inside file");
            n = c->size - start;
        }
        if (type == BM_FOUR('u', 'u', 'i', 'd') &&
            c->boxes[parent].type != BM_FOUR('k', 'e', 'y', 's'))
            head += 16;
        if (n < head || n > end - start)
            return bm_host(c, start, "invalid box size or parent bounds");
        if (c->count - 1 >= c->max_boxes)
            return bm_limit(c, start, "box count limit");
        BM_TRY(ssi_grow(c->memory, (void **)&c->boxes, &c->capacity, c->count + 1, sizeof(bm_box)));
        uint32_t i = (uint32_t)c->count++;
        c->boxes[i] = (bm_box){start, n, start + head, type, parent, BM_NONE, BM_NONE, NULL};
        if (previous == BM_NONE)
            c->boxes[parent].first = i;
        else
            c->boxes[previous].next = i;
        previous = i;
        int prefix = recurse ? container_prefix(type, c->boxes[parent].type) : -1;
        if (prefix == -2) {
            uint8_t version;
            if (n - head < 4)
                return bm_host(c, start, "truncated iinf FullBox header");
            BM_TRY(bm_read(c, start + head, &version, 1));
            prefix = version == 0 ? 6 : 8;
        }
        if (prefix >= 0) {
            if ((uint64_t)prefix > n - head)
                return bm_host(c, start, "truncated container prefix");
            BM_TRY(bm_children(c, i, start + head + (unsigned)prefix, start + n, depth + 1, true));
        }
        start += n;
    }
    return SS_OK;
}
uint32_t bm_find(bm_context *c, uint32_t parent, uint32_t type) {
    if (parent == BM_NONE)
        return BM_NONE;
    for (uint32_t i = c->boxes[parent].first; i != BM_NONE; i = c->boxes[i].next)
        if (c->boxes[i].type == type)
            return i;
    return BM_NONE;
}
size_t bm_count(bm_context *c, uint32_t parent, uint32_t type) {
    size_t n = 0;
    if (parent != BM_NONE)
        for (uint32_t i = c->boxes[parent].first; i != BM_NONE; i = c->boxes[i].next)
            n += c->boxes[i].type == type;
    return n;
}
ss_status_t bm_unique(bm_context *c, uint32_t parent, uint32_t type, bool required, uint32_t *out) {
    size_t n = bm_count(c, parent, type);
    *out = bm_find(c, parent, type);
    if (n > 1 || (required && !n))
        return bm_host(c, parent == BM_NONE ? 0 : c->boxes[parent].offset,
                       "duplicate or missing required host box");
    return SS_OK;
}
bool bm_in_mdat(bm_context *c, uint64_t offset, uint64_t size) {
    size_t lo = 0, hi = c->mdat_count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (c->mdats[m].start <= offset)
            lo = m + 1;
        else
            hi = m;
    }
    if (!lo)
        return false;
    bm_range r = c->mdats[lo - 1];
    return offset <= r.end && size <= r.end - offset;
}
static ss_status_t init_tree(bm_context *c) {
    uint64_t max = c->kind == SS_CONTAINER_HEIF ? SS_HEIF_MAX_FILE_BYTES : SS_QT_MAX_FILE_BYTES;
    c->max_boxes = c->kind == SS_CONTAINER_HEIF ? 1048576 : 2097152;
    c->host_domain =
        c->kind == SS_CONTAINER_HEIF ? SS_DOMAIN_HEIF_MALFORMED : SS_DOMAIN_QUICKTIME_MALFORMED;
    c->size = c->io.size(c->io.context);
    if (c->size > max)
        return bm_limit(c, 0, "host file size limit");
    BM_TRY(ssi_grow(c->memory, (void **)&c->boxes, &c->capacity, 1, sizeof(bm_box)));
    c->count = 1;
    c->boxes[0] = (bm_box){0, c->size, 0, 0, BM_NONE, BM_NONE, BM_NONE, NULL};
    BM_TRY(bm_children(c, 0, 0, c->size, 1, true));
    c->mdat_count = bm_count(c, 0, BM_FOUR('m', 'd', 'a', 't'));
    if (c->mdat_count) {
        c->mdats = ssi_alloc(c->memory, c->mdat_count * sizeof(*c->mdats));
        if (!c->mdats)
            return c->memory->failure;
        size_t index = 0;
        for (uint32_t i = c->boxes[0].first; i != BM_NONE; i = c->boxes[i].next)
            if (c->boxes[i].type == BM_FOUR('m', 'd', 'a', 't'))
                c->mdats[index++] =
                    (bm_range){c->boxes[i].payload, c->boxes[i].offset + c->boxes[i].size};
    }
    return SS_OK;
}
ss_status_t bm_context_init(bm_context *c, ss_container_kind_t kind, const ss_io_t *io,
                            const ss_binding_options_t *options) {
    memset(c, 0, sizeof(*c));
    if ((kind != SS_CONTAINER_HEIF && kind != SS_CONTAINER_QUICKTIME) || !SSI_VALID(io, ss_io_t) ||
        !io->read_at || !io->size)
        return SS_INVALID_ARGUMENT;
    ss_binding_options_init(&c->options);
    if (options) {
        if (!SSI_VALID(options, ss_binding_options_t))
            return SS_INVALID_ARGUMENT;
        c->options = *options;
    }
    c->kind = kind;
    c->io = *io;
    BM_TRY(ssi_memory_create(&c->options.resources, &c->memory));
    return init_tree(c);
}
void bm_context_clear(bm_context *c) {
    if (!c->memory)
        return;
    for (size_t i = 0; i < c->count; ++i)
        ssi_free(c->boxes[i].data);
    ssi_free(c->boxes);
    ssi_free(c->mdats);
    ssi_memory_release(c->memory);
    c->memory = NULL;
}
ss_status_t bm_parse_ssps(bm_context *c, uint8_t *data, size_t size, uint64_t offset,
                          ss_document_t **out) {
    ss_error_t e = SS_INIT(ss_error_t);
    ss_status_t s = ssi_document_parse(c->memory, data, size, true, out, &e);
    if (s == SS_OK)
        return s;
    ss_binding_domain_t d = SS_DOMAIN_SSPS_MALFORMED;
    if (s == SS_UNSUPPORTED || s == SS_UNSUPPORTED_CRITICAL_PACKET)
        d = SS_DOMAIN_UNSUPPORTED_SSPS;
    if (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY)
        d = SS_DOMAIN_RESOURCE_LIMIT;
    if (c->result == SS_OK)
        c->result = s;
    if (c->options.diagnostic) {
        ss_binding_diagnostic_t diag = SS_INIT(ss_binding_diagnostic_t);
        diag.domain = d;
        diag.status = s;
        diag.file_offset = offset;
        diag.ssps_offset = e.byte_offset;
        diag.packet_type = e.packet_type;
        memcpy(diag.message, e.message, sizeof(diag.message));
        c->options.diagnostic(c->options.diagnostic_context, &diag);
    }
    return s;
}
ss_status_t bm_finish_container(bm_context *c, ss_container_t *out) {
    if (c->result != SS_OK)
        return c->result;
    out->info.checkpoint_count = out->document->checkpoint_count;
    size_t n;
    if (!ssi_multiply(out->document->checkpoint_count, sizeof(*out->checkpoints), &n))
        return bm_limit(c, 0, "checkpoint index size overflow");
    out->checkpoints = ssi_alloc(c->memory, n);
    if (!out->checkpoints)
        return c->memory->failure;
    size_t sample = 0;
    for (size_t i = 0; i < out->document->checkpoint_count; ++i) {
        ssi_checkpoint_t cp = out->document->checkpoints[i];
        while (sample + 1 < out->info.sample_count &&
               out->samples[sample].timestamp_ns < cp.timestamp)
            ++sample;
        out->checkpoints[i] = (ss_container_checkpoint_t)SS_INIT(ss_container_checkpoint_t);
        out->checkpoints[i].timestamp_ns = cp.timestamp;
        out->checkpoints[i].sample_index = sample;
        out->checkpoints[i].ssps_offset = out->document->packets[cp.begin].info.byte_offset;
    }
    return SS_OK;
}
static ss_status_t open_context(bm_context *c, ss_container_t **out) {
    ss_container_t *p = ssi_alloc(c->memory, sizeof(*p));
    if (!p)
        return c->memory->failure;
    memset(p, 0, sizeof(*p));
    p->memory = c->memory;
    ssi_memory_retain(p->memory);
    p->info = (ss_container_info_t)SS_INIT(ss_container_info_t);
    p->info.kind = c->kind;
    ss_status_t s = c->kind == SS_CONTAINER_HEIF ? bm_heif_open(c, p) : bm_qt_open(c, p);
    if (s == SS_OK)
        s = bm_finish_container(c, p);
    if (s != SS_OK) {
        ss_container_release(p);
        return s;
    }
    *out = p;
    return SS_OK;
}
ss_status_t ss_container_open(ss_container_kind_t kind, const ss_io_t *io,
                              const ss_binding_options_t *options, ss_container_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    bm_context c;
    ss_status_t s = bm_context_init(&c, kind, io, options);
    if (s == SS_OK)
        s = open_context(&c, out);
    if (s != SS_OK && c.result == SS_OK && (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY))
        bm_error(&c, SS_DOMAIN_RESOURCE_LIMIT, s, 0, 0, "binding allocation limit");
    bm_context_clear(&c);
    return s;
}
typedef struct memory_io {
    const uint8_t *bytes;
    size_t size;
} memory_io;
static ss_status_t read_memory(void *context, uint64_t off, void *data, size_t n) {
    memory_io *m = context;
    if (off > m->size || n > m->size - off)
        return SS_IO_ERROR;
    if (n)
        memcpy(data, m->bytes + (size_t)off, n);
    return SS_OK;
}
static uint64_t memory_size(void *context) {
    return ((memory_io *)context)->size;
}
static ss_io_t memory_callbacks(memory_io *m) {
    ss_io_t io = SS_INIT(ss_io_t);
    io.context = m;
    io.read_at = read_memory;
    io.size = memory_size;
    return io;
}
ss_status_t ss_container_open_memory(ss_container_kind_t kind, const void *bytes, size_t size,
                                     const ss_binding_options_t *options, ss_container_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    if (!bytes && size)
        return SS_INVALID_ARGUMENT;
    memory_io m = {bytes, size};
    ss_io_t io = memory_callbacks(&m);
    return ss_container_open(kind, &io, options, out);
}
void ss_container_release(ss_container_t *c) {
    if (!c)
        return;
    ssi_memory_t *m = c->memory;
    ss_document_release(c->document);
    ssi_free(c->samples);
    ssi_free(c->checkpoints);
    ssi_free(c);
    ssi_memory_release(m);
}
ss_status_t ss_container_get_info(const ss_container_t *c, ss_container_info_t *out) {
    if (!c || !SSI_VALID(out, ss_container_info_t))
        return SS_INVALID_ARGUMENT;
    *out = c->info;
    return SS_OK;
}
ss_status_t ss_container_document(const ss_container_t *c, ss_document_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    if (!c)
        return SS_INVALID_ARGUMENT;
    ss_document_retain(c->document);
    *out = c->document;
    return SS_OK;
}
ss_status_t ss_container_ssps_bytes(const ss_container_t *c, const uint8_t **bytes, size_t *size) {
    if (!c || !bytes || !size)
        return SS_INVALID_ARGUMENT;
    *bytes = c->document->data;
    *size = c->document->size;
    return SS_OK;
}
ss_status_t ss_container_get_sample(const ss_container_t *c, uint64_t i,
                                    ss_container_sample_t *out) {
    if (!c || !SSI_VALID(out, ss_container_sample_t))
        return SS_INVALID_ARGUMENT;
    if (i >= c->info.sample_count)
        return SS_OUT_OF_RANGE;
    *out = c->samples[i];
    return SS_OK;
}
ss_status_t ss_container_get_checkpoint(const ss_container_t *c, uint64_t i,
                                        ss_container_checkpoint_t *out) {
    if (!c || !SSI_VALID(out, ss_container_checkpoint_t))
        return SS_INVALID_ARGUMENT;
    if (i >= c->info.checkpoint_count)
        return SS_OUT_OF_RANGE;
    *out = c->checkpoints[i];
    return SS_OK;
}
ss_status_t bm_copy(bm_context *c, ssi_buffer_t *out, uint64_t offset, uint64_t size) {
    uint8_t scratch[32768];
    while (size) {
        size_t n = (size_t)SSI_MIN(size, sizeof(scratch));
        BM_TRY(bm_read(c, offset, scratch, n));
        BM_TRY(ssi_buffer_append(out, scratch, n));
        offset += n;
        size -= n;
    }
    return SS_OK;
}
ss_status_t bm_put(ssi_buffer_t *b, uint64_t v, unsigned n) {
    uint8_t p[8];
    if (n > 8)
        return SS_INVALID_ARGUMENT;
    for (unsigned i = 0; i < n; ++i)
        p[n - i - 1] = (uint8_t)(v >> (i * 8));
    return ssi_buffer_append(b, p, n);
}
ss_status_t bm_begin(ssi_buffer_t *b, uint32_t type, size_t *start) {
    *start = b->size;
    BM_TRY(bm_put(b, 0, 4));
    return bm_put(b, type, 4);
}
ss_status_t bm_end(ssi_buffer_t *b, size_t start) {
    if (b->size - start > UINT32_MAX)
        return SS_BINDING_UNREPRESENTABLE;
    bm_w32(b->data + start, (uint32_t)(b->size - start));
    return SS_OK;
}
ss_status_t bm_full(ssi_buffer_t *b, uint32_t type, uint32_t vf, size_t *start) {
    BM_TRY(bm_begin(b, type, start));
    return bm_put(b, vf, 4);
}
ss_status_t bm_raw_box(ssi_buffer_t *b, uint32_t type, const void *p, size_t n) {
    size_t start;
    BM_TRY(bm_begin(b, type, &start));
    BM_TRY(ssi_buffer_append(b, p, n));
    return bm_end(b, start);
}
ss_status_t ss_binding_output_bytes(const ss_binding_output_t *out, const uint8_t **p, size_t *n) {
    if (!out || !p || !n)
        return SS_INVALID_ARGUMENT;
    *p = out->buffer.data;
    *n = out->buffer.size;
    return SS_OK;
}
void ss_binding_output_release(ss_binding_output_t *out) {
    if (!out)
        return;
    ssi_memory_t *m = out->memory;
    ssi_buffer_clear(&out->buffer);
    ssi_free(out);
    ssi_memory_release(m);
}
static ss_status_t publish(bm_context *c, ssi_buffer_t *b, bool binding,
                           ss_binding_output_t **out) {
    memory_io m = {b->data, b->size};
    bm_context verify = {0};
    verify.memory = c->memory;
    ssi_memory_retain(verify.memory);
    verify.io = memory_callbacks(&m);
    verify.kind = c->kind;
    verify.options = c->options;
    ss_status_t s = init_tree(&verify);
    ss_container_t *container = NULL;
    if (s == SS_OK && binding)
        s = open_context(&verify, &container);
    /* Rewriters validate the unbound host before emitting. Stripped output is also parsed
     * structurally here; absence of the removed binding is intentional. */
    ss_container_release(container);
    if (s != SS_OK && c->result == SS_OK) {
        if (verify.result == SS_OK && (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY))
            bm_error(c, SS_DOMAIN_RESOURCE_LIMIT, s, 0, 0, "output validation allocation limit");
        c->result = s;
    }
    bm_context_clear(&verify);
    if (s != SS_OK)
        return s;
    ss_binding_output_t *p = ssi_alloc(c->memory, sizeof(*p));
    if (!p)
        return c->memory->failure;
    p->memory = c->memory;
    ssi_memory_retain(p->memory);
    p->buffer = *b;
    memset(b, 0, sizeof(*b));
    *out = p;
    return SS_OK;
}
static ss_status_t rewrite(ss_container_kind_t kind, const void *host, size_t size, uint32_t id,
                           const ss_document_t *doc, const ss_binding_options_t *options,
                           ss_binding_output_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    if (!host && size)
        return SS_INVALID_ARGUMENT;
    memory_io m = {host, size};
    ss_io_t io = memory_callbacks(&m);
    bm_context c;
    ss_status_t s = bm_context_init(&c, kind, &io, options);
    ssi_buffer_t b = {.memory = c.memory};
    if (s == SS_OK)
        s = kind == SS_CONTAINER_HEIF ? bm_heif_rewrite(&c, doc, &b)
                                      : bm_qt_rewrite(&c, id, doc, &b);
    if (s == SS_OK)
        s = publish(&c, &b, doc != NULL, out);
    if (s != SS_OK && s != SS_INVALID_ARGUMENT && c.result == SS_OK) {
        ss_binding_domain_t d =
            s == SS_BINDING_UNREPRESENTABLE
                ? SS_DOMAIN_BINDING_UNREPRESENTABLE
                : (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY ? SS_DOMAIN_RESOURCE_LIMIT
                                                                   : SS_DOMAIN_BINDING_MALFORMED);
        bm_error(&c, d, s, 0, 0, "container rewrite failed; no output published");
    }
    ssi_buffer_clear(&b);
    bm_context_clear(&c);
    return s;
}
ss_status_t ss_heif_bind_memory(const void *host, size_t size, const ss_document_t *doc,
                                const ss_binding_options_t *options, ss_binding_output_t **out) {
    if (!doc) {
        if (out)
            *out = NULL;
        return SS_INVALID_ARGUMENT;
    }
    return rewrite(SS_CONTAINER_HEIF, host, size, 0, doc, options, out);
}
ss_status_t ss_quicktime_bind_memory(const void *host, size_t size, uint32_t id,
                                     const ss_document_t *doc, const ss_binding_options_t *options,
                                     ss_binding_output_t **out) {
    if (!doc) {
        if (out)
            *out = NULL;
        return SS_INVALID_ARGUMENT;
    }
    return rewrite(SS_CONTAINER_QUICKTIME, host, size, id, doc, options, out);
}
ss_status_t ss_container_strip_memory(ss_container_kind_t kind, const void *host, size_t size,
                                      const ss_binding_options_t *options,
                                      ss_binding_output_t **out) {
    return rewrite(kind, host, size, 0, NULL, options, out);
}
ss_status_t ss_quicktime_mux(const void *entry, size_t size,
                             const ss_encoded_video_sample_t *samples, size_t count,
                             const ss_document_t *doc, const ss_binding_options_t *options,
                             ss_binding_output_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    if (!entry || !size || !samples || !count || !doc)
        return SS_INVALID_ARGUMENT;
    memory_io m = {NULL, 0};
    ss_io_t io = memory_callbacks(&m);
    bm_context c;
    ss_status_t s = bm_context_init(&c, SS_CONTAINER_QUICKTIME, &io, options);
    ssi_buffer_t b = {.memory = c.memory};
    if (s == SS_OK)
        s = bm_qt_mux(&c, entry, size, samples, count, doc, &b);
    if (s == SS_OK)
        s = publish(&c, &b, true, out);
    if (s != SS_OK && s != SS_INVALID_ARGUMENT && c.result == SS_OK)
        bm_error(&c,
                 s == SS_BINDING_UNREPRESENTABLE ? SS_DOMAIN_BINDING_UNREPRESENTABLE
                                                 : (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY
                                                        ? SS_DOMAIN_RESOURCE_LIMIT
                                                        : SS_DOMAIN_BINDING_MALFORMED),
                 s, 0, 0, "QuickTime mux failed; no output published");
    ssi_buffer_clear(&b);
    bm_context_clear(&c);
    return s;
}
ss_status_t ss_quicktime_trim_memory(const void *host, size_t size, uint64_t start, uint64_t end,
                                     const ss_writer_options_t *writer_options,
                                     const ss_binding_options_t *options,
                                     ss_binding_output_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    if ((!host && size) || !SSI_VALID(writer_options, ss_writer_options_t))
        return SS_INVALID_ARGUMENT;
    memory_io m = {host, size};
    ss_io_t io = memory_callbacks(&m);
    bm_context c;
    ss_status_t s = bm_context_init(&c, SS_CONTAINER_QUICKTIME, &io, options);
    ssi_buffer_t b = {.memory = c.memory};
    if (s == SS_OK)
        s = bm_qt_trim(&c, start, end, writer_options, &b);
    if (s == SS_OK)
        s = publish(&c, &b, true, out);
    if (s != SS_OK && c.result == SS_OK && (s == SS_RESOURCE_LIMIT || s == SS_OUT_OF_MEMORY))
        bm_error(&c, SS_DOMAIN_RESOURCE_LIMIT, s, 0, 0, "trim allocation limit");
    if (s == SS_IO_ERROR && c.result == SS_OK)
        bm_error(&c, SS_DOMAIN_IO_ERROR, s, 0, 0, "trim stream identity generation failed");
    if (s == SS_BINDING_UNREPRESENTABLE && c.result == SS_OK)
        bm_error(&c, SS_DOMAIN_BINDING_UNREPRESENTABLE, s, 0, 0,
                 "trim cannot be represented in this binding");
    ssi_buffer_clear(&b);
    bm_context_clear(&c);
    return s;
}
