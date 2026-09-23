#include <spatialsnapshot/spatialsnapshot.h>
#include "../../Sources/SpatialSnapshotC/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef SS_HAVE_LZ4
#include <lz4.h>
#endif

static unsigned checks = 0;
#define EXPECT(condition)                                                                          \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define OK(expression)                                                                             \
    do {                                                                                           \
        ss_status_t status_ = (expression);                                                        \
        ++checks;                                                                                  \
        if (status_ != SS_OK) {                                                                    \
            fprintf(stderr, "%s:%d: %s => %s\n", __FILE__, __LINE__, #expression,                  \
                    ss_status_string(status_));                                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define NEAR(a, b, tolerance) EXPECT(fabs((double)(a) - (double)(b)) <= (tolerance))

/* Deterministic fixture entropy is test-only; production supplies a CSPRNG. */
static ss_status_t fixture_random(void *context, uint8_t *bytes, size_t n) {
    uint8_t *next = context;
    for (size_t i = 0; i < n; ++i)
        bytes[i] = ++*next;
    return SS_OK;
}
static ss_writer_options_t writer_options(uint32_t kind, uint8_t *entropy) {
    ss_writer_options_t o;
    ss_writer_options_init(&o);
    o.kind = kind;
    o.random_bytes = fixture_random;
    o.random_context = entropy;
    return o;
}
static ss_camera_t camera(uint64_t time) {
    ss_camera_t c;
    ss_camera_init(&c);
    c.timestamp_ns = time;
    c.raster_width = 4;
    c.raster_height = 3;
    c.fx = 2;
    c.fy = 3;
    c.cx = 1.5f;
    c.cy = 1;
    return c;
}
static uint8_t *fixture(const char *name, size_t *size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", SS_FIXTURES_DIR, name);
    FILE *file = fopen(path, "rb");
    EXPECT(file != NULL);
    EXPECT(fseek(file, 0, SEEK_END) == 0);
    long n = ftell(file);
    EXPECT(n >= 0);
    rewind(file);
    uint8_t *data = malloc((size_t)n);
    EXPECT(data != NULL);
    EXPECT(fread(data, 1, (size_t)n, file) == (size_t)n);
    fclose(file);
    *size = (size_t)n;
    return data;
}
static ss_document_t *open_fixture(const char *name) {
    size_t n;
    uint8_t *data = fixture(name, &n);
    ss_document_t *d = NULL;
    ss_error_t e = SS_INIT(ss_error_t);
    ss_status_t s = ss_document_open_memory(data, n, NULL, &d, &e);
    if (s != SS_OK)
        fprintf(stderr, "fixture %s: %s (%s)\n", name, ss_status_string(s), e.message);
    EXPECT(s == SS_OK);
    free(data);
    return d;
}
static ss_document_t *open_writer(ss_writer_t *writer) {
    const uint8_t *data = NULL;
    size_t n = 0;
    OK(ss_writer_bytes(writer, &data, &n));
    ss_document_t *d = NULL;
    OK(ss_document_open_memory(data, n, NULL, &d, NULL));
    return d;
}
static ss_geometry_update_t mesh_update(ss_cell_key_t key) {
    static const uint16_t q[] = {0, 0, 0, 0, 65535, 0, 65535, 0, 0};
    static const uint16_t indices[] = {0, 2, 1};
    static const uint8_t classes[] = {4};
    ss_geometry_update_t u = {.struct_size = sizeof(u),
                              .abi_version = SS_ABI_VERSION,
                              .operation = SS_GEOMETRY_PUT,
                              .mesh = {.struct_size = sizeof(ss_mesh_view_t),
                                       .abi_version = SS_ABI_VERSION,
                                       .cell = key,
                                       .vertex_count = 3,
                                       .triangle_count = 1,
                                       .quantized_xyz = q,
                                       .triangle_indices = indices,
                                       .classifications = classes}};
    return u;
}
static ss_depth_view_t depth_sample(uint64_t time) {
    static const uint16_t values[] = {1000};
    static const uint8_t confidence[] = {3};
    ss_depth_view_t d = {.struct_size = sizeof(d),
                         .abi_version = SS_ABI_VERSION,
                         .timestamp_ns = time,
                         .width = 1,
                         .height = 1,
                         .fx = 1,
                         .fy = 1,
                         .depth_mm = values,
                         .confidence = confidence};
    return d;
}

static void test_golden(void) {
    EXPECT(ss_version() == SS_VERSION);
    EXPECT(ss_crc32c(NULL, 0) == 0);
    EXPECT(ss_crc32c("123456789", 9) == 0xe3069283);
    const char *names[] = {"v1/minimal-still.ssps", "v1/mesh-depth-still.ssps",
                           "v1/minimal-video.ssps"};
    for (unsigned test = 0; test < 3; ++test) {
        uint8_t entropy = 0;
        ss_writer_options_t options = writer_options(test == 2 ? SS_VIDEO : SS_STILL, &entropy);
        ss_writer_t *w = NULL;
        OK(ss_writer_create(&options, &w));
        ss_camera_t c = camera(0);
        ss_geometry_update_t u = mesh_update((ss_cell_key_t){0, 0, 2});
        ss_depth_view_t depth = depth_sample(0);
        OK(ss_writer_append_frame(w, &c, test == 1 ? &u : NULL, test == 1 ? 1 : 0,
                                  test == 1 ? &depth : NULL));
        if (test == 2) {
            c.timestamp_ns = 1000000000;
            c.tx = 0.25f;
            OK(ss_writer_append_frame(w, &c, NULL, 0, NULL));
            c.timestamp_ns = 2000000000;
            c.tx = 0.5f;
            OK(ss_writer_append_frame(w, &c, NULL, 0, NULL));
        }
        OK(ss_writer_finish(w, test == 2 ? 3000000000 : 0));
        const uint8_t *actual = NULL;
        size_t n = 0, expected_n;
        OK(ss_writer_bytes(w, &actual, &n));
        uint8_t *expected = fixture(names[test], &expected_n);
        EXPECT(n == expected_n);
        EXPECT(!memcmp(actual, expected, n));
        ss_document_t *d = open_writer(w);
        ss_document_info_t info = SS_INIT(ss_document_info_t);
        OK(ss_document_get_info(d, &info));
        EXPECT(info.kind == (test == 2 ? SS_VIDEO : SS_STILL));
        EXPECT(info.checkpoint_count == 1);
        free(expected);
        ss_document_release(d);
        ss_writer_release(w);
    }
}

static void test_queries(void) {
    ss_document_t *d = open_fixture("v1/mesh-depth-still.ssps");
    ss_scene_cursor_t *cursor = NULL;
    OK(ss_scene_cursor_create(d, &cursor));
    ss_document_release(d); /* cursor retains document */
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 1);
    ss_mesh_view_t mesh = SS_INIT(ss_mesh_view_t);
    OK(ss_scene_cursor_chunk(cursor, 0, &mesh));
    ss_vec3_t p;
    OK(ss_mesh_vertex(&mesh, 2, &p));
    NEAR(p.x, 0.5, 1e-14);
    NEAR(p.z, 1, 1e-14);
    ss_camera_t c = camera(0);
    ss_vec2_t pixel = {1.75, 1.75};
    OK(ss_camera_unproject(&c, &pixel, 1, &p));
    NEAR(p.x, 0.125, 1e-14);
    NEAR(p.y, 0.25, 1e-14);
    ss_vec2_t projected;
    OK(ss_camera_project(&c, &p, &projected));
    NEAR(projected.x, pixel.x, 1e-14);
    NEAR(projected.y, pixel.y, 1e-14);
    ss_hit_t hit = SS_INIT(ss_hit_t);
    OK(ss_scene_raycast_pixel(cursor, &pixel, &hit));
    NEAR(hit.position.z, 1, 1e-12);
    NEAR(hit.normal.z, 1, 1e-12);
    EXPECT(hit.classification == SS_CLASS_TABLE);
    uint32_t visible = 0;
    OK(ss_scene_point_visible(cursor, &p, 0, &visible));
    EXPECT(visible == 1);
    ss_vec3_t behind = {p.x * 2, p.y * 2, 2};
    OK(ss_scene_point_visible(cursor, &behind, 0.001, &visible));
    EXPECT(visible == 0);
    ss_ray_t ray = {.struct_size = sizeof(ray),
                    .abi_version = SS_ABI_VERSION,
                    .origin = {0.125, 0.25, 0},
                    .direction = {0, 0, 17},
                    .max_distance = 2};
    OK(ss_scene_raycast(cursor, &ray, &hit));
    NEAR(hit.distance, 1, 1e-12);
    ray.max_distance = 0.9;
    EXPECT(ss_scene_raycast(cursor, &ray, &hit) == SS_NOT_FOUND);
    ray.direction = (ss_vec3_t){0, 0, 0};
    EXPECT(ss_scene_raycast(cursor, &ray, &hit) == SS_INVALID_ARGUMENT);
    ss_depth_view_t depth = SS_INIT(ss_depth_view_t);
    OK(ss_scene_cursor_depth(cursor, &depth));
    EXPECT(depth.depth_mm[0] == 1000 && depth.confidence[0] == 3);
    uint32_t confidence;
    OK(ss_scene_depth_point(cursor, 0, 0, &p, &confidence));
    NEAR(p.z, 1, 1e-14);
    EXPECT(confidence == 3);
    EXPECT(ss_scene_depth_point(cursor, 1, 0, &p, &confidence) == SS_OUT_OF_RANGE);
    EXPECT(ss_scene_cursor_seek(cursor, 1) == SS_OUT_OF_RANGE);
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 1);
    ss_scene_cursor_release(cursor);

    c = camera(0);
    c.tx = 10;
    c.ty = -4;
    c.tz = 3;
    c.qy = (float)sqrt(0.5);
    c.qw = (float)sqrt(0.5);
    pixel = (ss_vec2_t){2, 2};
    OK(ss_camera_unproject(&c, &pixel, 2, &p));
    OK(ss_camera_project(&c, &p, &projected));
    NEAR(projected.x, pixel.x, 1e-12);
    NEAR(projected.y, pixel.y, 1e-12);
    c.qw *= 1.00001f;
    OK(ss_camera_unproject(&c, &pixel, 2, &p));
    OK(ss_camera_project(&c, &p, &projected));
    NEAR(projected.x, pixel.x, 1e-12);
    NEAR(projected.y, pixel.y, 1e-12);
    c.fx = NAN;
    EXPECT(ss_camera_project(&c, &p, &projected) == SS_INVALID_ARGUMENT);
}

static void test_timeline(void) {
    ss_document_t *d = open_fixture("v1/geometry-video.ssps");
    ss_scene_cursor_t *cursor = NULL;
    OK(ss_scene_cursor_create(d, &cursor));
    ss_camera_t c = SS_INIT(ss_camera_t);
    ss_depth_view_t depth = SS_INIT(ss_depth_view_t);
    uint64_t times[] = {2000000000, 0, 1000000000, 1500000000, 3000000000};
    uint32_t counts[] = {1, 1, 2, 2, 1};
    for (unsigned i = 0; i < 5; ++i) {
        OK(ss_scene_cursor_seek(cursor, times[i]));
        EXPECT(ss_scene_cursor_chunk_count(cursor) == counts[i]);
        if (i >= 3)
            EXPECT(ss_scene_cursor_camera(cursor, &c) == SS_NO_SAMPLE);
        else {
            OK(ss_scene_cursor_camera(cursor, &c));
            EXPECT(c.timestamp_ns == times[i]);
        }
        if (times[i])
            EXPECT(ss_scene_cursor_depth(cursor, &depth) == SS_NO_SAMPLE);
    }
    uint64_t media_times[] = {0, 1000000000, 2000000000};
    OK(ss_document_validate_media_binding(d, 4, 3, media_times, 3, 3000000000));
    EXPECT(ss_document_validate_media_binding(d, 3, 4, media_times, 3, 3000000000) == SS_MALFORMED);
    media_times[1]++;
    EXPECT(ss_document_validate_media_binding(d, 4, 3, media_times, 3, 3000000000) == SS_MALFORMED);
    ss_scene_cursor_release(cursor);
    ss_document_release(d);
    d = open_fixture("v1/later-empty-checkpoint.ssps");
    OK(ss_scene_cursor_create(d, &cursor));
    OK(ss_scene_cursor_seek(cursor, 40000000000));
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 0);
    OK(ss_scene_cursor_seek(cursor, 39999999999));
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 1);
    ss_scene_cursor_release(cursor);
    ss_document_release(d);
    uint64_t time;
    OK(ss_time_to_nanoseconds(1, 30, &time));
    EXPECT(time == 33333333);
    OK(ss_time_to_nanoseconds(2, 30, &time));
    EXPECT(time == 66666667);
    OK(ss_time_to_nanoseconds(1, 2000000000, &time));
    EXPECT(time == 1);
    OK(ss_time_to_nanoseconds(UINT64_MAX, 1000000000, &time));
    EXPECT(time == UINT64_MAX);
    EXPECT(ss_time_to_nanoseconds(UINT64_MAX, 1, &time) == SS_OUT_OF_RANGE);
    EXPECT(ss_time_to_nanoseconds(1, 0, &time) == SS_INVALID_ARGUMENT);
}

static void test_streaming(void) {
    size_t n;
    uint8_t *data = fixture("v1/mesh-depth-still.ssps", &n);
    for (size_t fragment = 1; fragment < 80; fragment += 13) {
        ss_decoder_t *decoder = NULL;
        OK(ss_decoder_create(NULL, &decoder));
        for (size_t i = 0; i < n;) {
            size_t bytes = n - i < fragment ? n - i : fragment;
            OK(ss_decoder_feed(decoder, data + i, bytes));
            i += bytes;
        }
        ss_document_t *d = NULL;
        OK(ss_decoder_finish(decoder, &d, NULL));
        EXPECT(ss_decoder_feed(decoder, data, 1) == SS_INVALID_STATE);
        ss_document_t *again = (ss_document_t *)1;
        EXPECT(ss_decoder_finish(decoder, &again, NULL) == SS_INVALID_STATE);
        EXPECT(again == NULL);
        ss_decoder_release(decoder);
        ss_document_release(d);
    }
    ss_decoder_t *decoder = NULL;
    OK(ss_decoder_create(NULL, &decoder));
    OK(ss_decoder_feed(decoder, data, n - 1));
    ss_document_t *d = (ss_document_t *)1;
    EXPECT(ss_decoder_finish(decoder, &d, NULL) == SS_MALFORMED);
    EXPECT(d == NULL);
    ss_decoder_release(decoder);
    free(data);
}

static double mesh_area(const ss_mesh_view_t *mesh) {
    double sum = 0;
    for (uint32_t i = 0; i < mesh->triangle_count; ++i) {
        ss_vec3_t v[3];
        for (unsigned j = 0; j < 3; ++j)
            OK(ss_mesh_vertex(mesh, mesh->triangle_indices[(size_t)3 * i + j], &v[j]));
        ss_vec3_t n = ssi_cross(ssi_sub(v[1], v[0]), ssi_sub(v[2], v[0]));
        sum += sqrt(ssi_dot(n, n)) / 2;
    }
    return sum;
}
static void test_partition(void) {
    ss_triangle_t triangle = {.struct_size = sizeof(triangle),
                              .abi_version = SS_ABI_VERSION,
                              .vertices = {{-0.25, -0.25, 1}, {0.75, -0.25, 1}, {-0.25, 0.75, 1}},
                              .classification = SS_CLASS_FLOOR};
    ss_mesh_set_t *set = NULL;
    OK(ss_mesh_partition(&triangle, 1, NULL, &set));
    EXPECT(ss_mesh_set_count(set) > 1);
    double area = 0;
    ss_cell_key_t previous = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (uint32_t i = 0; i < ss_mesh_set_count(set); ++i) {
        ss_mesh_view_t mesh = SS_INIT(ss_mesh_view_t);
        OK(ss_mesh_set_chunk(set, i, &mesh));
        EXPECT(ssi_key_compare(previous, mesh.cell) < 0);
        previous = mesh.cell;
        area += mesh_area(&mesh);
        for (uint32_t t = 0; t < mesh.triangle_count; ++t)
            EXPECT(mesh.classifications[t] == SS_CLASS_FLOOR);
    }
    NEAR(area, 0.5, 3e-5);
    uint8_t entropy = 0;
    ss_writer_options_t options = writer_options(SS_STILL, &entropy);
    ss_writer_t *w = NULL;
    OK(ss_writer_create(&options, &w));
    size_t n = ss_mesh_set_count(set);
    ss_geometry_update_t *updates = calloc(n, sizeof(*updates));
    EXPECT(updates != NULL);
    for (size_t i = 0; i < n; ++i) {
        updates[i] = (ss_geometry_update_t)SS_INIT(ss_geometry_update_t);
        updates[i].operation = SS_GEOMETRY_PUT;
        updates[i].mesh = (ss_mesh_view_t)SS_INIT(ss_mesh_view_t);
        OK(ss_mesh_set_chunk(set, (uint32_t)(n - i - 1),
                             &updates[i].mesh)); /* unsorted API input */
    }
    ss_camera_t c = camera(0);
    OK(ss_writer_append_frame(w, &c, updates, n, NULL));
    OK(ss_writer_finish(w, 0));
    ss_document_t *d = open_writer(w);
    ss_document_release(d);
    ss_writer_release(w);
    free(updates);
    ss_mesh_set_release(set);
    triangle.vertices[1].x = NAN;
    EXPECT(ss_mesh_partition(&triangle, 1, NULL, &set) == SS_INVALID_ARGUMENT);
    EXPECT(set == NULL);
    triangle.vertices[1].x = 1e9;
    EXPECT(ss_mesh_partition(&triangle, 1, NULL, &set) == SS_RESOURCE_LIMIT);
    triangle.vertices[1].x = 2e9;
    EXPECT(ss_mesh_partition(&triangle, 1, NULL, &set) == SS_OUT_OF_RANGE);
    OK(ss_mesh_partition(NULL, 0, NULL, &set));
    EXPECT(ss_mesh_set_count(set) == 0);
    ss_mesh_set_release(set);
}

static void test_writer_atomic_and_cadence(void) {
    uint8_t entropy = 0;
    ss_writer_options_t options = writer_options(SS_VIDEO, &entropy);
    ss_writer_t *w = NULL;
    OK(ss_writer_create(&options, &w));
    ss_camera_t c = camera(0);
    const uint8_t *bytes = NULL;
    size_t n;
    EXPECT(ss_writer_bytes(w, &bytes, &n) == SS_INVALID_STATE);
    ss_geometry_update_t initial = mesh_update((ss_cell_key_t){0, 0, 2});
    OK(ss_writer_append_frame(w, &c, &initial, 1, NULL));
    c.timestamp_ns = 1;
    ss_geometry_update_t bad = mesh_update((ss_cell_key_t){9, 9, 9});
    bad.operation = SS_GEOMETRY_REMOVE;
    EXPECT(ss_writer_append_frame(w, &c, &bad, 1, NULL) == SS_INVALID_ARGUMENT);
    OK(ss_writer_append_frame(w, &c, NULL, 0, NULL));
    ss_geometry_update_t two[] = {mesh_update((ss_cell_key_t){0, 0, 3}),
                                  mesh_update((ss_cell_key_t){0, 0, 3})};
    c.timestamp_ns = 2;
    EXPECT(ss_writer_append_frame(w, &c, two, 2, NULL) == SS_INVALID_ARGUMENT);
    OK(ss_writer_append_frame(w, &c, two, 1, NULL));
    c.timestamp_ns = 30000000000;
    ss_geometry_update_t update = mesh_update((ss_cell_key_t){0, 0, 4});
    OK(ss_writer_append_frame(w, &c, &update, 1, NULL));
    c.timestamp_ns = 30000000001;
    update = mesh_update((ss_cell_key_t){0, 0, 5});
    OK(ss_writer_append_frame(w, &c, &update, 1, NULL));
    c.timestamp_ns = 90000000000;
    OK(ss_writer_append_frame(w, &c, &update, 1, NULL)); /* identical PUT is a no-op */
    EXPECT(ss_writer_finish(w, 1) == SS_INVALID_ARGUMENT);
    OK(ss_writer_finish(w, c.timestamp_ns + 1));
    EXPECT(ss_writer_finish(w, c.timestamp_ns + 1) == SS_INVALID_STATE);
    ss_document_t *d = open_writer(w);
    ss_document_info_t info = SS_INIT(ss_document_info_t);
    OK(ss_document_get_info(d, &info));
    EXPECT(info.checkpoint_count == 2);
    ss_scene_cursor_t *cursor = NULL;
    OK(ss_scene_cursor_create(d, &cursor));
    OK(ss_scene_cursor_seek(cursor, 30000000001));
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 4);
    ss_scene_cursor_release(cursor);
    ss_document_release(d);
    ss_writer_release(w);
}

static void test_trim(void) {
    ss_document_t *d = open_fixture("v1/geometry-video.ssps");
    uint8_t entropy = 80;
    ss_writer_options_t o = writer_options(SS_VIDEO, &entropy);
    ss_writer_t *trimmed = NULL;
    EXPECT(ss_document_trim(d, 1, 2000000000, &o, &trimmed) == SS_INVALID_ARGUMENT);
    EXPECT(trimmed == NULL);
    EXPECT(ss_document_trim(d, 1000000000, 2000000001, &o, &trimmed) == SS_INVALID_ARGUMENT);
    OK(ss_document_trim(d, 1000000000, 3000000000, &o, &trimmed));
    ss_document_t *new_document = open_writer(trimmed);
    ss_document_info_t info = SS_INIT(ss_document_info_t), original = SS_INIT(ss_document_info_t);
    OK(ss_document_get_info(new_document, &info));
    OK(ss_document_get_info(d, &original));
    EXPECT(info.duration_ns == 2000000000 && info.camera_count == 2);
    EXPECT(memcmp(info.stream_id, original.stream_id, 16) != 0);
    ss_camera_t c = SS_INIT(ss_camera_t);
    OK(ss_document_camera(new_document, 0, &c));
    EXPECT(c.tx == 0 && c.ty == 0 && c.tz == 0 && c.qw == 1);
    OK(ss_document_camera(new_document, 1, &c));
    NEAR(c.tx, 0.25, 1e-7);
    ss_scene_cursor_t *cursor = NULL;
    OK(ss_scene_cursor_create(new_document, &cursor));
    EXPECT(ss_scene_cursor_chunk_count(cursor) ==
           4); /* two surfaces cross the new x=0 grid boundary */
    ss_ray_t ray = {.struct_size = sizeof(ray),
                    .abi_version = SS_ABI_VERSION,
                    .origin = {-0.125, 0.125, 0},
                    .direction = {0, 0, 1}};
    ss_hit_t hit = SS_INIT(ss_hit_t);
    OK(ss_scene_raycast(cursor, &ray, &hit));
    NEAR(hit.position.z, 1, 1e-12);
    OK(ss_scene_cursor_seek(cursor, 1000000000));
    EXPECT(ss_scene_cursor_chunk_count(cursor) == 2);
    OK(ss_scene_raycast(cursor, &ray, &hit));
    NEAR(hit.position.z, 1.5, 1e-12);
    ss_scene_cursor_release(cursor);
    ss_document_release(new_document);
    ss_writer_release(trimmed);
    ss_document_release(d);

    /* Rotation, gravity, preserved camera-local depth, and a nonempty regrid. */
    entropy = 0;
    ss_writer_t *w = NULL;
    o = writer_options(SS_VIDEO, &entropy);
    OK(ss_writer_create(&o, &w));
    c = camera(0);
    ss_geometry_update_t u = mesh_update((ss_cell_key_t){0, 0, 2});
    OK(ss_writer_append_frame(w, &c, &u, 1, NULL));
    c.timestamp_ns = 100;
    c.qz = (float)sqrt(0.5);
    c.qw = (float)sqrt(0.5);
    ss_depth_view_t depth = depth_sample(100);
    OK(ss_writer_append_frame(w, &c, NULL, 0, &depth));
    c.timestamp_ns = 200;
    c.tx = 0.25f;
    OK(ss_writer_append_frame(w, &c, NULL, 0, NULL));
    OK(ss_writer_finish(w, 300));
    d = open_writer(w);
    OK(ss_document_trim(d, 100, 300, &o, &trimmed));
    new_document = open_writer(trimmed);
    OK(ss_document_get_info(new_document, &info));
    NEAR(info.gravity.x, 1, 1e-6);
    NEAR(info.gravity.y, 0, 1e-6);
    OK(ss_document_camera(new_document, 1, &c));
    NEAR(c.tx, 0, 1e-6);
    NEAR(c.ty, -0.25, 1e-6);
    OK(ss_scene_cursor_create(new_document, &cursor));
    depth = (ss_depth_view_t)SS_INIT(ss_depth_view_t);
    OK(ss_scene_cursor_depth(cursor, &depth));
    EXPECT(depth.timestamp_ns == 0 && depth.depth_mm[0] == 1000 && depth.confidence[0] == 3);
    double area = 0;
    for (uint32_t i = 0; i < ss_scene_cursor_chunk_count(cursor); ++i) {
        ss_mesh_view_t mesh = SS_INIT(ss_mesh_view_t);
        OK(ss_scene_cursor_chunk(cursor, i, &mesh));
        area += mesh_area(&mesh);
    }
    NEAR(area, 0.125, 1e-5);
    ss_scene_cursor_release(cursor);
    ss_document_release(new_document);
    ss_writer_release(trimmed);
    ss_document_release(d);
    ss_writer_release(w);
}

typedef struct fail_allocator {
    size_t calls, fail_at, live;
} fail_allocator_t;
static void *failing_allocate(void *context, size_t size) {
    fail_allocator_t *a = context;
    if (a->calls++ == a->fail_at)
        return NULL;
    void *p = malloc(size);
    if (p)
        ++a->live;
    return p;
}
static void failing_deallocate(void *context, void *p) {
    fail_allocator_t *a = context;
    if (p) {
        EXPECT(a->live > 0);
        --a->live;
        free(p);
    }
}
static void test_resources(void) {
    size_t n;
    uint8_t *data = fixture("v1/mesh-depth-still.ssps", &n);
    ss_document_t *d = NULL;
    ss_open_options_t options;
    ss_open_options_init(&options);
    struct {
        ss_open_options_t prefix;
        uint64_t future_field;
    } extended = {0};
    ss_open_options_init(&extended.prefix);
    extended.prefix.struct_size = sizeof(extended);
    extended.prefix.abi_version = 0x00010100;
    OK(ss_document_open_memory(data, n, &extended.prefix, &d, NULL));
    ss_document_release(d);
    extended.prefix.abi_version = 0x00020000;
    EXPECT(ss_document_open_memory(data, n, &extended.prefix, &d, NULL) == SS_INVALID_ARGUMENT);
    EXPECT(d == NULL);
    options.max_memory_bytes = 1;
    EXPECT(ss_document_open_memory(data, n, &options, &d, NULL) == SS_RESOURCE_LIMIT);
    EXPECT(d == NULL);
    options.max_memory_bytes = 0;
    options.max_geometry_bytes = 1;
    EXPECT(ss_document_open_memory(data, n, &options, &d, NULL) == SS_RESOURCE_LIMIT);
    options.max_geometry_bytes = 0;
    options.max_packet_raw_bytes = 32;
    EXPECT(ss_document_open_memory(data, n, &options, &d, NULL) == SS_RESOURCE_LIMIT);
    options.max_packet_raw_bytes = 0;
    options.max_packets = 2;
    EXPECT(ss_document_open_memory(data, n, &options, &d, NULL) == SS_RESOURCE_LIMIT);
    free(data);
    data = fixture("v1/unknown-compressed-skipped.ssps", &n);
    options.max_packets = 0;
    options.max_packet_raw_bytes = 64;
    OK(ss_document_open_memory(data, n, &options, &d, NULL));
    ss_document_release(d);
    free(data);
    data = fixture("v1/mesh-depth-still.ssps", &n);
    options.max_packet_raw_bytes = 0;
    for (size_t fail = 0; fail < 70; ++fail) {
        fail_allocator_t a = {.fail_at = fail};
        options.allocator.context = &a;
        options.allocator.allocate = failing_allocate;
        options.allocator.deallocate = failing_deallocate;
        ss_status_t status = ss_document_open_memory(data, n, &options, &d, NULL);
        EXPECT(status == SS_OK || status == SS_OUT_OF_MEMORY);
        if (status == SS_OK) {
            ss_scene_cursor_t *cursor = NULL;
            status = ss_scene_cursor_create(d, &cursor);
            EXPECT(status == SS_OK || status == SS_OUT_OF_MEMORY);
            if (cursor) {
                ss_hit_t hit = SS_INIT(ss_hit_t);
                ss_vec2_t p = {1.75, 1.75};
                status = ss_scene_raycast_pixel(cursor, &p, &hit);
                EXPECT(status == SS_OK || status == SS_OUT_OF_MEMORY);
                ss_depth_view_t depth = SS_INIT(ss_depth_view_t);
                status = ss_scene_cursor_depth(cursor, &depth);
                EXPECT(status == SS_OK || status == SS_OUT_OF_MEMORY);
                ss_scene_cursor_release(cursor);
            }
            ss_document_release(d);
        } else
            EXPECT(d == NULL);
        EXPECT(a.live == 0);
    }
    free(data);
    for (size_t fail = 0; fail < 100; ++fail) {
        uint8_t entropy = 0;
        fail_allocator_t a = {.fail_at = fail};
        ss_writer_options_t o = writer_options(SS_VIDEO, &entropy);
        o.resources.allocator.context = &a;
        o.resources.allocator.allocate = failing_allocate;
        o.resources.allocator.deallocate = failing_deallocate;
        ss_writer_t *w = NULL;
        ss_status_t s = ss_writer_create(&o, &w);
        if (s == SS_OK) {
            ss_camera_t c = camera(0);
            ss_geometry_update_t u = mesh_update((ss_cell_key_t){0, 0, 2});
            ss_depth_view_t depth = depth_sample(0);
            s = ss_writer_append_frame(w, &c, &u, 1, &depth);
            if (s == SS_OUT_OF_MEMORY) {
                a.fail_at = SIZE_MAX;
                OK(ss_writer_append_frame(w, &c, &u, 1, &depth));
            } else
                EXPECT(s == SS_OK);
            s = ss_writer_finish(w, 1);
            EXPECT(s == SS_OK || s == SS_OUT_OF_MEMORY);
            ss_writer_release(w);
        } else
            EXPECT(s == SS_OUT_OF_MEMORY);
        EXPECT(a.live == 0);
    }
    d = open_fixture("v1/geometry-video.ssps");
    for (size_t fail = 0; fail < 200; ++fail) {
        uint8_t entropy = 80;
        fail_allocator_t a = {.fail_at = fail};
        ss_writer_options_t o = writer_options(SS_VIDEO, &entropy);
        o.resources.allocator.context = &a;
        o.resources.allocator.allocate = failing_allocate;
        o.resources.allocator.deallocate = failing_deallocate;
        ss_writer_t *trimmed = NULL;
        ss_status_t s = ss_document_trim(d, 1000000000, 3000000000, &o, &trimmed);
        EXPECT(s == SS_OK || s == SS_OUT_OF_MEMORY);
        if (s != SS_OK)
            EXPECT(trimmed == NULL);
        ss_writer_release(trimmed);
        EXPECT(a.live == 0);
    }
    ss_document_release(d);
}

static void test_lz4(void) {
    uint8_t out[35];
    const uint8_t literal[] = {0x30, 'a', 'b', 'c'};
    OK(ssi_lz4_decode(literal, sizeof(literal), out, 3));
    EXPECT(!memcmp(out, "abc", 3));
    const uint8_t overlap[] = {0x1f, 'x', 1, 0, 10, 0x50, 'a', 'b', 'c', 'd', 'e'};
    OK(ssi_lz4_decode(overlap, sizeof(overlap), out, 35));
    for (unsigned i = 0; i < 30; ++i)
        EXPECT(out[i] == 'x');
    EXPECT(!memcmp(out + 30, "abcde", 5));
    const uint8_t bad_last_match[] = {0x10, 'x', 1, 0, 0x50, 'a', 'b', 'c', 'd', 'e'};
    EXPECT(ssi_lz4_decode(bad_last_match, sizeof(bad_last_match), out, 10) == SS_MALFORMED);
    const uint8_t empty[] = {0};
    OK(ssi_lz4_decode(empty, 1, NULL, 0));
#ifdef SS_HAVE_LZ4
    const size_t sizes[] = {0, 1, 4, 5, 12, 13, 255, 256, 4096, 65535, 1000000};
    for (unsigned pattern = 0; pattern < 3; ++pattern)
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            size_t n = sizes[i];
            uint8_t *raw = malloc(n + 1), *decoded = malloc(n + 1);
            EXPECT(raw && decoded);
            uint32_t rng = 17;
            for (size_t j = 0; j < n; ++j) {
                rng = rng * 1664525u + 1013904223u;
                raw[j] = pattern == 0   ? 0
                         : pattern == 1 ? (uint8_t)(j % 257)
                                        : (uint8_t)(rng >> 24);
            }
            int bound = LZ4_compressBound((int)n);
            char *block = malloc((size_t)bound);
            EXPECT(block != NULL);
            int compressed = LZ4_compress_default((const char *)raw, block, (int)n, bound);
            EXPECT(compressed > 0);
            OK(ssi_lz4_decode((const uint8_t *)block, (size_t)compressed, decoded, n));
            EXPECT(!memcmp(raw, decoded, n));
            ssi_memory_t *memory = NULL;
            OK(ssi_memory_create(NULL, &memory));
            ssi_buffer_t encoded = {.memory = memory};
            OK(ssi_lz4_encode(&encoded, raw, n));
            EXPECT(LZ4_decompress_safe((const char *)encoded.data, (char *)decoded,
                                       (int)encoded.size, (int)n) == (int)n);
            EXPECT(!memcmp(raw, decoded, n));
            ssi_buffer_clear(&encoded);
            ssi_memory_release(memory);
            free(raw);
            free(decoded);
            free(block);
        }
#endif
}

static void test_byte_cadence(void) {
    uint32_t vertices = 65535, triangles = vertices - 2;
    uint16_t *xyz = malloc((size_t)6 * vertices), *indices = malloc((size_t)6 * triangles);
    uint8_t *classes = malloc(triangles);
    EXPECT(xyz && indices && classes);
    for (uint32_t i = 0; i < vertices; ++i) {
        xyz[3 * i] = (uint16_t)((i % 256) * 257);
        xyz[3 * i + 1] = (uint16_t)((i / 256) * 257);
        xyz[3 * i + 2] = 0;
    }
    for (uint32_t i = 0; i < triangles; ++i) {
        indices[3 * i] = 0;
        indices[3 * i + 1] = (uint16_t)(i + 1);
        indices[3 * i + 2] = (uint16_t)(i + 2);
    }
    memset(classes, 4, triangles);
    ss_geometry_update_t u = mesh_update((ss_cell_key_t){0, 0, 2});
    u.mesh.vertex_count = vertices;
    u.mesh.triangle_count = triangles;
    u.mesh.quantized_xyz = xyz;
    u.mesh.triangle_indices = indices;
    u.mesh.classifications = classes;
    uint8_t entropy = 0;
    ss_writer_options_t options = writer_options(SS_VIDEO, &entropy);
    ss_writer_t *w = NULL;
    OK(ss_writer_create(&options, &w));
    ss_camera_t c = camera(0);
    OK(ss_writer_append_frame(w, &c, &u, 1, NULL));
    uint64_t raw_size = w->scene.bytes;
    EXPECT(raw_size > 800000);
    uint64_t boundary = SSPS_CHECKPOINT_MAX_DELTA_BYTES / raw_size;
    for (uint64_t i = 1; i <= boundary + 1; ++i) {
        memset(classes, i % 2 ? 5 : 4, triangles);
        c.timestamp_ns = i;
        OK(ss_writer_append_frame(w, &c, &u, 1, NULL));
        EXPECT(w->checkpoint_id == (i <= boundary ? 1 : 2));
    }
    OK(ss_writer_finish(w, boundary + 2));
    ss_document_t *d = open_writer(w);
    ss_document_info_t info = SS_INIT(ss_document_info_t);
    OK(ss_document_get_info(d, &info));
    EXPECT(info.checkpoint_count == 2);
    ss_document_release(d);
    ss_writer_release(w);
    free(xyz);
    free(indices);
    free(classes);
}

int main(void) {
    test_golden();
    test_queries();
    test_timeline();
    test_streaming();
    test_partition();
    test_writer_atomic_and_cadence();
    test_trim();
    test_resources();
    test_lz4();
    test_byte_cadence();
    printf("%u core checks passed\n", checks);
    return 0;
}
