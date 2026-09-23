#include "internal.h"

static ss_status_t emit(ssi_buffer_t *b, uint64_t *sequence, uint64_t time, uint16_t type,
                        const uint8_t *raw, size_t size) {
    if (*sequence >= b->memory->options.max_packets || *sequence == UINT64_MAX)
        return SS_RESOURCE_LIMIT;
    ss_status_t s = ssi_emit_packet(b, *sequence, time, type, raw, size);
    if (s == SS_OK)
        ++*sequence;
    return s;
}

ss_status_t ss_writer_create(const ss_writer_options_t *options, ss_writer_t **out) {
    if (out)
        *out = NULL;
    if (!out || !SSI_VALID(options, ss_writer_options_t) || !options->random_bytes ||
        (options->kind != SS_STILL && options->kind != SS_VIDEO) || !ssi_finite3(options->gravity))
        return SS_INVALID_ARGUMENT;
    double norm = hypot(hypot(options->gravity.x, options->gravity.y), options->gravity.z);
    if (!isfinite(norm) || norm == 0)
        return SS_INVALID_ARGUMENT;
    ssi_memory_t *memory = NULL;
    ss_status_t s = ssi_memory_create(&options->resources, &memory);
    if (s != SS_OK)
        return s;
    ss_writer_t *w = ssi_alloc(memory, sizeof(*w));
    if (!w) {
        s = memory->failure;
        ssi_memory_release(memory);
        return s;
    }
    *w = (ss_writer_t){.memory = memory,
                       .options = *options,
                       .buffer = {.memory = memory},
                       .scene = {.memory = memory}};
    w->options.gravity = (ss_vec3_t){options->gravity.x / norm, options->gravity.y / norm,
                                     options->gravity.z / norm};
    uint8_t header[64] = {0};
    memcpy(header, "SSPS\r\n\032\n", 8);
    ssi_w16(header + 8, 1);
    ssi_w16(header + 12, 64);
    ssi_w16(header + 14, (uint16_t)options->kind);
    ssi_w16(header + 20, 48);
    if (options->random_bytes(options->random_context, header + 24, 16) != SS_OK ||
        ssi_zero(header + 24, 16)) {
        s = SS_IO_ERROR;
        goto fail;
    }
    ssi_w32(header + 40, ss_crc32c(header, sizeof(header)));
    s = ssi_buffer_append(&w->buffer, header, sizeof(header));
    if (s != SS_OK)
        goto fail;
    uint8_t scene[32] = {0};
    ssi_wf32(scene, (float)w->options.gravity.x);
    ssi_wf32(scene + 4, (float)w->options.gravity.y);
    ssi_wf32(scene + 8, (float)w->options.gravity.z);
    ssi_w32(scene + 16, 1);
    ssi_w32(scene + 20, 500000);
    ssi_w32(scene + 24, 1000);
    s = emit(&w->buffer, &w->sequence, 0, SS_PACKET_SCENE_INFO, scene, sizeof(scene));
    if (s != SS_OK)
        goto fail;
    *out = w;
    return SS_OK;
fail:
    ss_writer_release(w);
    return s;
}

typedef struct prepared_update {
    ss_cell_key_t key;
    uint32_t operation;
    ssi_mesh_t *mesh;
} prepared_update_t;
static int compare_updates(const void *a, const void *b) {
    const ss_geometry_update_t *x = *(const ss_geometry_update_t *const *)a,
                               *y = *(const ss_geometry_update_t *const *)b;
    return ssi_key_compare(x->mesh.cell, y->mesh.cell);
}
static ss_status_t emit_mesh(ssi_buffer_t *b, uint64_t *sequence, uint64_t time,
                             const ssi_mesh_t *mesh, uint64_t checkpoint) {
    ssi_buffer_t raw = {.memory = b->memory};
    ss_status_t s = ssi_mesh_encode(&raw, mesh, checkpoint);
    if (s == SS_OK)
        s = emit(b, sequence, time, SS_PACKET_GEOMETRY_PUT, raw.data, raw.size);
    ssi_buffer_clear(&raw);
    return s;
}

ss_status_t ss_writer_append_frame(ss_writer_t *w, const ss_camera_t *camera,
                                   const ss_geometry_update_t *updates, size_t count,
                                   const ss_depth_view_t *depth) {
    if (!w || !camera || (!updates && count))
        return SS_INVALID_ARGUMENT;
    if (w->finished)
        return SS_INVALID_STATE;
    if (ssi_camera_validate(camera, false, false) != SS_OK)
        return SS_INVALID_ARGUMENT;
    if ((!w->frames && camera->timestamp_ns != 0) ||
        (w->frames && (w->options.kind == SS_STILL || camera->timestamp_ns <= w->last_time ||
                       camera->raster_width != w->width || camera->raster_height != w->height)))
        return SS_INVALID_ARGUMENT;
    uint8_t camera_raw[64];
    ssi_camera_encode(camera_raw, camera);
    ss_camera_t encoded;
    if (ssi_camera_decode(camera_raw, 64, camera->timestamp_ns, &encoded) != SS_OK ||
        (!w->frames && ssi_camera_validate(&encoded, true, true) != SS_OK))
        return SS_INVALID_ARGUMENT;
    if (count > w->memory->options.max_packets)
        return SS_RESOURCE_LIMIT;
    if (depth &&
        (!SSI_VALID(depth, ss_depth_view_t) || depth->timestamp_ns != camera->timestamp_ns ||
         (w->depth_width && (depth->width != w->depth_width || depth->height != w->depth_height))))
        return SS_INVALID_ARGUMENT;
    ssi_memory_t *m = w->memory;
    ss_status_t s = SS_OK;
    ssi_buffer_t batch = {.memory = m}, depth_raw = {.memory = m};
    ssi_map_t next = {.memory = m};
    const ss_geometry_update_t **sorted = NULL;
    prepared_update_t *prepared = NULL;
    ssi_entry_t **cells = NULL;
    size_t changed = 0, bytes;
    uint64_t raw_delta = 0, sequence = w->sequence;
    if (depth) {
        s = ssi_depth_encode(&depth_raw, depth);
        if (s != SS_OK)
            goto done;
    }
    if (count) {
        if (!ssi_multiply(count, sizeof(*sorted), &bytes)) {
            s = SS_RESOURCE_LIMIT;
            goto done;
        }
        sorted = ssi_alloc(m, bytes);
        if (!ssi_multiply(count, sizeof(*prepared), &bytes)) {
            s = SS_RESOURCE_LIMIT;
            goto done;
        }
        prepared = ssi_alloc(m, bytes);
        if (!sorted || !prepared) {
            s = m->failure;
            goto done;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!SSI_VALID(&updates[i], ss_geometry_update_t) ||
                !SSI_VALID(&updates[i].mesh, ss_mesh_view_t) ||
                (updates[i].operation != SS_GEOMETRY_PUT &&
                 updates[i].operation != SS_GEOMETRY_REMOVE)) {
                s = SS_INVALID_ARGUMENT;
                goto done;
            }
            sorted[i] = &updates[i];
        }
        qsort(sorted, count, sizeof(*sorted), compare_updates);
        for (size_t i = 1; i < count; ++i)
            if (!ssi_key_compare(sorted[i - 1]->mesh.cell, sorted[i]->mesh.cell)) {
                s = SS_INVALID_ARGUMENT;
                goto done;
            }
    }
    s = ssi_map_clone(&w->scene, &next);
    if (s != SS_OK)
        goto done;
    for (size_t i = 0; i < count; ++i) {
        const ss_geometry_update_t *u = sorted[i];
        if (u->operation == SS_GEOMETRY_REMOVE) {
            s = ssi_map_remove(&next, u->mesh.cell);
            if (s == SS_MALFORMED)
                s = SS_INVALID_ARGUMENT;
            if (s != SS_OK)
                goto done;
            prepared[changed++] =
                (prepared_update_t){.key = u->mesh.cell, .operation = SS_GEOMETRY_REMOVE};
            raw_delta += 16;
        } else {
            ssi_mesh_t *mesh = NULL;
            s = ssi_mesh_canonicalize(m, &u->mesh, &mesh);
            if (s != SS_OK)
                goto done;
            ssi_entry_t *old = ssi_map_find(&next, mesh->key);
            if (old && ssi_mesh_equal(old->mesh, mesh)) {
                ssi_mesh_release(mesh);
                continue;
            }
            ssi_entry_t entry = {.key = mesh->key, .mesh = mesh, .raw_size = mesh->raw_size};
            s = ssi_map_put(&next, entry);
            ssi_mesh_release(mesh);
            if (s == SS_MALFORMED)
                s = SS_INVALID_ARGUMENT;
            if (s != SS_OK)
                goto done;
            prepared[changed++] = (prepared_update_t){
                .key = entry.key, .operation = SS_GEOMETRY_PUT, .mesh = entry.mesh};
            raw_delta += entry.raw_size;
        }
    }
    bool checkpoint =
        !w->frames ||
        (changed && (camera->timestamp_ns - w->checkpoint_time > SSPS_CHECKPOINT_MAX_DELTA_NS ||
                     raw_delta > SSPS_CHECKPOINT_MAX_DELTA_BYTES - w->delta_bytes));
    uint64_t checkpoint_id = w->checkpoint_id;
    if (checkpoint) {
        if (checkpoint_id == UINT64_MAX) {
            s = SS_OUT_OF_RANGE;
            goto done;
        }
        ++checkpoint_id;
        s = ssi_map_sorted(&next, &cells);
        if (s != SS_OK)
            goto done;
        uint8_t cp[16] = {0};
        ssi_w64(cp, checkpoint_id);
        ssi_w32(cp + 8, (uint32_t)next.count);
        s = emit(&batch, &sequence, camera->timestamp_ns, SS_PACKET_CHECKPOINT_BEGIN, cp, 16);
        if (s != SS_OK)
            goto done;
        for (size_t i = 0; i < next.count; ++i) {
            s = emit_mesh(&batch, &sequence, camera->timestamp_ns, cells[i]->mesh, checkpoint_id);
            if (s != SS_OK)
                goto done;
        }
        s = emit(&batch, &sequence, camera->timestamp_ns, SS_PACKET_CHECKPOINT_END, cp, 16);
        if (s != SS_OK)
            goto done;
    } else {
        for (size_t i = 0; i < changed; ++i) {
            prepared_update_t *u = &prepared[i];
            if (u->operation == SS_GEOMETRY_PUT)
                s = emit_mesh(&batch, &sequence, camera->timestamp_ns, u->mesh, 0);
            else {
                uint8_t remove[16] = {0};
                ssi_w32(remove, (uint32_t)u->key.x);
                ssi_w32(remove + 4, (uint32_t)u->key.y);
                ssi_w32(remove + 8, (uint32_t)u->key.z);
                s = emit(&batch, &sequence, camera->timestamp_ns, SS_PACKET_GEOMETRY_REMOVE, remove,
                         16);
            }
            if (s != SS_OK)
                goto done;
        }
    }
    s = emit(&batch, &sequence, camera->timestamp_ns, SS_PACKET_CAMERA_SAMPLE, camera_raw,
             sizeof(camera_raw));
    if (s != SS_OK)
        goto done;
    if (depth) {
        s = emit(&batch, &sequence, camera->timestamp_ns, SS_PACKET_DEPTH_SAMPLE, depth_raw.data,
                 depth_raw.size);
        if (s != SS_OK)
            goto done;
    }
    s = ssi_buffer_append(&w->buffer, batch.data, batch.size);
    if (s != SS_OK)
        goto done;
    ssi_map_clear(&w->scene);
    w->scene = next;
    next = (ssi_map_t){.memory = m};
    w->sequence = sequence;
    ++w->frames;
    w->last_time = camera->timestamp_ns;
    w->width = camera->raster_width;
    w->height = camera->raster_height;
    if (depth) {
        w->depth_width = depth->width;
        w->depth_height = depth->height;
    }
    if (checkpoint) {
        w->checkpoint_id = checkpoint_id;
        w->checkpoint_time = camera->timestamp_ns;
        w->delta_bytes = 0;
    } else
        w->delta_bytes += raw_delta;
done:
    ssi_map_clear(&next);
    ssi_free(sorted);
    ssi_free(prepared);
    ssi_free(cells);
    ssi_buffer_clear(&batch);
    ssi_buffer_clear(&depth_raw);
    return s;
}

ss_status_t ss_writer_finish(ss_writer_t *w, uint64_t duration) {
    if (!w)
        return SS_INVALID_ARGUMENT;
    if (w->finished)
        return SS_INVALID_STATE;
    if (!w->frames || duration < w->last_time ||
        (w->options.kind == SS_STILL ? duration != 0 : duration == 0))
        return SS_INVALID_ARGUMENT;
    size_t original = w->buffer.size;
    uint64_t sequence = w->sequence;
    ss_status_t s = emit(&w->buffer, &sequence, duration, SS_PACKET_STREAM_END, NULL, 0);
    if (s != SS_OK)
        return s;
    ss_document_t *validated = NULL;
    s = ssi_document_parse(w->memory, w->buffer.data, w->buffer.size, false, &validated, NULL);
    ss_document_release(validated);
    if (s != SS_OK) {
        w->buffer.size = original;
        return s;
    }
    w->sequence = sequence;
    w->finished = true;
    return SS_OK;
}
ss_status_t ss_writer_bytes(const ss_writer_t *w, const uint8_t **bytes, size_t *size) {
    if (!w || !bytes || !size)
        return SS_INVALID_ARGUMENT;
    if (!w->finished)
        return SS_INVALID_STATE;
    *bytes = w->buffer.data;
    *size = w->buffer.size;
    return SS_OK;
}
void ss_writer_release(ss_writer_t *w) {
    if (!w)
        return;
    ssi_memory_t *m = w->memory;
    ssi_buffer_clear(&w->buffer);
    ssi_map_clear(&w->scene);
    ssi_free(w);
    ssi_memory_release(m);
}
