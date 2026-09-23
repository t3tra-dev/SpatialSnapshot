#include "internal.h"

static int triple_compare(const uint16_t *a, const uint16_t *b) {
    for (unsigned i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
bool ssi_triangle_degenerate(const uint16_t *a, const uint16_t *b, const uint16_t *c) {
    const double scale = 0.5 / 65535.0;
    ss_vec3_t u = {(double)b[0] - a[0], (double)b[1] - a[1], (double)b[2] - a[2]};
    ss_vec3_t v = {(double)c[0] - a[0], (double)c[1] - a[1], (double)c[2] - a[2]};
    ss_vec3_t cross = ssi_cross(u, v);
    return ssi_dot(cross, cross) * (scale * scale * scale * scale) <= 1e-16;
}
static ssi_mesh_t *mesh_allocate(ssi_memory_t *memory, ss_cell_key_t key, uint32_t v, uint32_t t) {
    size_t array_bytes = (size_t)6 * v + (size_t)7 * t;
    ssi_mesh_t *m = ssi_alloc(memory, sizeof(*m) + array_bytes);
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->references = 1;
    m->key = key;
    m->vertices = v;
    m->triangles = t;
    m->raw_size = (uint32_t)(32 + array_bytes);
    m->xyz = (uint16_t *)(m + 1);
    m->indices = m->xyz + (size_t)3 * v;
    m->classes = (uint8_t *)(m->indices + (size_t)3 * t);
    return m;
}
void ssi_mesh_retain(ssi_mesh_t *m) {
    if (m)
        ++m->references;
}
void ssi_mesh_release(ssi_mesh_t *m) {
    if (m && --m->references == 0)
        ssi_free(m);
}
bool ssi_mesh_equal(const ssi_mesh_t *a, const ssi_mesh_t *b) {
    return a && b && !ssi_key_compare(a->key, b->key) && a->vertices == b->vertices &&
           a->triangles == b->triangles && !memcmp(a->xyz, b->xyz, (size_t)6 * a->vertices) &&
           !memcmp(a->indices, b->indices, (size_t)6 * a->triangles) &&
           !memcmp(a->classes, b->classes, a->triangles);
}
void ssi_mesh_view(const ssi_mesh_t *m, ss_mesh_view_t *view) {
    *view = (ss_mesh_view_t){.struct_size = sizeof(*view),
                             .abi_version = SS_ABI_VERSION,
                             .cell = m->key,
                             .vertex_count = m->vertices,
                             .triangle_count = m->triangles,
                             .quantized_xyz = m->xyz,
                             .triangle_indices = m->indices,
                             .classifications = m->classes};
}
ss_status_t ss_mesh_vertex(const ss_mesh_view_t *m, uint32_t index, ss_vec3_t *p) {
    if (!SSI_VALID(m, ss_mesh_view_t) || !p || !m->quantized_xyz)
        return SS_INVALID_ARGUMENT;
    if (index >= m->vertex_count)
        return SS_OUT_OF_RANGE;
    const uint16_t *q = m->quantized_xyz + (size_t)3 * index;
    *p = (ss_vec3_t){0.5 * m->cell.x + 0.5 * q[0] / 65535.0, 0.5 * m->cell.y + 0.5 * q[1] / 65535.0,
                     0.5 * m->cell.z + 0.5 * q[2] / 65535.0};
    return SS_OK;
}

ss_status_t ssi_mesh_decode(ssi_memory_t *memory, const uint8_t *raw, size_t size,
                            ssi_mesh_t **out) {
    *out = NULL;
    if (size < 32 || ssi_u32(raw + 28) != 0)
        return SS_MALFORMED;
    uint32_t v = ssi_u32(raw + 20), t = ssi_u32(raw + 24);
    if (v < 3 || v > SSPS_MAX_VERTICES_PER_CHUNK || t < 1 || t > SSPS_MAX_TRIANGLES_PER_CHUNK ||
        size != 32 + (size_t)6 * v + (size_t)7 * t)
        return SS_MALFORMED;
    if (size > memory->options.max_geometry_bytes || size > memory->options.max_packet_raw_bytes)
        return SS_RESOURCE_LIMIT;
    ss_cell_key_t key = {ssi_i32(raw + 8), ssi_i32(raw + 12), ssi_i32(raw + 16)};
    ssi_mesh_t *m = mesh_allocate(memory, key, v, t);
    if (!m)
        return memory->failure;
    uint8_t *used = ssi_alloc(memory, v);
    if (!used) {
        ssi_mesh_release(m);
        return memory->failure;
    }
    memset(used, 0, v);
    for (size_t i = 0; i < (size_t)3 * v; ++i)
        m->xyz[i] = ssi_u16(raw + 32 + 2 * i);
    for (size_t i = 0; i < (size_t)3 * t; ++i)
        m->indices[i] = ssi_u16(raw + 32 + (size_t)6 * v + 2 * i);
    ss_status_t status = SS_MALFORMED;
    for (size_t i = 1; i < v; ++i)
        if (triple_compare(m->xyz + 3 * (i - 1), m->xyz + 3 * i) >= 0)
            goto done;
    for (size_t i = 0; i < t; ++i) {
        const uint16_t *ix = m->indices + 3 * i;
        if (ix[0] >= v || ix[1] >= v || ix[2] >= v || ix[0] >= ix[1] || ix[0] >= ix[2] ||
            ix[1] == ix[2])
            goto done;
        if (i && triple_compare(m->indices + 3 * (i - 1), ix) >= 0)
            goto done;
        if (ssi_triangle_degenerate(m->xyz + 3 * (size_t)ix[0], m->xyz + 3 * (size_t)ix[1],
                                    m->xyz + 3 * (size_t)ix[2]))
            goto done;
        used[ix[0]] = used[ix[1]] = used[ix[2]] = 1;
        uint8_t c = raw[32 + (size_t)6 * v + (size_t)6 * t + i];
        if (c == 255)
            goto done;
        m->classes[i] = c <= 8 ? c : 0;
    }
    for (size_t i = 0; i < v; ++i)
        if (!used[i])
            goto done;
    status = SS_OK;
    *out = m;
done:
    ssi_free(used);
    if (status != SS_OK)
        ssi_mesh_release(m);
    return status;
}

typedef struct face_q {
    uint16_t xyz[9];
    uint8_t classification;
} face_q_t;
typedef struct indexed_vertex {
    uint16_t xyz[3];
    uint32_t source;
} indexed_vertex_t;
typedef struct indexed_face {
    uint16_t indices[3];
    uint8_t classification;
} indexed_face_t;
static int compare_vertex(const void *a, const void *b) {
    return triple_compare(((const indexed_vertex_t *)a)->xyz, ((const indexed_vertex_t *)b)->xyz);
}
static int compare_face(const void *a, const void *b) {
    const indexed_face_t *x = a, *y = b;
    int c = triple_compare(x->indices, y->indices);
    return c ? c : (int)x->classification - y->classification;
}

static ss_status_t mesh_from_faces(ssi_memory_t *memory, ss_cell_key_t key, const face_q_t *faces,
                                   size_t count, ssi_mesh_t **out) {
    *out = NULL;
    if (!count)
        return SS_NOT_FOUND;
    if (count > SSPS_MAX_TRIANGLES_PER_CHUNK)
        return SS_RESOURCE_LIMIT;
    size_t n = 3 * count;
    indexed_vertex_t *vertices = ssi_alloc(memory, n * sizeof(*vertices));
    uint32_t *mapping = ssi_alloc(memory, n * sizeof(*mapping));
    indexed_face_t *indices = ssi_alloc(memory, count * sizeof(*indices));
    ss_status_t status = SS_OK;
    if (!vertices || !mapping || !indices) {
        status = memory->failure;
        goto done;
    }
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ssi_triangle_degenerate(faces[i].xyz, faces[i].xyz + 3, faces[i].xyz + 6))
            continue;
        indices[kept].classification = faces[i].classification;
        for (size_t j = 0; j < 3; ++j) {
            memcpy(vertices[3 * kept + j].xyz, faces[i].xyz + 3 * j, 6);
            vertices[3 * kept + j].source = (uint32_t)(3 * kept + j);
        }
        ++kept;
    }
    if (!kept) {
        status = SS_NOT_FOUND;
        goto done;
    }
    qsort(vertices, 3 * kept, sizeof(*vertices), compare_vertex);
    uint32_t unique = 0;
    for (size_t i = 0; i < 3 * kept; ++i) {
        if (i == 0 || triple_compare(vertices[i - 1].xyz, vertices[i].xyz))
            ++unique;
        mapping[vertices[i].source] = unique - 1;
    }
    if (unique > SSPS_MAX_VERTICES_PER_CHUNK) {
        status = SS_RESOURCE_LIMIT;
        goto done;
    }
    for (size_t i = 0; i < kept; ++i) {
        uint16_t q[3] = {(uint16_t)mapping[3 * i], (uint16_t)mapping[3 * i + 1],
                         (uint16_t)mapping[3 * i + 2]};
        unsigned first = q[1] < q[0] ? 1 : 0;
        if (q[2] < q[first])
            first = 2;
        for (unsigned j = 0; j < 3; ++j)
            indices[i].indices[j] = q[(first + j) % 3];
    }
    qsort(indices, kept, sizeof(*indices), compare_face);
    size_t triangles = 0;
    for (size_t i = 0; i < kept; ++i) {
        if (triangles && !triple_compare(indices[triangles - 1].indices, indices[i].indices)) {
            if (indices[triangles - 1].classification != indices[i].classification) {
                status = SS_INVALID_ARGUMENT;
                goto done;
            }
        } else
            indices[triangles++] = indices[i];
    }
    size_t raw_size = 32 + (size_t)6 * unique + 7 * triangles;
    if (raw_size > memory->options.max_packet_raw_bytes ||
        raw_size > memory->options.max_geometry_bytes) {
        status = SS_RESOURCE_LIMIT;
        goto done;
    }
    ssi_mesh_t *mesh = mesh_allocate(memory, key, unique, (uint32_t)triangles);
    if (!mesh) {
        status = memory->failure;
        goto done;
    }
    unique = 0;
    for (size_t i = 0; i < 3 * kept; ++i)
        if (i == 0 || triple_compare(vertices[i - 1].xyz, vertices[i].xyz))
            memcpy(mesh->xyz + (size_t)3 * unique++, vertices[i].xyz, 6);
    for (size_t i = 0; i < triangles; ++i) {
        memcpy(mesh->indices + 3 * i, indices[i].indices, 6);
        mesh->classes[i] = indices[i].classification;
    }
    *out = mesh;
done:
    ssi_free(vertices);
    ssi_free(mapping);
    ssi_free(indices);
    return status;
}

ss_status_t ssi_mesh_canonicalize(ssi_memory_t *memory, const ss_mesh_view_t *input,
                                  ssi_mesh_t **out) {
    *out = NULL;
    if (!SSI_VALID(input, ss_mesh_view_t) || input->vertex_count < 3 ||
        input->vertex_count > SSPS_MAX_VERTICES_PER_CHUNK || input->triangle_count < 1 ||
        input->triangle_count > SSPS_MAX_TRIANGLES_PER_CHUNK || !input->quantized_xyz ||
        !input->triangle_indices || !input->classifications)
        return SS_INVALID_ARGUMENT;
    face_q_t *faces = ssi_alloc(memory, input->triangle_count * sizeof(*faces));
    if (!faces)
        return memory->failure;
    ss_status_t status = SS_OK;
    for (size_t i = 0; i < input->triangle_count; ++i) {
        if (input->classifications[i] > 8) {
            status = SS_INVALID_ARGUMENT;
            goto done;
        }
        faces[i].classification = input->classifications[i];
        for (size_t j = 0; j < 3; ++j) {
            uint16_t ix = input->triangle_indices[3 * i + j];
            if (ix >= input->vertex_count) {
                status = SS_INVALID_ARGUMENT;
                goto done;
            }
            memcpy(faces[i].xyz + 3 * j, input->quantized_xyz + (size_t)3 * ix, 6);
        }
    }
    status = mesh_from_faces(memory, input->cell, faces, input->triangle_count, out);
    if (status == SS_NOT_FOUND)
        status = SS_INVALID_ARGUMENT;
done:
    ssi_free(faces);
    return status;
}

ss_status_t ssi_mesh_encode(ssi_buffer_t *b, const ssi_mesh_t *m, uint64_t checkpoint) {
    uint8_t *p = ssi_alloc(b->memory, m->raw_size);
    if (!p)
        return b->memory->failure;
    memset(p, 0, 32);
    ssi_w64(p, checkpoint);
    ssi_w32(p + 8, (uint32_t)m->key.x);
    ssi_w32(p + 12, (uint32_t)m->key.y);
    ssi_w32(p + 16, (uint32_t)m->key.z);
    ssi_w32(p + 20, m->vertices);
    ssi_w32(p + 24, m->triangles);
    for (size_t i = 0; i < (size_t)3 * m->vertices; ++i)
        ssi_w16(p + 32 + 2 * i, m->xyz[i]);
    for (size_t i = 0; i < (size_t)3 * m->triangles; ++i)
        ssi_w16(p + 32 + (size_t)6 * m->vertices + 2 * i, m->indices[i]);
    memcpy(p + 32 + (size_t)6 * m->vertices + (size_t)6 * m->triangles, m->classes, m->triangles);
    ss_status_t status = ssi_buffer_append(b, p, m->raw_size);
    ssi_free(p);
    return status;
}

typedef struct fragment {
    ss_cell_key_t key;
    face_q_t face;
} fragment_t;
static int compare_fragments(const void *a, const void *b) {
    return ssi_key_compare(((const fragment_t *)a)->key, ((const fragment_t *)b)->key);
}
static double axis_value(ss_vec3_t p, unsigned axis) {
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}
static void set_axis(ss_vec3_t *p, unsigned axis, double v) {
    if (axis == 0)
        p->x = v;
    else if (axis == 1)
        p->y = v;
    else
        p->z = v;
}
static size_t clip_plane(const ss_vec3_t *in, size_t n, ss_vec3_t *out, unsigned axis,
                         double boundary, bool lower) {
    if (!n)
        return 0;
    size_t count = 0;
    ss_vec3_t a = in[n - 1];
    double av = axis_value(a, axis);
    bool ain = lower ? av >= boundary : av <= boundary;
    for (size_t i = 0; i < n; ++i) {
        ss_vec3_t b = in[i];
        double bv = axis_value(b, axis);
        bool bin = lower ? bv >= boundary : bv <= boundary;
        if (ain != bin) {
            double t = (boundary - av) / (bv - av);
            ss_vec3_t p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            set_axis(&p, axis, boundary);
            out[count++] = p;
        }
        if (bin)
            out[count++] = b;
        a = b;
        av = bv;
        ain = bin;
    }
    return count;
}
static uint16_t quantize(double p, double origin) {
    double q = floor((p - origin) * 2.0 * 65535.0 + 0.5);
    return q <= 0 ? 0 : q >= 65535 ? 65535 : (uint16_t)q;
}

ss_status_t ssi_partition(ssi_memory_t *memory, const ss_triangle_t *triangles, size_t count,
                          ss_mesh_set_t **out) {
    *out = NULL;
    if (!triangles && count)
        return SS_INVALID_ARGUMENT;
    if (count > memory->options.max_partition_work)
        return SS_RESOURCE_LIMIT;
    fragment_t *fragments = NULL;
    size_t fragment_count = 0, capacity = 0;
    uint64_t work = 0;
    ss_mesh_set_t *set = NULL;
    ss_status_t status = SS_OK;
    for (size_t i = 0; i < count; ++i) {
        const ss_triangle_t *triangle = &triangles[i];
        if (!SSI_VALID(triangle, ss_triangle_t) || triangle->classification > 8) {
            status = SS_INVALID_ARGUMENT;
            goto done;
        }
        for (unsigned j = 0; j < 3; ++j)
            if (!ssi_finite3(triangle->vertices[j])) {
                status = SS_INVALID_ARGUMENT;
                goto done;
            }
        int64_t low[3], high[3];
        uint64_t cells = 1;
        for (unsigned axis = 0; axis < 3; ++axis) {
            double lo = axis_value(triangle->vertices[0], axis), hi = lo;
            for (unsigned j = 1; j < 3; ++j) {
                double v = axis_value(triangle->vertices[j], axis);
                lo = fmin(lo, v);
                hi = fmax(hi, v);
            }
            if (lo < 0.5 * INT32_MIN || hi > 0.5 * ((double)INT32_MAX + 1)) {
                status = SS_OUT_OF_RANGE;
                goto done;
            }
            low[axis] = (int64_t)floor(lo * 2);
            high[axis] = hi > lo ? (int64_t)ceil(hi * 2) - 1 : low[axis];
            if (low[axis] > INT32_MAX)
                low[axis] = INT32_MAX;
            if (high[axis] > INT32_MAX)
                high[axis] = INT32_MAX;
            if (high[axis] < low[axis])
                high[axis] = low[axis];
            uint64_t width = (uint64_t)(high[axis] - low[axis] + 1);
            if (width > (memory->options.max_partition_work - work) / cells) {
                status = SS_RESOURCE_LIMIT;
                goto done;
            }
            cells *= width;
        }
        work += cells;
        for (int64_t x = low[0]; x <= high[0]; ++x)
            for (int64_t y = low[1]; y <= high[1]; ++y)
                for (int64_t z = low[2]; z <= high[2]; ++z) {
                    ss_vec3_t polygons[2][16];
                    memcpy(polygons[0], triangle->vertices, sizeof(triangle->vertices));
                    size_t n = 3;
                    unsigned current = 0;
                    double origin[3] = {0.5 * (double)x, 0.5 * (double)y, 0.5 * (double)z};
                    for (unsigned axis = 0; axis < 3 && n >= 3; ++axis) {
                        n = clip_plane(polygons[current], n, polygons[current ^ 1], axis,
                                       origin[axis], true);
                        current ^= 1;
                        n = clip_plane(polygons[current], n, polygons[current ^ 1], axis,
                                       origin[axis] + 0.5, false);
                        current ^= 1;
                    }
                    for (size_t j = 1; j + 1 < n; ++j) {
                        fragment_t fragment = {
                            .key = {(int32_t)x, (int32_t)y, (int32_t)z},
                            .face = {.classification = (uint8_t)triangle->classification}};
                        size_t vertices[3] = {0, j, j + 1};
                        for (size_t k = 0; k < 3; ++k)
                            for (unsigned axis = 0; axis < 3; ++axis)
                                fragment.face.xyz[3 * k + axis] = quantize(
                                    axis_value(polygons[current][vertices[k]], axis), origin[axis]);
                        if (ssi_triangle_degenerate(fragment.face.xyz, fragment.face.xyz + 3,
                                                    fragment.face.xyz + 6))
                            continue;
                        status = ssi_grow(memory, (void **)&fragments, &capacity,
                                          fragment_count + 1, sizeof(*fragments));
                        if (status != SS_OK)
                            goto done;
                        fragments[fragment_count++] = fragment;
                    }
                }
    }
    if (fragment_count)
        qsort(fragments, fragment_count, sizeof(*fragments), compare_fragments);
    size_t cells = 0;
    for (size_t i = 0; i < fragment_count; ++i)
        if (!i || ssi_key_compare(fragments[i - 1].key, fragments[i].key))
            ++cells;
    if (cells > SSPS_MAX_SCENE_CELLS || cells > memory->options.max_scene_cells) {
        status = SS_RESOURCE_LIMIT;
        goto done;
    }
    set = ssi_alloc(memory, sizeof(*set));
    if (!set) {
        status = memory->failure;
        goto done;
    }
    *set = (ss_mesh_set_t){.memory = memory};
    ssi_memory_retain(memory);
    if (cells) {
        set->meshes = ssi_alloc(memory, cells * sizeof(*set->meshes));
        if (!set->meshes) {
            status = memory->failure;
            goto done;
        }
    }
    uint64_t bytes = 0;
    for (size_t i = 0; i < fragment_count;) {
        size_t end = i + 1;
        while (end < fragment_count && !ssi_key_compare(fragments[i].key, fragments[end].key))
            ++end;
        face_q_t *faces = ssi_alloc(memory, (end - i) * sizeof(*faces));
        if (!faces) {
            status = memory->failure;
            goto done;
        }
        for (size_t j = i; j < end; ++j)
            faces[j - i] = fragments[j].face;
        ssi_mesh_t *mesh = NULL;
        status = mesh_from_faces(memory, fragments[i].key, faces, end - i, &mesh);
        ssi_free(faces);
        if (status != SS_OK)
            goto done;
        set->meshes[set->count++] = mesh;
        bytes += mesh->raw_size;
        if (bytes > memory->options.max_geometry_bytes) {
            status = SS_RESOURCE_LIMIT;
            goto done;
        }
        i = end;
    }
    *out = set;
    set = NULL;
done:
    ssi_free(fragments);
    ss_mesh_set_release(set);
    return status;
}
ss_status_t ss_mesh_partition(const ss_triangle_t *triangles, size_t count,
                              const ss_open_options_t *options, ss_mesh_set_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    ssi_memory_t *memory = NULL;
    ss_status_t status = ssi_memory_create(options, &memory);
    if (status != SS_OK)
        return status;
    status = ssi_partition(memory, triangles, count, out);
    ssi_memory_release(memory);
    return status;
}
void ss_mesh_set_release(ss_mesh_set_t *set) {
    if (!set)
        return;
    ssi_memory_t *m = set->memory;
    for (size_t i = 0; i < set->count; ++i)
        ssi_mesh_release(set->meshes[i]);
    ssi_free(set->meshes);
    ssi_free(set);
    ssi_memory_release(m);
}
uint32_t ss_mesh_set_count(const ss_mesh_set_t *set) {
    return set ? (uint32_t)set->count : 0;
}
ss_status_t ss_mesh_set_chunk(const ss_mesh_set_t *set, uint32_t index, ss_mesh_view_t *mesh) {
    if (!set || !SSI_VALID(mesh, ss_mesh_view_t))
        return SS_INVALID_ARGUMENT;
    if (index >= set->count)
        return SS_OUT_OF_RANGE;
    ssi_mesh_view(set->meshes[index], mesh);
    return SS_OK;
}
