#include "internal.h"

ss_vec3_t ssi_sub(ss_vec3_t a, ss_vec3_t b) {
    return (ss_vec3_t){a.x - b.x, a.y - b.y, a.z - b.z};
}
ss_vec3_t ssi_cross(ss_vec3_t a, ss_vec3_t b) {
    return (ss_vec3_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double ssi_dot(ss_vec3_t a, ss_vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
bool ssi_finite3(ss_vec3_t p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}
void ssi_rotation(const ss_camera_t *c, double r[9]) {
    double x = c->qx, y = c->qy, z = c->qz, w = c->qw;
    double n = sqrt(x * x + y * y + z * z + w * w);
    x /= n;
    y /= n;
    z /= n;
    w /= n;
    r[0] = 1 - 2 * (y * y + z * z);
    r[1] = 2 * (x * y - w * z);
    r[2] = 2 * (x * z + w * y);
    r[3] = 2 * (x * y + w * z);
    r[4] = 1 - 2 * (x * x + z * z);
    r[5] = 2 * (y * z - w * x);
    r[6] = 2 * (x * z - w * y);
    r[7] = 2 * (y * z + w * x);
    r[8] = 1 - 2 * (x * x + y * y);
}
ss_vec3_t ssi_rotate(const double r[9], ss_vec3_t p) {
    return (ss_vec3_t){r[0] * p.x + r[1] * p.y + r[2] * p.z, r[3] * p.x + r[4] * p.y + r[5] * p.z,
                       r[6] * p.x + r[7] * p.y + r[8] * p.z};
}
ss_vec3_t ssi_inverse_rotate(const double r[9], ss_vec3_t p) {
    return (ss_vec3_t){r[0] * p.x + r[3] * p.y + r[6] * p.z, r[1] * p.x + r[4] * p.y + r[7] * p.z,
                       r[2] * p.x + r[5] * p.y + r[8] * p.z};
}

ss_status_t ss_camera_project(const ss_camera_t *c, const ss_vec3_t *point, ss_vec2_t *pixel) {
    if (!point || !pixel || !ssi_finite3(*point) || ssi_camera_validate(c, false, false) != SS_OK)
        return SS_INVALID_ARGUMENT;
    double r[9];
    ssi_rotation(c, r);
    ss_vec3_t p = ssi_inverse_rotate(r, ssi_sub(*point, (ss_vec3_t){c->tx, c->ty, c->tz}));
    if (!(p.z > 0) || !ssi_finite3(p))
        return SS_OUT_OF_RANGE;
    ss_vec2_t result = {c->fx * (p.x / p.z) + c->cx, c->fy * (p.y / p.z) + c->cy};
    if (!isfinite(result.x) || !isfinite(result.y))
        return SS_OUT_OF_RANGE;
    *pixel = result;
    return SS_OK;
}
ss_status_t ss_camera_unproject(const ss_camera_t *c, const ss_vec2_t *pixel, double depth,
                                ss_vec3_t *point) {
    if (!pixel || !point || !isfinite(pixel->x) || !isfinite(pixel->y) || !isfinite(depth) ||
        depth <= 0 || ssi_camera_validate(c, false, false) != SS_OK)
        return SS_INVALID_ARGUMENT;
    ss_vec3_t local = {(pixel->x - c->cx) * depth / c->fx, (pixel->y - c->cy) * depth / c->fy,
                       depth};
    double r[9];
    ssi_rotation(c, r);
    ss_vec3_t p = ssi_rotate(r, local);
    p.x += c->tx;
    p.y += c->ty;
    p.z += c->tz;
    if (!ssi_finite3(p))
        return SS_OUT_OF_RANGE;
    *point = p;
    return SS_OK;
}
ss_status_t ss_camera_ray(const ss_camera_t *c, const ss_vec2_t *pixel, ss_ray_t *ray) {
    if (!SSI_VALID(ray, ss_ray_t) || !pixel || !isfinite(pixel->x) || !isfinite(pixel->y) ||
        ssi_camera_validate(c, false, false) != SS_OK)
        return SS_INVALID_ARGUMENT;
    double r[9];
    ssi_rotation(c, r);
    ss_vec3_t direction =
        ssi_rotate(r, (ss_vec3_t){(pixel->x - c->cx) / c->fx, (pixel->y - c->cy) / c->fy, 1});
    double n = hypot(hypot(direction.x, direction.y), direction.z);
    if (!isfinite(n) || n == 0)
        return SS_OUT_OF_RANGE;
    *ray = (ss_ray_t){.struct_size = sizeof(*ray),
                      .abi_version = SS_ABI_VERSION,
                      .origin = {c->tx, c->ty, c->tz},
                      .direction = {direction.x / n, direction.y / n, direction.z / n}};
    return SS_OK;
}

static void cursor_clear(ss_scene_cursor_t *c) {
    ssi_free(c->sorted);
    c->sorted = NULL;
    ssi_map_clear(&c->map);
    ssi_free(c->depth_values);
    ssi_free(c->depth_confidence);
    c->depth_values = NULL;
    c->depth_confidence = NULL;
    ssi_free(c->bvh);
    ssi_free(c->bvh_entries);
    c->bvh = NULL;
    c->bvh_entries = NULL;
    c->bvh_count = 0;
}
ss_status_t ss_scene_cursor_create(ss_document_t *d, ss_scene_cursor_t **out) {
    if (out)
        *out = NULL;
    if (!d || !out)
        return SS_INVALID_ARGUMENT;
    ss_scene_cursor_t *c = ssi_alloc(d->memory, sizeof(*c));
    if (!c)
        return d->memory->failure;
    *c = (ss_scene_cursor_t){.document = d, .frame = SIZE_MAX, .map = {.memory = d->memory}};
    ss_document_retain(d);
    ss_status_t s = ss_scene_cursor_seek(c, 0);
    if (s != SS_OK) {
        ss_scene_cursor_release(c);
        return s;
    }
    *out = c;
    return SS_OK;
}
void ss_scene_cursor_release(ss_scene_cursor_t *c) {
    if (!c)
        return;
    ss_document_t *d = c->document;
    cursor_clear(c);
    ssi_free(c);
    ss_document_release(d);
}
ss_status_t ss_scene_cursor_seek(ss_scene_cursor_t *c, uint64_t time) {
    if (!c)
        return SS_INVALID_ARGUMENT;
    ss_document_t *d = c->document;
    if (time > d->info.duration_ns)
        return SS_OUT_OF_RANGE;
    if (c->positioned && time == c->timestamp)
        return SS_OK;
    size_t lo = 0, hi = d->checkpoint_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->checkpoints[mid].timestamp <= time)
            lo = mid + 1;
        else
            hi = mid;
    }
    const ssi_checkpoint_t *cp = &d->checkpoints[lo - 1];
    ssi_map_t next = {.memory = d->memory};
    ssi_entry_t **sorted = NULL;
    ss_status_t s = SS_OK;
    /* Index directly to the most recent completed checkpoint, never replay
     * earlier geometry. The input document is already fully validated. */
    for (size_t i = cp->begin + 1; i < d->packet_count && d->packets[i].info.timestamp_ns <= time;
         ++i) {
        const ssi_packet_t *p = &d->packets[i];
        if (p->info.type == SS_PACKET_GEOMETRY_PUT) {
            ssi_entry_t e = {.key = p->key, .packet = i, .raw_size = p->info.raw_payload_size};
            s = ssi_map_put(&next, e);
        } else if (p->info.type == SS_PACKET_GEOMETRY_REMOVE)
            s = ssi_map_remove(&next, p->key);
        if (s != SS_OK)
            goto done;
    }
    s = ssi_map_sorted(&next, &sorted);
    if (s != SS_OK)
        goto done;
    cursor_clear(c);
    c->map = next;
    c->sorted = sorted;
    c->timestamp = time;
    c->frame = ssi_find_frame(d, time);
    c->positioned = true;
    return SS_OK;
done:
    ssi_free(sorted);
    ssi_map_clear(&next);
    return s;
}
ss_status_t ss_scene_cursor_camera(const ss_scene_cursor_t *c, ss_camera_t *camera) {
    if (!c || !SSI_VALID(camera, ss_camera_t))
        return SS_INVALID_ARGUMENT;
    if (c->frame == SIZE_MAX)
        return SS_NO_SAMPLE;
    *camera = c->document->frames[c->frame].camera;
    return SS_OK;
}
uint32_t ss_scene_cursor_chunk_count(const ss_scene_cursor_t *c) {
    return c ? (uint32_t)c->map.count : 0;
}
static ss_status_t load_mesh(ss_scene_cursor_t *c, ssi_entry_t *e) {
    if (e->mesh)
        return SS_OK;
    uint8_t *raw = NULL;
    const ssi_packet_t *p = &c->document->packets[(size_t)e->packet];
    ss_status_t s = ssi_packet_raw(c->document, p, &raw);
    if (s == SS_OK)
        s = ssi_mesh_decode(c->document->memory, raw, p->info.raw_payload_size, &e->mesh);
    ssi_free(raw);
    return s;
}
ss_status_t ss_scene_cursor_chunk(ss_scene_cursor_t *c, uint32_t i, ss_mesh_view_t *mesh) {
    if (!c || !SSI_VALID(mesh, ss_mesh_view_t))
        return SS_INVALID_ARGUMENT;
    if (i >= c->map.count)
        return SS_OUT_OF_RANGE;
    ss_status_t s = load_mesh(c, c->sorted[i]);
    if (s == SS_OK)
        ssi_mesh_view(c->sorted[i]->mesh, mesh);
    return s;
}
ss_status_t ss_scene_cursor_depth(ss_scene_cursor_t *c, ss_depth_view_t *depth) {
    if (!c || !SSI_VALID(depth, ss_depth_view_t))
        return SS_INVALID_ARGUMENT;
    if (c->frame == SIZE_MAX || c->document->frames[c->frame].depth_packet == SIZE_MAX)
        return SS_NO_SAMPLE;
    if (!c->depth_values) {
        const ssi_packet_t *p = &c->document->packets[c->document->frames[c->frame].depth_packet];
        uint8_t *raw = NULL;
        ss_status_t s = ssi_packet_raw(c->document, p, &raw);
        if (s != SS_OK)
            return s;
        uint32_t w = ssi_u32(raw), h = ssi_u32(raw + 4);
        size_t pixels = (size_t)w * h;
        uint16_t *values = ssi_alloc(c->document->memory, pixels * sizeof(*values));
        uint8_t *confidence = ssi_alloc(c->document->memory, pixels);
        if (!values || !confidence) {
            ssi_free(values);
            ssi_free(confidence);
            ssi_free(raw);
            return c->document->memory->failure;
        }
        for (size_t i = 0; i < pixels; ++i) {
            values[i] = ssi_u16(raw + 32 + 2 * i);
            confidence[i] = (uint8_t)((raw[32 + 2 * pixels + (i >> 2)] >> (2 * (i & 3))) & 3);
        }
        c->depth_values = values;
        c->depth_confidence = confidence;
        c->depth = (ss_depth_view_t){.struct_size = sizeof(c->depth),
                                     .abi_version = SS_ABI_VERSION,
                                     .timestamp_ns = c->timestamp,
                                     .width = w,
                                     .height = h,
                                     .fx = ssi_f32(raw + 8),
                                     .fy = ssi_f32(raw + 12),
                                     .cx = ssi_f32(raw + 16),
                                     .cy = ssi_f32(raw + 20),
                                     .depth_mm = values,
                                     .confidence = confidence};
        ssi_free(raw);
    }
    *depth = c->depth;
    return SS_OK;
}
ss_status_t ss_scene_depth_point(ss_scene_cursor_t *c, uint32_t u, uint32_t v, ss_vec3_t *point,
                                 uint32_t *confidence) {
    if (!c || !point || !confidence)
        return SS_INVALID_ARGUMENT;
    ss_depth_view_t d = SS_INIT(ss_depth_view_t);
    ss_status_t s = ss_scene_cursor_depth(c, &d);
    if (s != SS_OK)
        return s;
    if (u >= d.width || v >= d.height)
        return SS_OUT_OF_RANGE;
    size_t i = (size_t)v * d.width + u;
    if (!d.depth_mm[i])
        return SS_NO_SAMPLE;
    ss_camera_t camera = c->document->frames[c->frame].camera;
    camera.raster_width = d.width;
    camera.raster_height = d.height;
    camera.fx = d.fx;
    camera.fy = d.fy;
    camera.cx = d.cx;
    camera.cy = d.cy;
    ss_vec2_t pixel = {u, v};
    s = ss_camera_unproject(&camera, &pixel, d.depth_mm[i] * 0.001, point);
    if (s == SS_OK)
        *confidence = d.confidence[i];
    return s;
}

static int compare_bvh_x(const void *a, const void *b) {
    const ssi_entry_t *x = *(ssi_entry_t *const *)a, *y = *(ssi_entry_t *const *)b;
    return x->key.x < y->key.x ? -1 : x->key.x > y->key.x ? 1 : ssi_key_compare(x->key, y->key);
}
static int compare_bvh_y(const void *a, const void *b) {
    const ssi_entry_t *x = *(ssi_entry_t *const *)a, *y = *(ssi_entry_t *const *)b;
    return x->key.y < y->key.y ? -1 : x->key.y > y->key.y ? 1 : ssi_key_compare(x->key, y->key);
}
static int compare_bvh_z(const void *a, const void *b) {
    const ssi_entry_t *x = *(ssi_entry_t *const *)a, *y = *(ssi_entry_t *const *)b;
    return x->key.z < y->key.z ? -1 : x->key.z > y->key.z ? 1 : ssi_key_compare(x->key, y->key);
}
static size_t build_node(ss_scene_cursor_t *c, size_t start, size_t count) {
    size_t index = c->bvh_count++;
    ssi_bvh_node_t *node = &c->bvh[index];
    memset(node, 0, sizeof(*node));
    for (unsigned a = 0; a < 3; ++a) {
        node->low[a] = DBL_MAX;
        node->high[a] = -DBL_MAX;
    }
    for (size_t i = start; i < start + count; ++i) {
        ss_cell_key_t k = c->bvh_entries[i]->key;
        double low[3] = {0.5 * k.x, 0.5 * k.y, 0.5 * k.z};
        for (unsigned a = 0; a < 3; ++a) {
            node->low[a] = fmin(node->low[a], low[a]);
            node->high[a] = fmax(node->high[a], low[a] + 0.5);
        }
    }
    if (count <= 4) {
        node->start = start;
        node->count = count;
        return index;
    }
    unsigned axis = 0;
    for (unsigned a = 1; a < 3; ++a)
        if (node->high[a] - node->low[a] > node->high[axis] - node->low[axis])
            axis = a;
    qsort(c->bvh_entries + start, count, sizeof(*c->bvh_entries),
          axis == 0   ? compare_bvh_x
          : axis == 1 ? compare_bvh_y
                      : compare_bvh_z);
    node->left = build_node(c, start, count / 2);
    node->right = build_node(c, start + count / 2, count - count / 2);
    return index;
}
static ss_status_t ensure_bvh(ss_scene_cursor_t *c) {
    if (c->bvh || !c->map.count)
        return SS_OK;
    size_t n = c->map.count;
    c->bvh = ssi_alloc(c->document->memory, (2 * n - 1) * sizeof(*c->bvh));
    c->bvh_entries = ssi_alloc(c->document->memory, n * sizeof(*c->bvh_entries));
    if (!c->bvh || !c->bvh_entries) {
        ssi_free(c->bvh);
        ssi_free(c->bvh_entries);
        c->bvh = NULL;
        c->bvh_entries = NULL;
        return c->document->memory->failure;
    }
    memcpy(c->bvh_entries, c->sorted, n * sizeof(*c->bvh_entries));
    c->bvh_count = 0;
    (void)build_node(c, 0, n);
    return SS_OK;
}
static bool ray_box(const ss_ray_t *ray, const ssi_bvh_node_t *node, double far) {
    double near = ray->min_distance, origin[3] = {ray->origin.x, ray->origin.y, ray->origin.z},
           direction[3] = {ray->direction.x, ray->direction.y, ray->direction.z};
    for (unsigned a = 0; a < 3; ++a) {
        if (direction[a] == 0) {
            if (origin[a] < node->low[a] || origin[a] > node->high[a])
                return false;
            continue;
        }
        double t0 = (node->low[a] - origin[a]) / direction[a],
               t1 = (node->high[a] - origin[a]) / direction[a];
        if (t0 > t1) {
            double swap = t0;
            t0 = t1;
            t1 = swap;
        }
        near = fmax(near, t0);
        far = fmin(far, t1);
        if (near > far)
            return false;
    }
    return true;
}
static bool ray_triangle(const ss_ray_t *ray, ss_vec3_t a, ss_vec3_t b, ss_vec3_t c, double far,
                         double *distance, ss_vec3_t *normal) {
    ss_vec3_t e1 = ssi_sub(b, a), e2 = ssi_sub(c, a), h = ssi_cross(ray->direction, e2);
    ss_vec3_t n = ssi_cross(e1, e2);
    double norm = hypot(hypot(n.x, n.y), n.z), det = ssi_dot(e1, h);
    if (norm == 0 || fabs(det) <= DBL_EPSILON * norm)
        return false;
    ss_vec3_t s = ssi_sub(ray->origin, a);
    double u = ssi_dot(s, h) / det;
    if (u < -1e-12 || u > 1 + 1e-12)
        return false;
    ss_vec3_t q = ssi_cross(s, e1);
    double v = ssi_dot(ray->direction, q) / det;
    if (v < -1e-12 || u + v > 1 + 1e-12)
        return false;
    double t = ssi_dot(e2, q) / det;
    if (t < ray->min_distance || t > far || !isfinite(t))
        return false;
    *distance = t;
    *normal = (ss_vec3_t){n.x / norm, n.y / norm, n.z / norm};
    return true;
}
static ss_status_t raycast_node(ss_scene_cursor_t *c, size_t index, const ss_ray_t *ray,
                                ss_hit_t *hit, bool *found, double *far) {
    const ssi_bvh_node_t *node = &c->bvh[index];
    if (!ray_box(ray, node, *far))
        return SS_OK;
    if (!node->count) {
        ss_status_t s = raycast_node(c, node->left, ray, hit, found, far);
        return s == SS_OK ? raycast_node(c, node->right, ray, hit, found, far) : s;
    }
    for (size_t i = node->start; i < node->start + node->count; ++i) {
        ssi_entry_t *e = c->bvh_entries[i];
        ss_status_t s = load_mesh(c, e);
        if (s != SS_OK)
            return s;
        ss_mesh_view_t mesh;
        ssi_mesh_view(e->mesh, &mesh);
        for (uint32_t t = 0; t < mesh.triangle_count; ++t) {
            ss_vec3_t vertices[3], normal;
            double distance;
            for (unsigned j = 0; j < 3; ++j)
                (void)ss_mesh_vertex(&mesh, mesh.triangle_indices[(size_t)3 * t + j], &vertices[j]);
            if (!ray_triangle(ray, vertices[0], vertices[1], vertices[2], *far, &distance, &normal))
                continue;
            int key_order = *found ? ssi_key_compare(mesh.cell, hit->cell) : -1;
            if (*found && distance == *far &&
                (key_order > 0 || (key_order == 0 && t >= hit->triangle_index)))
                continue;
            *far = distance;
            *found = true;
            *hit = (ss_hit_t){.struct_size = sizeof(*hit),
                              .abi_version = SS_ABI_VERSION,
                              .position = {ray->origin.x + distance * ray->direction.x,
                                           ray->origin.y + distance * ray->direction.y,
                                           ray->origin.z + distance * ray->direction.z},
                              .normal = normal,
                              .distance = distance,
                              .cell = mesh.cell,
                              .triangle_index = t,
                              .classification = mesh.classifications[t]};
        }
    }
    return SS_OK;
}
ss_status_t ss_scene_raycast(ss_scene_cursor_t *c, const ss_ray_t *input, ss_hit_t *hit) {
    if (!c || !SSI_VALID(input, ss_ray_t) || !SSI_VALID(hit, ss_hit_t) ||
        !ssi_finite3(input->origin) || !ssi_finite3(input->direction) ||
        !isfinite(input->min_distance) || !isfinite(input->max_distance) ||
        input->min_distance < 0 || input->max_distance < 0 ||
        (input->max_distance != 0 && input->max_distance < input->min_distance))
        return SS_INVALID_ARGUMENT;
    double norm = hypot(hypot(input->direction.x, input->direction.y), input->direction.z);
    if (norm == 0 || !isfinite(norm))
        return SS_INVALID_ARGUMENT;
    if (!c->map.count)
        return SS_NOT_FOUND;
    ss_ray_t ray = *input;
    ray.direction = (ss_vec3_t){input->direction.x / norm, input->direction.y / norm,
                                input->direction.z / norm};
    ss_status_t s = ensure_bvh(c);
    if (s != SS_OK)
        return s;
    bool found = false;
    double far = ray.max_distance != 0 ? ray.max_distance : DBL_MAX;
    ss_hit_t result = SS_INIT(ss_hit_t);
    s = raycast_node(c, 0, &ray, &result, &found, &far);
    if (s != SS_OK)
        return s;
    if (!found)
        return SS_NOT_FOUND;
    *hit = result;
    return SS_OK;
}
ss_status_t ss_scene_raycast_pixel(ss_scene_cursor_t *c, const ss_vec2_t *pixel, ss_hit_t *hit) {
    ss_camera_t camera = SS_INIT(ss_camera_t);
    ss_status_t s = ss_scene_cursor_camera(c, &camera);
    if (s != SS_OK)
        return s;
    ss_ray_t ray = SS_INIT(ss_ray_t);
    s = ss_camera_ray(&camera, pixel, &ray);
    return s == SS_OK ? ss_scene_raycast(c, &ray, hit) : s;
}
ss_status_t ss_scene_point_visible(ss_scene_cursor_t *c, const ss_vec3_t *point, double tolerance,
                                   uint32_t *visible) {
    if (!point || !visible || !isfinite(tolerance) || tolerance < 0)
        return SS_INVALID_ARGUMENT;
    ss_camera_t camera = SS_INIT(ss_camera_t);
    ss_status_t s = ss_scene_cursor_camera(c, &camera);
    if (s != SS_OK)
        return s;
    ss_vec2_t pixel;
    s = ss_camera_project(&camera, point, &pixel);
    if (s == SS_OUT_OF_RANGE) {
        *visible = 0;
        return SS_OK;
    }
    if (s != SS_OK)
        return s;
    if (pixel.x < 0 || pixel.y < 0 || pixel.x > camera.raster_width - 1 ||
        pixel.y > camera.raster_height - 1) {
        *visible = 0;
        return SS_OK;
    }
    ss_vec3_t direction = ssi_sub(*point, (ss_vec3_t){camera.tx, camera.ty, camera.tz});
    double distance = hypot(hypot(direction.x, direction.y), direction.z);
    if (distance <= tolerance) {
        *visible = 1;
        return SS_OK;
    }
    ss_ray_t ray = {.struct_size = sizeof(ray),
                    .abi_version = SS_ABI_VERSION,
                    .origin = {camera.tx, camera.ty, camera.tz},
                    .direction = direction,
                    .max_distance = distance - tolerance};
    ss_hit_t hit = SS_INIT(ss_hit_t);
    s = ss_scene_raycast(c, &ray, &hit);
    if (s == SS_NOT_FOUND) {
        *visible = 1;
        return SS_OK;
    }
    if (s == SS_OK)
        *visible = hit.distance >= distance - tolerance ? 1u : 0u;
    return s;
}
