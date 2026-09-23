#ifndef SS_INTERNAL_H
#define SS_INTERNAL_H
#include "spatialsnapshot/spatialsnapshot.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "SSPS requires 8-bit bytes");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "SSPS requires IEEE 754 binary32");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53, "SSPS queries require binary64");

#define SSI_VALID(p, type)                                                                         \
    ((p) && (p)->struct_size >= sizeof(type) && ((p)->abi_version >> 16) == (SS_ABI_VERSION >> 16))
#define SSI_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct ssi_memory {
    ss_open_options_t options;
    size_t used;
    size_t references;
    ss_status_t failure;
} ssi_memory_t;
typedef struct ssi_buffer {
    uint8_t *data;
    size_t size, capacity;
    ssi_memory_t *memory;
} ssi_buffer_t;

typedef struct ssi_mesh {
    size_t references;
    ss_cell_key_t key;
    uint32_t vertices, triangles, raw_size;
    uint16_t *xyz, *indices;
    uint8_t *classes;
} ssi_mesh_t;
typedef struct ssi_entry {
    ss_cell_key_t key;
    uint64_t packet;
    ssi_mesh_t *mesh;
    uint32_t raw_size;
    uint8_t state; /* 0 empty, 1 occupied, 2 deleted */
} ssi_entry_t;
typedef struct ssi_map {
    ssi_memory_t *memory;
    ssi_entry_t *entries;
    size_t capacity, count, occupied;
    uint64_t bytes;
} ssi_map_t;
typedef struct ssi_packet {
    ss_packet_info_t info;
    ss_cell_key_t key;
    uint64_t checkpoint_id;
    uint32_t chunks;
} ssi_packet_t;
typedef struct ssi_frame {
    ss_camera_t camera;
    size_t depth_packet;
} ssi_frame_t;
typedef struct ssi_checkpoint {
    uint64_t timestamp;
    size_t begin, end;
} ssi_checkpoint_t;
typedef struct ssi_bvh_node {
    double low[3], high[3];
    size_t left, right, start, count;
} ssi_bvh_node_t;

struct ss_document {
    ssi_memory_t *memory;
    size_t references;
    uint8_t *data;
    size_t size;
    bool owns_data;
    ss_document_info_t info;
    ssi_packet_t *packets;
    size_t packet_count, packet_capacity;
    ssi_frame_t *frames;
    size_t frame_count, frame_capacity;
    ssi_checkpoint_t *checkpoints;
    size_t checkpoint_count, checkpoint_capacity;
};
struct ss_scene_cursor {
    ss_document_t *document;
    uint64_t timestamp;
    bool positioned;
    size_t frame;
    ssi_map_t map;
    ssi_entry_t **sorted;
    uint16_t *depth_values;
    uint8_t *depth_confidence;
    ss_depth_view_t depth;
    ssi_bvh_node_t *bvh;
    ssi_entry_t **bvh_entries;
    size_t bvh_count;
};
struct ss_decoder {
    ssi_memory_t *memory;
    ssi_buffer_t buffer;
    bool finished;
    ss_status_t failure;
};
struct ss_writer {
    ssi_memory_t *memory;
    ss_writer_options_t options;
    ssi_buffer_t buffer;
    ssi_map_t scene;
    uint64_t sequence, frames, last_time, checkpoint_id, checkpoint_time, delta_bytes;
    uint32_t width, height, depth_width, depth_height;
    bool finished;
};
struct ss_mesh_set {
    ssi_memory_t *memory;
    ssi_mesh_t **meshes;
    size_t count;
};

ss_status_t ssi_memory_create(const ss_open_options_t *options, ssi_memory_t **out);
void ssi_memory_retain(ssi_memory_t *memory);
void ssi_memory_release(ssi_memory_t *memory);
void *ssi_alloc(ssi_memory_t *memory, size_t bytes);
void ssi_free(void *pointer);
ss_status_t ssi_grow(ssi_memory_t *memory, void **data, size_t *capacity, size_t count,
                     size_t element);
ss_status_t ssi_buffer_append(ssi_buffer_t *buffer, const void *bytes, size_t size);
void ssi_buffer_clear(ssi_buffer_t *buffer);
bool ssi_add(size_t a, size_t b, size_t *result);
bool ssi_multiply(size_t a, size_t b, size_t *result);
uint16_t ssi_u16(const uint8_t *p);
uint32_t ssi_u32(const uint8_t *p);
uint64_t ssi_u64(const uint8_t *p);
int32_t ssi_i32(const uint8_t *p);
float ssi_f32(const uint8_t *p);
void ssi_w16(uint8_t *p, uint16_t v);
void ssi_w32(uint8_t *p, uint32_t v);
void ssi_w64(uint8_t *p, uint64_t v);
void ssi_wf32(uint8_t *p, float v);
bool ssi_zero(const uint8_t *p, size_t n);
bool ssi_known(uint32_t type);
void ssi_error(ss_error_t *error, ss_status_t status, uint64_t offset, uint64_t sequence,
               uint32_t type, const char *message);

ss_status_t ssi_lz4_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t raw_size);
ss_status_t ssi_lz4_encode(ssi_buffer_t *out, const uint8_t *src, size_t size);
ss_status_t ssi_packet_raw(const ss_document_t *document, const ssi_packet_t *packet,
                           uint8_t **out);
ss_status_t ssi_emit_packet(ssi_buffer_t *out, uint64_t sequence, uint64_t timestamp, uint16_t type,
                            const uint8_t *raw, size_t size);

int ssi_key_compare(ss_cell_key_t a, ss_cell_key_t b);
ssi_entry_t *ssi_map_find(const ssi_map_t *map, ss_cell_key_t key);
ss_status_t ssi_map_put(ssi_map_t *map, ssi_entry_t entry);
ss_status_t ssi_map_remove(ssi_map_t *map, ss_cell_key_t key);
ss_status_t ssi_map_clone(const ssi_map_t *source, ssi_map_t *destination);
ss_status_t ssi_map_sorted(const ssi_map_t *map, ssi_entry_t ***out);
void ssi_map_clear(ssi_map_t *map);
void ssi_mesh_retain(ssi_mesh_t *mesh);
void ssi_mesh_release(ssi_mesh_t *mesh);
void ssi_mesh_view(const ssi_mesh_t *mesh, ss_mesh_view_t *view);
bool ssi_mesh_equal(const ssi_mesh_t *a, const ssi_mesh_t *b);
ss_status_t ssi_mesh_decode(ssi_memory_t *memory, const uint8_t *raw, size_t size,
                            ssi_mesh_t **out);
ss_status_t ssi_mesh_canonicalize(ssi_memory_t *memory, const ss_mesh_view_t *input,
                                  ssi_mesh_t **out);
ss_status_t ssi_mesh_encode(ssi_buffer_t *buffer, const ssi_mesh_t *mesh, uint64_t checkpoint);
bool ssi_triangle_degenerate(const uint16_t *a, const uint16_t *b, const uint16_t *c);
ss_status_t ssi_partition(ssi_memory_t *memory, const ss_triangle_t *triangles, size_t count,
                          ss_mesh_set_t **out);

ss_status_t ssi_camera_validate(const ss_camera_t *camera, bool first, bool canonical);
ss_status_t ssi_camera_decode(const uint8_t *raw, size_t size, uint64_t timestamp,
                              ss_camera_t *camera);
void ssi_camera_encode(uint8_t raw[64], const ss_camera_t *camera);
void ssi_rotation(const ss_camera_t *camera, double r[9]);
ss_vec3_t ssi_rotate(const double r[9], ss_vec3_t p);
ss_vec3_t ssi_inverse_rotate(const double r[9], ss_vec3_t p);
ss_vec3_t ssi_sub(ss_vec3_t a, ss_vec3_t b);
ss_vec3_t ssi_cross(ss_vec3_t a, ss_vec3_t b);
double ssi_dot(ss_vec3_t a, ss_vec3_t b);
bool ssi_finite3(ss_vec3_t v);
ss_status_t ssi_depth_validate_raw(const uint8_t *raw, size_t size);
ss_status_t ssi_depth_encode(ssi_buffer_t *buffer, const ss_depth_view_t *depth);
ss_status_t ssi_document_parse(ssi_memory_t *memory, uint8_t *data, size_t size, bool owned,
                               ss_document_t **out, ss_error_t *error);
size_t ssi_find_frame(const ss_document_t *document, uint64_t time);

#endif
