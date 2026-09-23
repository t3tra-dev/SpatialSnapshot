#ifndef SS_BMFF_H
#define SS_BMFF_H
#include "internal.h"
#define BM_FOUR(a, b, c, d)                                                                        \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))
#define BM_NONE UINT32_MAX
#define BM_TRY(x)                                                                                  \
    do {                                                                                           \
        ss_status_t bm_s_ = (x);                                                                   \
        if (bm_s_ != SS_OK)                                                                        \
            return bm_s_;                                                                          \
    } while (0)

typedef struct bm_box {
    uint64_t offset, size, payload;
    uint32_t type, parent, first, next;
    uint8_t *data;
} bm_box;
typedef struct bm_range {
    uint64_t start, end;
} bm_range;
typedef struct bm_context {
    ssi_memory_t *memory;
    ss_binding_options_t options;
    ss_io_t io;
    uint64_t size;
    ss_container_kind_t kind;
    ss_binding_domain_t host_domain;
    ss_status_t result;
    bm_box *boxes;
    size_t count, capacity;
    uint32_t root, max_boxes;
    bm_range *mdats;
    size_t mdat_count;
} bm_context;
typedef struct bm_cursor {
    bm_context *ctx;
    const uint8_t *data;
    size_t size, pos;
    uint64_t offset;
    bool failed;
} bm_cursor;
struct ss_container {
    ssi_memory_t *memory;
    ss_document_t *document;
    ss_container_info_t info;
    ss_container_sample_t *samples;
    ss_container_checkpoint_t *checkpoints;
};
struct ss_binding_output {
    ssi_memory_t *memory;
    ssi_buffer_t buffer;
};
uint16_t bm_u16(const uint8_t *p);
uint32_t bm_u32(const uint8_t *p);
uint64_t bm_u64(const uint8_t *p);
int64_t bm_i32(const uint8_t *p);
int64_t bm_i64(const uint8_t *p);
void bm_w16(uint8_t *p, uint16_t n);
void bm_w32(uint8_t *p, uint32_t n);
void bm_w64(uint8_t *p, uint64_t n);
ss_status_t bm_error(bm_context *c, ss_binding_domain_t domain, ss_status_t status, uint64_t offset,
                     uint32_t id, const char *message);
bool bm_check(bm_context *c, bool condition, uint64_t offset, uint32_t id, const char *message);
ss_status_t bm_host(bm_context *c, uint64_t offset, const char *message);
ss_status_t bm_limit(bm_context *c, uint64_t offset, const char *message);
ss_status_t bm_read(bm_context *c, uint64_t offset, void *data, size_t size);
ss_status_t bm_load(bm_context *c, uint32_t box, bm_cursor *cursor);
uint64_t bm_get(bm_cursor *r, unsigned bytes);
const uint8_t *bm_take(bm_cursor *r, size_t size);
bool bm_string(bm_cursor *r, const char **s, size_t *size);
ss_status_t bm_done(bm_cursor *r);
ss_status_t bm_children(bm_context *c, uint32_t parent, uint64_t start, uint64_t end,
                        uint32_t depth, bool recurse);
uint32_t bm_find(bm_context *c, uint32_t parent, uint32_t type);
size_t bm_count(bm_context *c, uint32_t parent, uint32_t type);
ss_status_t bm_unique(bm_context *c, uint32_t parent, uint32_t type, bool required, uint32_t *out);
bool bm_in_mdat(bm_context *c, uint64_t offset, uint64_t size);
ss_status_t bm_context_init(bm_context *c, ss_container_kind_t kind, const ss_io_t *io,
                            const ss_binding_options_t *options);
void bm_context_clear(bm_context *c);
ss_status_t bm_parse_ssps(bm_context *c, uint8_t *data, size_t size, uint64_t file_offset,
                          ss_document_t **out);
ss_status_t bm_finish_container(bm_context *c, ss_container_t *out);
ss_status_t bm_heif_open(bm_context *c, ss_container_t *out);
ss_status_t bm_qt_open(bm_context *c, ss_container_t *out);
ss_status_t bm_heif_rewrite(bm_context *c, const ss_document_t *doc, ssi_buffer_t *out);
ss_status_t bm_qt_rewrite(bm_context *c, uint32_t video_id, const ss_document_t *doc,
                          ssi_buffer_t *out);
ss_status_t bm_qt_mux(bm_context *c, const void *entry, size_t entry_size,
                      const ss_encoded_video_sample_t *samples, size_t count,
                      const ss_document_t *doc, ssi_buffer_t *out);
ss_status_t bm_qt_trim(bm_context *c, uint64_t start, uint64_t end,
                       const ss_writer_options_t *options, ssi_buffer_t *out);
ss_status_t bm_copy(bm_context *c, ssi_buffer_t *out, uint64_t offset, uint64_t size);
ss_status_t bm_put(ssi_buffer_t *b, uint64_t value, unsigned bytes);
ss_status_t bm_begin(ssi_buffer_t *b, uint32_t type, size_t *start);
ss_status_t bm_end(ssi_buffer_t *b, size_t start);
ss_status_t bm_full(ssi_buffer_t *b, uint32_t type, uint32_t version_flags, size_t *start);
ss_status_t bm_raw_box(ssi_buffer_t *b, uint32_t type, const void *data, size_t size);
ss_status_t bm_codec_config(bm_context *c, uint32_t type, const uint8_t *data, size_t size,
                            uint64_t offset);
#endif
