#ifndef SPATIALSNAPSHOT_H
#define SPATIALSNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SS_SHARED)
#if defined(SS_BUILDING_LIBRARY)
#define SS_API __declspec(dllexport)
#else
#define SS_API __declspec(dllimport)
#endif
#else
#define SS_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define SS_VERSION UINT32_C(0x00010000)
#define SS_ABI_VERSION ((uint32_t)0x00010000)
#define SSPS_VERSION_MAJOR 1
#define SSPS_VERSION_MINOR 0
#define SSPS_STREAM_HEADER_SIZE 64
#define SSPS_PACKET_HEADER_SIZE 48
#define SSPS_MAX_HEADER_BYTES 4096
#define SSPS_MAX_PACKET_BYTES UINT32_C(16777216)
#define SSPS_MAX_SCENE_CELLS UINT32_C(262144)
#define SSPS_MAX_VERTICES_PER_CHUNK UINT32_C(65535)
#define SSPS_MAX_TRIANGLES_PER_CHUNK UINT32_C(262144)
#define SSPS_TIME_UNITS_PER_SECOND UINT64_C(1000000000)
#define SSPS_CHECKPOINT_MAX_DELTA_NS UINT64_C(30000000000)
#define SSPS_CHECKPOINT_MAX_DELTA_BYTES UINT64_C(33554432)
#define SSPS_GEOMETRY_CELL_EDGE_M 0.5
#define SSPS_DEPTH_UNIT_M 0.001
#ifdef __cplusplus
#define SS_INIT(type) {(uint32_t)sizeof(type), SS_ABI_VERSION}
#else
#define SS_INIT(type) {.struct_size = (uint32_t)sizeof(type), .abi_version = SS_ABI_VERSION}
#endif

typedef int32_t ss_status_t;
#define SS_OK ((ss_status_t)0)
#define SS_INVALID_ARGUMENT ((ss_status_t) - 1)
#define SS_MALFORMED ((ss_status_t) - 2)
#define SS_UNSUPPORTED ((ss_status_t) - 3)
#define SS_UNSUPPORTED_CRITICAL_PACKET ((ss_status_t) - 4)
#define SS_RESOURCE_LIMIT ((ss_status_t) - 5)
#define SS_OUT_OF_MEMORY ((ss_status_t) - 6)
#define SS_IO_ERROR ((ss_status_t) - 7)
#define SS_NO_SAMPLE ((ss_status_t) - 8)
#define SS_NOT_FOUND ((ss_status_t) - 9)
#define SS_OUT_OF_RANGE ((ss_status_t) - 10)
#define SS_INVALID_STATE ((ss_status_t) - 11)
#define SS_BINDING_UNREPRESENTABLE ((ss_status_t) - 12)
#define SS_DECODER_UNAVAILABLE ((ss_status_t) - 13)

typedef uint32_t ss_document_kind_t;
#define SS_STILL ((ss_document_kind_t)1)
#define SS_VIDEO ((ss_document_kind_t)2)
typedef uint32_t ss_geometry_class_t;
#define SS_CLASS_UNKNOWN ((ss_geometry_class_t)0)
#define SS_CLASS_WALL ((ss_geometry_class_t)1)
#define SS_CLASS_FLOOR ((ss_geometry_class_t)2)
#define SS_CLASS_CEILING ((ss_geometry_class_t)3)
#define SS_CLASS_TABLE ((ss_geometry_class_t)4)
#define SS_CLASS_SEAT ((ss_geometry_class_t)5)
#define SS_CLASS_WINDOW ((ss_geometry_class_t)6)
#define SS_CLASS_DOOR ((ss_geometry_class_t)7)
#define SS_CLASS_OTHER ((ss_geometry_class_t)8)

#define SS_PACKET_SCENE_INFO UINT16_C(0x0001)
#define SS_PACKET_CAMERA_SAMPLE UINT16_C(0x0002)
#define SS_PACKET_CHECKPOINT_BEGIN UINT16_C(0x0010)
#define SS_PACKET_GEOMETRY_PUT UINT16_C(0x0011)
#define SS_PACKET_GEOMETRY_REMOVE UINT16_C(0x0012)
#define SS_PACKET_CHECKPOINT_END UINT16_C(0x0013)
#define SS_PACKET_DEPTH_SAMPLE UINT16_C(0x0020)
#define SS_PACKET_STREAM_END UINT16_C(0x00ff)
#define SS_FLAG_LZ4 UINT32_C(1)
#define SS_FLAG_CRITICAL UINT32_C(2)
#define SS_GEOMETRY_PUT ((uint32_t)1)
#define SS_GEOMETRY_REMOVE ((uint32_t)2)

typedef struct ss_document ss_document_t;
typedef struct ss_scene_cursor ss_scene_cursor_t;
typedef struct ss_decoder ss_decoder_t;
typedef struct ss_writer ss_writer_t;
typedef struct ss_mesh_set ss_mesh_set_t;

/* Fixed-layout mathematical tuples; these are not extensible records.
 * binary64 scene coordinates preserve the u16 grid at large signed cell keys. */
typedef struct ss_vec2 {
    double x, y;
} ss_vec2_t;
typedef struct ss_vec3 {
    double x, y, z;
} ss_vec3_t;
typedef struct ss_cell_key {
    int32_t x, y, z;
} ss_cell_key_t;

typedef struct ss_allocator {
    uint32_t struct_size, abi_version;
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*deallocate)(void *context, void *pointer);
} ss_allocator_t;

typedef struct ss_open_options {
    uint32_t struct_size, abi_version;
    uint64_t max_memory_bytes;     /* 0: 256 MiB, includes owned input and indexes */
    uint64_t max_geometry_bytes;   /* 0: 128 MiB per geometry state */
    uint64_t max_packets;          /* 0: 1,000,000 */
    uint64_t max_partition_work;   /* 0: 1,000,000 triangle/cell intersections */
    uint32_t max_scene_cells;      /* 0: format maximum */
    uint32_t max_packet_raw_bytes; /* 0: format maximum, known payloads only */
    ss_allocator_t allocator;      /* both callbacks NULL: malloc/free */
} ss_open_options_t;

typedef struct ss_error {
    uint32_t struct_size, abi_version;
    ss_status_t status;
    uint32_t packet_type;
    uint64_t byte_offset, sequence_number;
    char message[160];
} ss_error_t;

typedef struct ss_io {
    uint32_t struct_size, abi_version;
    void *context;
    ss_status_t (*read_at)(void *context, uint64_t offset, void *destination, size_t bytes);
    uint64_t (*size)(void *context);
} ss_io_t;

typedef struct ss_camera {
    uint32_t struct_size, abi_version;
    uint64_t timestamp_ns;
    uint32_t raster_width, raster_height;
    float fx, fy, cx, cy;
    float tx, ty, tz;
    float qx, qy, qz, qw;
} ss_camera_t;

/* Array pointers are native-endian, borrowed, and valid until the owning
 * cursor is sought/released, or the mesh set is released. */
typedef struct ss_mesh_view {
    uint32_t struct_size, abi_version;
    ss_cell_key_t cell;
    uint32_t vertex_count, triangle_count;
    const uint16_t *quantized_xyz;    /* 3 * vertex_count */
    const uint16_t *triangle_indices; /* 3 * triangle_count */
    const uint8_t *classifications;   /* triangle_count, values 0...8 */
} ss_mesh_view_t;

typedef struct ss_depth_view {
    uint32_t struct_size, abi_version;
    uint64_t timestamp_ns;
    uint32_t width, height;
    float fx, fy, cx, cy;
    const uint16_t *depth_mm;  /* width * height, native-endian */
    const uint8_t *confidence; /* one UNPACKED value 0...3 per pixel */
} ss_depth_view_t;

typedef struct ss_packet_info {
    uint32_t struct_size, abi_version;
    uint64_t sequence_number, timestamp_ns, byte_offset;
    uint32_t type, flags, header_size, stored_payload_size, raw_payload_size;
} ss_packet_info_t;

typedef struct ss_document_info {
    uint32_t struct_size, abi_version;
    uint32_t version_major, version_minor;
    ss_document_kind_t kind;
    uint8_t stream_id[16];
    uint64_t duration_ns, packet_count, camera_count, checkpoint_count;
    ss_vec3_t gravity;
} ss_document_info_t;

typedef struct ss_ray {
    uint32_t struct_size, abi_version;
    ss_vec3_t origin, direction;       /* direction need not be normalized */
    double min_distance, max_distance; /* meters; 0 max means unbounded */
} ss_ray_t;

typedef struct ss_hit {
    uint32_t struct_size, abi_version;
    ss_vec3_t position, normal;
    double distance;
    ss_cell_key_t cell;
    uint32_t triangle_index;
    ss_geometry_class_t classification;
} ss_hit_t;

typedef ss_status_t (*ss_random_bytes_fn)(void *context, uint8_t *destination, size_t bytes);
typedef struct ss_writer_options {
    uint32_t struct_size, abi_version;
    ss_document_kind_t kind;
    ss_vec3_t gravity;
    /* REQUIRED: host-supplied cryptographically strong randomness. C core
     * deliberately has no OS/POSIX dependency or weak random fallback. */
    ss_random_bytes_fn random_bytes;
    void *random_context;
    ss_open_options_t resources;
} ss_writer_options_t;

typedef struct ss_geometry_update {
    uint32_t struct_size, abi_version;
    uint32_t operation;
    ss_mesh_view_t mesh; /* REMOVE only uses mesh.cell */
} ss_geometry_update_t;

typedef struct ss_triangle {
    uint32_t struct_size, abi_version;
    ss_vec3_t vertices[3];
    ss_geometry_class_t classification;
} ss_triangle_t;

SS_API uint32_t ss_version(void);
SS_API const char *ss_status_string(ss_status_t status);
SS_API void ss_open_options_init(ss_open_options_t *options);
SS_API void ss_writer_options_init(ss_writer_options_t *options);
SS_API void ss_camera_init(ss_camera_t *camera);
SS_API uint32_t ss_crc32c(const void *bytes, size_t size);

/* A document is published only after the entire stream is validated.
 * Inputs are copied; no input/callback lifetime extends beyond open(). */
SS_API ss_status_t ss_document_open(const ss_io_t *io, const ss_open_options_t *options,
                                    ss_document_t **out_document, ss_error_t *error);
SS_API ss_status_t ss_document_open_memory(const void *bytes, size_t size,
                                           const ss_open_options_t *options,
                                           ss_document_t **out_document, ss_error_t *error);
SS_API void ss_document_retain(ss_document_t *document);
SS_API void ss_document_release(ss_document_t *document);
SS_API ss_status_t ss_document_get_info(const ss_document_t *document, ss_document_info_t *info);
SS_API ss_status_t ss_document_camera(const ss_document_t *document, uint64_t index,
                                      ss_camera_t *camera);
SS_API ss_status_t ss_document_packet(const ss_document_t *document, uint64_t index,
                                      ss_packet_info_t *info);
/* Returns stored bytes, including for unknown packets; never decompresses an extension. */
SS_API ss_status_t ss_document_packet_payload(const ss_document_t *document, uint64_t index,
                                              const uint8_t **bytes, size_t *size);
SS_API ss_status_t ss_validate(const void *bytes, size_t size, const ss_open_options_t *options,
                               ss_error_t *error);

/* Arbitrary physical byte fragmentation. No scenes escape before finish().
 * finish() transfers the buffered input into a validated immutable document. */
SS_API ss_status_t ss_decoder_create(const ss_open_options_t *options, ss_decoder_t **out_decoder);
SS_API ss_status_t ss_decoder_feed(ss_decoder_t *decoder, const void *bytes, size_t size);
SS_API ss_status_t ss_decoder_finish(ss_decoder_t *decoder, ss_document_t **out_document,
                                     ss_error_t *error);
SS_API void ss_decoder_release(ss_decoder_t *decoder);

SS_API ss_status_t ss_scene_cursor_create(ss_document_t *document, ss_scene_cursor_t **out_cursor);
SS_API void ss_scene_cursor_release(ss_scene_cursor_t *cursor);
/* G(t) is available throughout [0,duration]. Camera/depth are exact samples:
 * no interpolation or hold-last-sample is implied. A failed seek is atomic. */
SS_API ss_status_t ss_scene_cursor_seek(ss_scene_cursor_t *cursor, uint64_t timestamp_ns);
SS_API ss_status_t ss_scene_cursor_camera(const ss_scene_cursor_t *cursor, ss_camera_t *camera);
SS_API uint32_t ss_scene_cursor_chunk_count(const ss_scene_cursor_t *cursor);
SS_API ss_status_t ss_scene_cursor_chunk(ss_scene_cursor_t *cursor, uint32_t index,
                                         ss_mesh_view_t *mesh);
SS_API ss_status_t ss_scene_cursor_depth(ss_scene_cursor_t *cursor, ss_depth_view_t *depth);
SS_API ss_status_t ss_mesh_vertex(const ss_mesh_view_t *mesh, uint32_t index, ss_vec3_t *point);
SS_API ss_status_t ss_camera_project(const ss_camera_t *camera, const ss_vec3_t *scene_point,
                                     ss_vec2_t *pixel);
SS_API ss_status_t ss_camera_unproject(const ss_camera_t *camera, const ss_vec2_t *pixel,
                                       double depth_m, ss_vec3_t *scene_point);
SS_API ss_status_t ss_camera_ray(const ss_camera_t *camera, const ss_vec2_t *pixel, ss_ray_t *ray);
SS_API ss_status_t ss_scene_raycast(ss_scene_cursor_t *cursor, const ss_ray_t *ray, ss_hit_t *hit);
SS_API ss_status_t ss_scene_raycast_pixel(ss_scene_cursor_t *cursor, const ss_vec2_t *pixel,
                                          ss_hit_t *hit);
SS_API ss_status_t ss_scene_depth_point(ss_scene_cursor_t *cursor, uint32_t u, uint32_t v,
                                        ss_vec3_t *point, uint32_t *confidence);
SS_API ss_status_t ss_scene_point_visible(ss_scene_cursor_t *cursor, const ss_vec3_t *point,
                                          double tolerance_m, uint32_t *visible);

/* Clips scene-space triangles to the fixed grid, rounds per SSPS 12.2,
 * removes collapsed fragments, and canonicalizes vertices and triangles. */
SS_API ss_status_t ss_mesh_partition(const ss_triangle_t *triangles, size_t count,
                                     const ss_open_options_t *options, ss_mesh_set_t **out_set);
SS_API void ss_mesh_set_release(ss_mesh_set_t *set);
SS_API uint32_t ss_mesh_set_count(const ss_mesh_set_t *set);
SS_API ss_status_t ss_mesh_set_chunk(const ss_mesh_set_t *set, uint32_t index,
                                     ss_mesh_view_t *mesh);

SS_API ss_status_t ss_writer_create(const ss_writer_options_t *options, ss_writer_t **out_writer);
/* One complete frame/batch. Meshes and batches may be unsorted: the writer
 * canonicalizes them and automatically emits checkpoints at the v1 cadence.
 * Calls are atomic. All array data is copied before return. */
SS_API ss_status_t ss_writer_append_frame(ss_writer_t *writer, const ss_camera_t *camera,
                                          const ss_geometry_update_t *updates, size_t update_count,
                                          const ss_depth_view_t *depth);
SS_API ss_status_t ss_writer_finish(ss_writer_t *writer, uint64_t duration_ns);
SS_API ss_status_t ss_writer_bytes(const ss_writer_t *writer, const uint8_t **bytes, size_t *size);
SS_API void ss_writer_release(ss_writer_t *writer);
SS_API ss_status_t ss_document_trim(const ss_document_t *document, uint64_t start_ns,
                                    uint64_t end_ns, const ss_writer_options_t *options,
                                    ss_writer_t **out_writer);

/* Container adapters supply already normalized presentation metadata. */
SS_API ss_status_t ss_time_to_nanoseconds(uint64_t value, uint32_t timescale,
                                          uint64_t *nanoseconds);
SS_API ss_status_t ss_document_validate_media_binding(const ss_document_t *document, uint32_t width,
                                                      uint32_t height,
                                                      const uint64_t *presentation_times_ns,
                                                      size_t frame_count, uint64_t duration_ns);

#ifdef __cplusplus
}
#endif

#include "bindings.h"
#endif
