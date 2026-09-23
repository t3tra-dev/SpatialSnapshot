#include <spatialsnapshot/spatialsnapshot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
typedef struct blob {
    uint8_t *p;
    size_t size;
} blob;
static blob read_file(const char *file) {
    FILE *f = fopen(file, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    CHECK(n >= 0);
    CHECK(!fseek(f, 0, SEEK_SET));
    blob b = {malloc((size_t)n), (size_t)n};
    CHECK(b.p);
    CHECK(fread(b.p, 1, b.size, f) == b.size);
    CHECK(!fclose(f));
    return b;
}
static blob fixture(const char *name) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", SS_FIXTURES_DIR, name);
    return read_file(path);
}
static void output_file(const char *name, const uint8_t *p, size_t n) {
    FILE *f = fopen(name, "wb");
    CHECK(f);
    CHECK(fwrite(p, 1, n, f) == n);
    CHECK(!fclose(f));
}
typedef struct allocator {
    size_t calls, fail, live;
} allocator;
static void *allocate(void *context, size_t size) {
    allocator *a = context;
    if (++a->calls == a->fail)
        return NULL;
    void *p = malloc(size);
    if (p)
        ++a->live;
    return p;
}
static void deallocate(void *context, void *p) {
    allocator *a = context;
    CHECK(a->live);
    --a->live;
    free(p);
}
static unsigned diagnoses;
static ss_binding_domain_t last_domain;
static void diagnostic(void *context, const ss_binding_diagnostic_t *d) {
    if (context)
        fprintf(stderr, "%s: %s\n", ss_binding_domain_string(d->domain), d->message);
    CHECK(d->struct_size == sizeof(*d));
    CHECK(d->status != SS_OK);
    ++diagnoses;
    last_domain = d->domain;
}
static ss_document_t *document(blob b) {
    ss_document_t *doc = NULL;
    CHECK(ss_document_open_memory(b.p, b.size, NULL, &doc, NULL) == SS_OK);
    return doc;
}
static void same_ssps(ss_container_t *c, blob b) {
    const uint8_t *p;
    size_t n;
    CHECK(ss_container_ssps_bytes(c, &p, &n) == SS_OK);
    CHECK(n == b.size && !memcmp(p, b.p, n));
}
static void roundtrip(ss_container_kind_t kind, const char *path) {
    blob source = fixture(path);
    ss_container_t *c = NULL;
    CHECK(ss_container_open_memory(kind, source.p, source.size, NULL, &c) == SS_OK);
    ss_container_info_t info = SS_INIT(ss_container_info_t);
    CHECK(ss_container_get_info(c, &info) == SS_OK);
    CHECK(info.raster_width == 4 && info.raster_height == 3);
    CHECK(info.sample_count == (kind == SS_CONTAINER_HEIF ? 1 : 3));
    CHECK(info.checkpoint_count == 1);
    ss_container_checkpoint_t cp = SS_INIT(ss_container_checkpoint_t);
    CHECK(ss_container_get_checkpoint(c, 0, &cp) == SS_OK);
    CHECK(cp.timestamp_ns == 0 && cp.sample_index == 0);
    ss_container_sample_t sample = SS_INIT(ss_container_sample_t);
    CHECK(ss_container_get_sample(c, info.sample_count - 1, &sample) == SS_OK);
    CHECK(sample.timestamp_ns == (kind == SS_CONTAINER_HEIF ? 0 : UINT64_C(2000000000)));
    ss_document_t *doc = NULL;
    CHECK(ss_container_document(c, &doc) == SS_OK);
    const uint8_t *ssps;
    size_t length;
    CHECK(ss_container_ssps_bytes(c, &ssps, &length) == SS_OK);
    blob original = {malloc(length), length};
    CHECK(original.p);
    memcpy(original.p, ssps, length);
    ss_binding_output_t *stripped = NULL;
    CHECK(ss_container_strip_memory(kind, source.p, source.size, NULL, &stripped) == SS_OK);
    const uint8_t *ordinary;
    size_t ordinary_size;
    CHECK(ss_binding_output_bytes(stripped, &ordinary, &ordinary_size) == SS_OK);
    ss_container_t *absent = NULL;
    CHECK(ss_container_open_memory(kind, ordinary, ordinary_size, NULL, &absent) == SS_MALFORMED);
    CHECK(!absent);
    if (kind == SS_CONTAINER_HEIF) {
        CHECK(ordinary_size < source.size);
        for (size_t i = 0; i + 8 <= ordinary_size; ++i)
            CHECK(memcmp(ordinary + i, "SSPS\r\n\x1a\n", 8));
    }
    ss_binding_output_t *bound = NULL;
    ss_status_t status =
        kind == SS_CONTAINER_HEIF
            ? ss_heif_bind_memory(ordinary, ordinary_size, doc, NULL, &bound)
            : ss_quicktime_bind_memory(ordinary, ordinary_size, 0, doc, NULL, &bound);
    CHECK(status == SS_OK);
    CHECK(bound);
    const uint8_t *bytes;
    size_t size;
    CHECK(ss_binding_output_bytes(bound, &bytes, &size) == SS_OK);
    ss_container_t *reopened = NULL;
    CHECK(ss_container_open_memory(kind, bytes, size, NULL, &reopened) == SS_OK);
    same_ssps(reopened, original);
    ss_container_release(c);
    c = NULL;
    ss_scene_cursor_t *scene = NULL;
    CHECK(ss_scene_cursor_create(doc, &scene) == SS_OK);
    CHECK(ss_scene_cursor_seek(scene, 0) == SS_OK);
    ss_scene_cursor_release(scene);
    ss_container_release(reopened);
    ss_binding_output_release(bound);
    ss_binding_output_release(stripped);
    ss_document_release(doc);
    free(original.p);
    free(source.p);
}
static void allocation_failures(ss_container_kind_t kind, const char *path) {
    blob source = fixture(path);
    for (size_t fail = 1; fail < 500; ++fail) {
        allocator a = {.fail = fail};
        ss_binding_options_t options;
        ss_binding_options_init(&options);
        options.resources.allocator.context = &a;
        options.resources.allocator.allocate = allocate;
        options.resources.allocator.deallocate = deallocate;
        options.diagnostic = diagnostic;
        ss_container_t *c = NULL;
        ss_status_t s = ss_container_open_memory(kind, source.p, source.size, &options, &c);
        CHECK(s == SS_OK || s == SS_OUT_OF_MEMORY);
        CHECK((s == SS_OK) == !!c);
        ss_container_release(c);
        CHECK(!a.live);
        if (s == SS_OK)
            break;
    }
    ss_container_t *c = NULL;
    CHECK(ss_container_open_memory(kind, source.p, source.size, NULL, &c) == SS_OK);
    ss_document_t *doc = NULL;
    CHECK(ss_container_document(c, &doc) == SS_OK);
    for (size_t fail = 1; fail < 700; ++fail) {
        allocator a = {.fail = fail};
        ss_binding_options_t options;
        ss_binding_options_init(&options);
        options.resources.allocator.context = &a;
        options.resources.allocator.allocate = allocate;
        options.resources.allocator.deallocate = deallocate;
        ss_binding_output_t *out = NULL;
        ss_status_t s =
            kind == SS_CONTAINER_HEIF
                ? ss_heif_bind_memory(source.p, source.size, doc, &options, &out)
                : ss_quicktime_bind_memory(source.p, source.size, 0, doc, &options, &out);
        CHECK(s == SS_OK || s == SS_OUT_OF_MEMORY);
        CHECK((s == SS_OK) == !!out);
        ss_binding_output_release(out);
        CHECK(!a.live);
        if (s == SS_OK)
            break;
    }
    ss_document_release(doc);
    ss_container_release(c);
    free(source.p);
}
static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t u64(const uint8_t *p) {
    return (uint64_t)u32(p) << 32 | u32(p + 4);
}
static void w64(uint8_t *p, uint64_t n) {
    w32(p, (uint32_t)(n >> 32));
    w32(p + 4, (uint32_t)n);
}
typedef struct sparse_input {
    blob source;
    uint64_t bias, length;
    uint64_t blocked_start, blocked_end;
    unsigned calls;
    int fail;
} sparse_input;
static uint64_t sparse_size(void *context) {
    return ((sparse_input *)context)->length;
}
static ss_status_t sparse_read(void *context, uint64_t offset, void *out, size_t size) {
    sparse_input *s = context;
    ++s->calls;
    if (s->fail)
        return SS_IO_ERROR;
    if (offset < s->bias) {
        uint8_t header[16];
        w32(header, 1);
        memcpy(header + 4, "free", 4);
        w64(header + 8, s->bias);
        if (offset > sizeof(header) || size > sizeof(header) - offset)
            return SS_IO_ERROR;
        memcpy(out, header + (size_t)offset, size);
        return SS_OK;
    }
    offset -= s->bias;
    if (offset > s->source.size || size > s->source.size - offset ||
        (offset < s->blocked_end && offset + size > s->blocked_start))
        return SS_IO_ERROR;
    memcpy(out, s->source.p + (size_t)offset, size);
    return SS_OK;
}
static void random_access_io(void) {
    blob source = fixture("bindings/qt-minimal.mov");
    uint64_t bias = UINT64_C(4294967296);
    /* Patch the known independent fixture's two co64 tables for a virtual 4 GiB prefix. */
    unsigned patched = 0;
    for (size_t i = 4; i + 16 <= source.size; ++i)
        if (!memcmp(source.p + i, "co64", 4)) {
            uint32_t count = u32(source.p + i + 8);
            CHECK(count == 3);
            for (uint32_t j = 0; j < count; ++j) {
                uint8_t *p = source.p + i + 12 + j * 8;
                w64(p, u64(p) + bias);
            }
            ++patched;
        }
    CHECK(patched == 2);
    sparse_input input = {.source = source,
                          .bias = bias,
                          .length = bias + source.size,
                          .blocked_start = 28,
                          .blocked_end = 136};
    ss_io_t io = SS_INIT(ss_io_t);
    io.context = &input;
    io.read_at = sparse_read;
    io.size = sparse_size;
    ss_container_t *c = NULL;
    CHECK(ss_container_open(SS_CONTAINER_QUICKTIME, &io, NULL, &c) == SS_OK);
    ss_container_sample_t sample = SS_INIT(ss_container_sample_t);
    CHECK(ss_container_get_sample(c, 0, &sample) == SS_OK);
    CHECK(sample.media_offset == bias + 28);
    CHECK(input.calls > 0);
    ss_container_release(c);
    c = NULL;
    input.calls = 0;
    input.length = SS_QT_MAX_FILE_BYTES + 1;
    CHECK(ss_container_open(SS_CONTAINER_QUICKTIME, &io, NULL, &c) == SS_RESOURCE_LIMIT);
    CHECK(!c && !input.calls);
    input.length = bias + source.size;
    input.fail = 1;
    CHECK(ss_container_open(SS_CONTAINER_QUICKTIME, &io, NULL, &c) == SS_IO_ERROR);
    CHECK(!c);
    free(source.p);
}
static void mux(void) {
    blob ssps = fixture("v1/minimal-video.ssps");
    ss_document_t *doc = document(ssps);
    uint8_t entry[86] = {0};
    w32(entry, sizeof(entry));
    memcpy(entry + 4, "raw ", 4);
    w16(entry + 14, 1);
    w16(entry + 32, 4);
    w16(entry + 34, 3);
    w32(entry + 36, 72u << 16);
    w32(entry + 40, 72u << 16);
    w16(entry + 48, 1);
    w16(entry + 82, 24);
    w16(entry + 84, 65535);
    uint8_t rgb[36] = {0};
    ss_encoded_video_sample_t samples[3];
    for (unsigned i = 0; i < 3; ++i) {
        samples[i] = (ss_encoded_video_sample_t)SS_INIT(ss_encoded_video_sample_t);
        samples[i].bytes = rgb;
        samples[i].size = sizeof(rgb);
        samples[i].presentation_timestamp_ns = (uint64_t)i * 1000000000;
        samples[i].decode_duration_ns = 1000000000;
        samples[i].is_sync = 1;
    }
    for (unsigned reordered = 0; reordered < 2; ++reordered) {
        if (reordered) {
            samples[1].presentation_timestamp_ns = 2000000000;
            samples[2].presentation_timestamp_ns = 1000000000;
        }
        ss_binding_output_t *out = NULL;
        CHECK(ss_quicktime_mux(entry, sizeof(entry), samples, 3, doc, NULL, &out) == SS_OK);
        const uint8_t *p;
        size_t n;
        CHECK(ss_binding_output_bytes(out, &p, &n) == SS_OK);
        ss_container_t *c = NULL;
        CHECK(ss_container_open_memory(SS_CONTAINER_QUICKTIME, p, n, NULL, &c) == SS_OK);
        same_ssps(c, ssps);
        ss_container_release(c);
        ss_binding_output_release(out);
    }
    ss_binding_options_t options;
    ss_binding_options_init(&options);
    options.diagnostic = diagnostic;
    ss_binding_output_t *out = NULL;
    w16(entry + 32, 5);
    CHECK(ss_quicktime_mux(entry, sizeof(entry), samples, 3, doc, &options, &out) == SS_MALFORMED);
    CHECK(!out && last_domain == SS_DOMAIN_BINDING_MALFORMED);
    w16(entry + 32, 4);
    allocator a = {.fail = 1};
    options.resources.allocator.context = &a;
    options.resources.allocator.allocate = allocate;
    options.resources.allocator.deallocate = deallocate;
    unsigned before = diagnoses;
    CHECK(ss_quicktime_mux(entry, sizeof(entry), samples, 3, doc, &options, &out) ==
          SS_OUT_OF_MEMORY);
    CHECK(!out && !a.live && diagnoses == before + 1 && last_domain == SS_DOMAIN_RESOURCE_LIMIT);
    ss_document_release(doc);
    free(ssps.p);
}
static void overlapping_item_rewrite(void) {
    blob host = fixture("bindings/heif-overlap.heif");
    blob ssps = fixture("v1/minimal-still.ssps");
    ss_document_t *doc = document(ssps);
    ss_binding_output_t *out = NULL;
    ss_binding_options_t options;
    ss_binding_options_init(&options);
    options.diagnostic = diagnostic;
    CHECK(ss_container_strip_memory(SS_CONTAINER_HEIF, host.p, host.size, &options, &out) ==
          SS_MALFORMED);
    CHECK(!out && last_domain == SS_DOMAIN_BINDING_MALFORMED);
    CHECK(ss_heif_bind_memory(host.p, host.size, doc, &options, &out) == SS_MALFORMED);
    CHECK(!out && last_domain == SS_DOMAIN_BINDING_MALFORMED);
    ss_document_release(doc);
    free(ssps.p);
    free(host.p);
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--strip")) {
        ss_container_kind_t kind =
            !strcmp(argv[2], "heif") ? SS_CONTAINER_HEIF : SS_CONTAINER_QUICKTIME;
        blob host = read_file(argv[3]);
        ss_binding_output_t *out = NULL;
        CHECK(ss_container_strip_memory(kind, host.p, host.size, NULL, &out) == SS_OK);
        const uint8_t *p;
        size_t n;
        CHECK(ss_binding_output_bytes(out, &p, &n) == SS_OK);
        output_file(argv[4], p, n);
        ss_binding_output_release(out);
        free(host.p);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "--bind")) {
        ss_container_kind_t kind =
            !strcmp(argv[2], "heif") ? SS_CONTAINER_HEIF : SS_CONTAINER_QUICKTIME;
        blob host = read_file(argv[3]), ssps = read_file(argv[4]);
        ss_document_t *doc = document(ssps);
        ss_binding_output_t *out = NULL;
        ss_binding_options_t options;
        ss_binding_options_init(&options);
        options.diagnostic = diagnostic;
        options.diagnostic_context = &options;
        ss_status_t s = kind == SS_CONTAINER_HEIF
                            ? ss_heif_bind_memory(host.p, host.size, doc, &options, &out)
                            : ss_quicktime_bind_memory(host.p, host.size, 0, doc, &options, &out);
        CHECK(s == SS_OK);
        const uint8_t *p;
        size_t n;
        CHECK(ss_binding_output_bytes(out, &p, &n) == SS_OK);
        output_file(argv[5], p, n);
        ss_binding_output_release(out);
        ss_document_release(doc);
        free(host.p);
        free(ssps.p);
        return 0;
    }
    roundtrip(SS_CONTAINER_HEIF, "bindings/heif-minimal.heif");
    roundtrip(SS_CONTAINER_HEIF, "bindings/heif-wide-ids.heif");
    roundtrip(SS_CONTAINER_HEIF, "bindings/heif-grid.heif");
    roundtrip(SS_CONTAINER_HEIF, "bindings/heif-forward-compatible.heif");
    roundtrip(SS_CONTAINER_QUICKTIME, "bindings/qt-minimal.mov");
    roundtrip(SS_CONTAINER_QUICKTIME, "bindings/qt-bframes.mov");
    roundtrip(SS_CONTAINER_QUICKTIME, "bindings/qt-uuid-key.mov");
    roundtrip(SS_CONTAINER_QUICKTIME, "bindings/qt-forward-compatible.mov");
    allocation_failures(SS_CONTAINER_HEIF, "bindings/heif-minimal.heif");
    allocation_failures(SS_CONTAINER_QUICKTIME, "bindings/qt-minimal.mov");
    mux();
    overlapping_item_rewrite();
    random_access_io();
    printf("%u binding C checks passed\n", checks);
    return 0;
}
