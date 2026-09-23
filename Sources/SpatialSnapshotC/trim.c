#include "internal.h"

static bool has_geometry_at(const ss_document_t *d, uint64_t time) {
    size_t lo = 0, hi = d->packet_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->packets[mid].info.timestamp_ns < time)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < d->packet_count && d->packets[i].info.timestamp_ns == time; ++i) {
        uint32_t type = d->packets[i].info.type;
        if (type == SS_PACKET_CHECKPOINT_BEGIN || type == SS_PACKET_GEOMETRY_PUT ||
            type == SS_PACKET_GEOMETRY_REMOVE)
            return true;
    }
    return false;
}

static ss_status_t rebase_geometry(ss_scene_cursor_t *cursor, ss_writer_t *writer,
                                   const double rotation[9], ss_vec3_t origin,
                                   ss_mesh_set_t **out) {
    size_t count = 0, capacity = 0;
    ss_triangle_t *triangles = NULL;
    ss_status_t status = SS_OK;
    for (uint32_t i = 0; i < ss_scene_cursor_chunk_count(cursor); ++i) {
        ss_mesh_view_t mesh = SS_INIT(ss_mesh_view_t);
        status = ss_scene_cursor_chunk(cursor, i, &mesh);
        if (status != SS_OK)
            goto done;
        size_t total;
        if (!ssi_add(count, mesh.triangle_count, &total) ||
            total > writer->memory->options.max_partition_work) {
            status = SS_RESOURCE_LIMIT;
            goto done;
        }
        status =
            ssi_grow(writer->memory, (void **)&triangles, &capacity, total, sizeof(*triangles));
        if (status != SS_OK)
            goto done;
        for (uint32_t t = 0; t < mesh.triangle_count; ++t) {
            ss_triangle_t *triangle = &triangles[count++];
            *triangle = (ss_triangle_t){.struct_size = sizeof(*triangle),
                                        .abi_version = SS_ABI_VERSION,
                                        .classification = mesh.classifications[t]};
            for (unsigned j = 0; j < 3; ++j) {
                ss_vec3_t p;
                (void)ss_mesh_vertex(&mesh, mesh.triangle_indices[(size_t)3 * t + j], &p);
                triangle->vertices[j] = ssi_inverse_rotate(rotation, ssi_sub(p, origin));
            }
        }
    }
    status = ssi_partition(writer->memory, triangles, count, out);
done:
    ssi_free(triangles);
    return status;
}

static ss_status_t make_updates(ss_writer_t *w, const ss_mesh_set_t *set,
                                ss_geometry_update_t **out, size_t *count) {
    *out = NULL;
    *count = 0;
    size_t capacity;
    if (!ssi_add(w->scene.count, set->count, &capacity))
        return SS_RESOURCE_LIMIT;
    if (!capacity)
        return SS_OK;
    ss_geometry_update_t *updates = ssi_alloc(w->memory, capacity * sizeof(*updates));
    if (!updates)
        return w->memory->failure;
    ssi_entry_t **old = NULL;
    ss_status_t s = ssi_map_sorted(&w->scene, &old);
    if (s != SS_OK) {
        ssi_free(updates);
        return s;
    }
    size_t i = 0, j = 0, n = 0;
    while (i < w->scene.count || j < set->count) {
        int order = i == w->scene.count ? 1
                    : j == set->count   ? -1
                                        : ssi_key_compare(old[i]->key, set->meshes[j]->key);
        ss_geometry_update_t update = {
            .struct_size = sizeof(update),
            .abi_version = SS_ABI_VERSION,
            .mesh = {.struct_size = sizeof(ss_mesh_view_t), .abi_version = SS_ABI_VERSION}};
        if (order < 0) {
            update.operation = SS_GEOMETRY_REMOVE;
            update.mesh.cell = old[i++]->key;
        } else {
            bool equal = order == 0 && ssi_mesh_equal(old[i]->mesh, set->meshes[j]);
            if (order == 0)
                ++i;
            if (equal) {
                ++j;
                continue;
            }
            update.operation = SS_GEOMETRY_PUT;
            ssi_mesh_view(set->meshes[j++], &update.mesh);
        }
        updates[n++] = update;
    }
    ssi_free(old);
    *out = updates;
    *count = n;
    return SS_OK;
}

static void rebase_camera(const ss_camera_t *base, const double r[9], const ss_camera_t *source,
                          uint64_t start, ss_camera_t *out) {
    *out = *source;
    out->timestamp_ns -= start;
    if (source->timestamp_ns == start) {
        out->tx = out->ty = out->tz = out->qx = out->qy = out->qz = 0;
        out->qw = 1;
        return;
    }
    ss_vec3_t t = ssi_inverse_rotate(r, (ss_vec3_t){(double)source->tx - base->tx,
                                                    (double)source->ty - base->ty,
                                                    (double)source->tz - base->tz});
    out->tx = (float)t.x;
    out->ty = (float)t.y;
    out->tz = (float)t.z;
    double a[4] = {-base->qx, -base->qy, -base->qz, base->qw},
           b[4] = {source->qx, source->qy, source->qz, source->qw};
    double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    double nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    for (unsigned i = 0; i < 4; ++i) {
        a[i] /= na;
        b[i] /= nb;
    }
    out->qx = (float)(a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1]);
    out->qy = (float)(a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0]);
    out->qz = (float)(a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3]);
    out->qw = (float)(a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]);
}

ss_status_t ss_document_trim(const ss_document_t *document, uint64_t start, uint64_t end,
                             const ss_writer_options_t *options, ss_writer_t **out) {
    if (out)
        *out = NULL;
    if (!document || !out || !SSI_VALID(options, ss_writer_options_t) ||
        document->info.kind != SS_VIDEO || start >= end || end > document->info.duration_ns)
        return SS_INVALID_ARGUMENT;
    size_t first = ssi_find_frame(document, start);
    if (first == SIZE_MAX ||
        (end != document->info.duration_ns && ssi_find_frame(document, end) == SIZE_MAX))
        return SS_INVALID_ARGUMENT;
    const ss_camera_t *base = &document->frames[first].camera;
    double r[9];
    ssi_rotation(base, r);
    ss_vec3_t origin = {base->tx, base->ty, base->tz};
    ss_writer_options_t configured = *options;
    configured.kind = SS_VIDEO;
    configured.gravity = ssi_inverse_rotate(r, document->info.gravity);
    ss_writer_t *writer = NULL;
    ss_scene_cursor_t *cursor = NULL;
    ss_mesh_set_t *set = NULL;
    ss_geometry_update_t *updates = NULL;
    ss_status_t status = ss_writer_create(&configured, &writer);
    if (status != SS_OK)
        return status;
    /* Stream IDs must be new, even if a host random provider misbehaves. */
    for (unsigned attempt = 0; !memcmp(writer->buffer.data + 24, document->info.stream_id, 16);
         ++attempt) {
        if (attempt == 8 ||
            configured.random_bytes(configured.random_context, writer->buffer.data + 24, 16) !=
                SS_OK ||
            ssi_zero(writer->buffer.data + 24, 16)) {
            status = SS_IO_ERROR;
            goto done;
        }
        ssi_w32(writer->buffer.data + 40, 0);
        ssi_w32(writer->buffer.data + 40, ss_crc32c(writer->buffer.data, 64));
    }
    status = ss_scene_cursor_create((ss_document_t *)document, &cursor);
    if (status != SS_OK)
        goto done;
    for (size_t i = first;
         i < document->frame_count && document->frames[i].camera.timestamp_ns < end; ++i) {
        uint64_t time = document->frames[i].camera.timestamp_ns;
        status = ss_scene_cursor_seek(cursor, time);
        if (status != SS_OK)
            goto done;
        size_t update_count = 0;
        if (i == first || has_geometry_at(document, time)) {
            status = rebase_geometry(cursor, writer, r, origin, &set);
            if (status != SS_OK)
                goto done;
            status = make_updates(writer, set, &updates, &update_count);
            if (status != SS_OK)
                goto done;
        }
        ss_camera_t camera;
        rebase_camera(base, r, &document->frames[i].camera, start, &camera);
        if (ssi_camera_validate(&camera, i == first, false) != SS_OK) {
            status = SS_OUT_OF_RANGE;
            goto done;
        }
        ss_depth_view_t depth = SS_INIT(ss_depth_view_t);
        ss_depth_view_t *depth_pointer = NULL;
        status = ss_scene_cursor_depth(cursor, &depth);
        if (status == SS_OK) {
            depth.timestamp_ns -= start;
            depth_pointer = &depth;
        } else if (status != SS_NO_SAMPLE)
            goto done;
        status = ss_writer_append_frame(writer, &camera, updates, update_count, depth_pointer);
        ssi_free(updates);
        updates = NULL;
        ss_mesh_set_release(set);
        set = NULL;
        if (status != SS_OK)
            goto done;
    }
    status = ss_writer_finish(writer, end - start);
    if (status == SS_OK) {
        *out = writer;
        writer = NULL;
    }
done:
    ssi_free(updates);
    ss_mesh_set_release(set);
    ss_scene_cursor_release(cursor);
    ss_writer_release(writer);
    return status;
}
