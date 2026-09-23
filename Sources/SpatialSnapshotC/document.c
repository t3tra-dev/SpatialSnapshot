#include "internal.h"

typedef enum parse_state {
    EXPECT_SCENE_INFO,
    EXPECT_FIRST_CHECKPOINT,
    IN_CHECKPOINT,
    EXPECT_FIRST_CAMERA,
    RUNNING,
    ENDED
} parse_state_t;

static bool header_crc(const uint8_t *bytes, size_t n) {
    uint8_t header[SSPS_MAX_HEADER_BYTES];
    memcpy(header, bytes, n);
    uint32_t expected = ssi_u32(header + 40);
    memset(header + 40, 0, 4);
    return ss_crc32c(header, n) == expected;
}

ss_status_t ssi_document_parse(ssi_memory_t *memory, uint8_t *data, size_t size, bool owned,
                               ss_document_t **out, ss_error_t *error) {
    *out = NULL;
    ss_status_t status = SS_MALFORMED;
    const char *message = "incomplete stream header";
    size_t offset = 0;
    uint64_t sequence = 0;
    uint32_t type = 0;
    uint8_t *raw = NULL;
    ssi_mesh_t *mesh = NULL;
    ssi_map_t scene = {.memory = memory}, staging = {.memory = memory};
    ss_document_t *d = ssi_alloc(memory, sizeof(*d));
    if (!d) {
        if (owned)
            ssi_free(data);
        ssi_error(error, memory->failure, 0, 0, 0, ss_status_string(memory->failure));
        return memory->failure;
    }
    memset(d, 0, sizeof(*d));
    d->memory = memory;
    ssi_memory_retain(memory);
    d->references = 1;
    d->data = data;
    d->size = size;
    d->owns_data = owned;
    d->info.struct_size = sizeof(d->info);
    d->info.abi_version = SS_ABI_VERSION;
#define FAIL(code, text)                                                                           \
    do {                                                                                           \
        status = (code);                                                                           \
        message = (text);                                                                          \
        goto fail;                                                                                 \
    } while (0)
#define REQUIRE(condition, text)                                                                   \
    do {                                                                                           \
        if (!(condition))                                                                          \
            FAIL(SS_MALFORMED, text);                                                              \
    } while (0)
#define CHECK(expression, text)                                                                    \
    do {                                                                                           \
        status = (expression);                                                                     \
        if (status != SS_OK) {                                                                     \
            message = (text);                                                                      \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)
    REQUIRE(size >= 64, "incomplete stream header");
    REQUIRE(memcmp(data, "SSPS\r\n\032\n", 8) == 0, "invalid stream magic");
    uint16_t header_size = ssi_u16(data + 12);
    REQUIRE(header_size >= 64 && header_size <= SSPS_MAX_HEADER_BYTES && header_size <= size,
            "invalid stream header size");
    REQUIRE(header_crc(data, header_size), "stream header CRC32C mismatch");
    d->info.version_major = ssi_u16(data + 8);
    d->info.version_minor = ssi_u16(data + 10);
    if (d->info.version_major != 1)
        FAIL(SS_UNSUPPORTED, "unsupported SSPS major version");
    REQUIRE(ssi_u32(data + 16) == 0 && ssi_u16(data + 20) == 48 && ssi_u16(data + 22) == 0 &&
                ssi_zero(data + 44, 20),
            "invalid stream header fields");
    REQUIRE(!ssi_zero(data + 24, 16), "zero stream identifier");
    d->info.kind = ssi_u16(data + 14);
    if (d->info.kind != SS_STILL && d->info.kind != SS_VIDEO)
        FAIL(SS_UNSUPPORTED, "unsupported stream kind");
    memcpy(d->info.stream_id, data + 24, 16);
    offset = header_size;
    parse_state_t state = EXPECT_SCENE_INFO;
    uint64_t time = 0, checkpoint_id = 0, checkpoint_time = 0, delta_bytes = 0;
    uint32_t checkpoint_chunks = 0, checkpoint_members = 0, depth_width = 0, depth_height = 0;
    size_t checkpoint_begin = 0;
    unsigned phase = 0;
    bool group_geometry = false, group_camera = false, group_checkpoint = false, has_key = false;
    ss_cell_key_t last_key = {0, 0, 0};

    while (offset < size) {
        REQUIRE(state != ENDED, "bytes after STREAM_END");
        REQUIRE(size - offset >= 48, "incomplete packet header");
        const uint8_t *p = data + offset;
        REQUIRE(memcmp(p, "SSPK", 4) == 0, "invalid packet magic");
        size_t hs = ssi_u16(p + 4);
        type = ssi_u16(p + 6);
        REQUIRE(hs >= 48 && hs <= SSPS_MAX_HEADER_BYTES && hs <= size - offset,
                "invalid packet header size");
        REQUIRE(header_crc(p, hs), "packet header CRC32C mismatch");
        REQUIRE(ssi_u32(p + 44) == 0 && type != 0xffff, "invalid packet reserved field or type");
        uint32_t flags = ssi_u32(p + 8), stored_size = ssi_u32(p + 28), raw_size = ssi_u32(p + 32);
        uint64_t timestamp = ssi_u64(p + 20);
        REQUIRE(ssi_u64(p + 12) == sequence, "packet sequence discontinuity");
        REQUIRE(timestamp >= time, "decreasing timestamp");
        REQUIRE(stored_size <= SSPS_MAX_PACKET_BYTES && raw_size <= SSPS_MAX_PACKET_BYTES,
                "packet exceeds format size limit");
        REQUIRE(stored_size <= size - offset - hs, "truncated packet payload");
        REQUIRE((flags & SS_FLAG_LZ4) || stored_size == raw_size,
                "uncompressed payload size mismatch");
        REQUIRE(ss_crc32c(p + hs, stored_size) == ssi_u32(p + 36),
                "stored payload CRC32C mismatch");
        bool known = ssi_known(type);
        if (!known && (flags & SS_FLAG_CRITICAL))
            FAIL(SS_UNSUPPORTED_CRITICAL_PACKET, "unknown critical packet");
        if (known) {
            uint32_t required = SS_FLAG_CRITICAL;
            if (type == SS_PACKET_GEOMETRY_PUT || type == SS_PACKET_DEPTH_SAMPLE)
                required |= SS_FLAG_LZ4;
            REQUIRE(flags == required, "invalid required packet flags");
        }
        if (d->packet_count >= memory->options.max_packets)
            FAIL(SS_RESOURCE_LIMIT, "packet index resource limit");
        if (d->info.kind == SS_STILL)
            REQUIRE(timestamp == 0, "STILL timestamp must be zero");
        if (timestamp != time) {
            REQUIRE(state != IN_CHECKPOINT, "checkpoint spans timestamps");
            REQUIRE(!group_geometry || group_camera, "geometry timestamp has no camera");
            phase = 0;
            group_geometry = group_camera = group_checkpoint = has_key = false;
            time = timestamp;
        }
        ssi_packet_t packet = {.info = {.struct_size = sizeof(ss_packet_info_t),
                                        .abi_version = SS_ABI_VERSION,
                                        .sequence_number = sequence,
                                        .timestamp_ns = timestamp,
                                        .byte_offset = offset,
                                        .type = type,
                                        .flags = flags,
                                        .header_size = (uint32_t)hs,
                                        .stored_payload_size = stored_size,
                                        .raw_payload_size = raw_size}};
        if (known)
            CHECK(ssi_packet_raw(d, &packet, &raw), "payload decode or resource limit");
        if (!known) {
            REQUIRE(state == RUNNING && (!group_geometry || group_camera),
                    "extension violates packet ordering");
            phase = 4;
        } else
            switch (type) {
            case SS_PACKET_SCENE_INFO: {
                REQUIRE(state == EXPECT_SCENE_INFO && sequence == 0 && timestamp == 0,
                        "SCENE_INFO must occur once, first, at zero");
                REQUIRE(raw_size == 32, "invalid SCENE_INFO size");
                double gx = ssi_f32(raw), gy = ssi_f32(raw + 4), gz = ssi_f32(raw + 8);
                REQUIRE(isfinite(gx) && isfinite(gy) && isfinite(gz) && ssi_f32(raw + 12) == 0 &&
                            ssi_u32(raw + 16) == 1 && ssi_u32(raw + 20) == 500000 &&
                            ssi_u32(raw + 24) == 1000 && ssi_u32(raw + 28) == 0,
                        "invalid SCENE_INFO fields");
                double norm = sqrt(gx * gx + gy * gy + gz * gz);
                REQUIRE(fabs(norm - 1) <= 1e-4, "invalid gravity norm");
                d->info.gravity = (ss_vec3_t){gx / norm, gy / norm, gz / norm};
                state = EXPECT_FIRST_CHECKPOINT;
                break;
            }
            case SS_PACKET_CHECKPOINT_BEGIN:
                REQUIRE((state == EXPECT_FIRST_CHECKPOINT || state == RUNNING) && phase == 0 &&
                            !group_geometry,
                        "CHECKPOINT_BEGIN violates ordering");
                REQUIRE(raw_size == 16 && ssi_u32(raw + 12) == 0,
                        "invalid CHECKPOINT_BEGIN payload");
                REQUIRE(checkpoint_id != UINT64_MAX && ssi_u64(raw) == checkpoint_id + 1,
                        "checkpoint ID discontinuity");
                if (!checkpoint_id)
                    REQUIRE(timestamp == 0, "first checkpoint must be at zero");
                else
                    REQUIRE(d->info.kind == SS_VIDEO, "multiple checkpoints in STILL");
                checkpoint_chunks = ssi_u32(raw + 8);
                REQUIRE(checkpoint_chunks <= SSPS_MAX_SCENE_CELLS,
                        "checkpoint exceeds format chunk limit");
                if (checkpoint_chunks > memory->options.max_scene_cells)
                    FAIL(SS_RESOURCE_LIMIT, "checkpoint chunk resource limit");
                ++checkpoint_id;
                checkpoint_time = timestamp;
                checkpoint_members = 0;
                checkpoint_begin = d->packet_count;
                group_checkpoint = group_geometry = true;
                has_key = false;
                phase = 1;
                state = IN_CHECKPOINT;
                packet.checkpoint_id = checkpoint_id;
                packet.chunks = checkpoint_chunks;
                break;
            case SS_PACKET_GEOMETRY_PUT: {
                REQUIRE(state == IN_CHECKPOINT || (state == RUNNING && phase <= 1 &&
                                                   !group_checkpoint && d->info.kind == SS_VIDEO),
                        "GEOMETRY_PUT violates ordering");
                REQUIRE(raw_size >= 32, "invalid GEOMETRY_PUT prefix");
                packet.checkpoint_id = ssi_u64(raw);
                REQUIRE(packet.checkpoint_id == (state == IN_CHECKPOINT ? checkpoint_id : 0),
                        "geometry checkpoint membership mismatch");
                if (state == IN_CHECKPOINT)
                    REQUIRE(checkpoint_members < checkpoint_chunks, "too many checkpoint members");
                CHECK(ssi_mesh_decode(memory, raw, raw_size, &mesh),
                      "invalid geometry or geometry resource limit");
                packet.key = mesh->key;
                REQUIRE(!has_key || ssi_key_compare(last_key, packet.key) < 0,
                        "duplicate or unsorted geometry cell key");
                last_key = packet.key;
                has_key = true;
                ssi_entry_t entry = {
                    .key = packet.key, .packet = d->packet_count, .raw_size = raw_size};
                CHECK(ssi_map_put(state == IN_CHECKPOINT ? &staging : &scene, entry),
                      "scene size or geometry resource limit");
                if (state == IN_CHECKPOINT)
                    ++checkpoint_members;
                else {
                    REQUIRE(timestamp - checkpoint_time <= SSPS_CHECKPOINT_MAX_DELTA_NS &&
                                raw_size <= SSPS_CHECKPOINT_MAX_DELTA_BYTES - delta_bytes,
                            "checkpoint cadence exceeded");
                    delta_bytes += raw_size;
                    group_geometry = true;
                    phase = 1;
                }
                ssi_mesh_release(mesh);
                mesh = NULL;
                break;
            }
            case SS_PACKET_GEOMETRY_REMOVE:
                REQUIRE(state == RUNNING && phase <= 1 && !group_checkpoint &&
                            d->info.kind == SS_VIDEO,
                        "GEOMETRY_REMOVE violates ordering");
                REQUIRE(raw_size == 16 && ssi_u32(raw + 12) == 0,
                        "invalid GEOMETRY_REMOVE payload");
                packet.key = (ss_cell_key_t){ssi_i32(raw), ssi_i32(raw + 4), ssi_i32(raw + 8)};
                REQUIRE(!has_key || ssi_key_compare(last_key, packet.key) < 0,
                        "duplicate or unsorted geometry cell key");
                last_key = packet.key;
                has_key = true;
                CHECK(ssi_map_remove(&scene, packet.key), "removal of absent geometry cell");
                REQUIRE(timestamp - checkpoint_time <= SSPS_CHECKPOINT_MAX_DELTA_NS &&
                            raw_size <= SSPS_CHECKPOINT_MAX_DELTA_BYTES - delta_bytes,
                        "checkpoint cadence exceeded");
                delta_bytes += raw_size;
                group_geometry = true;
                phase = 1;
                break;
            case SS_PACKET_CHECKPOINT_END:
                REQUIRE(state == IN_CHECKPOINT && raw_size == 16, "unexpected CHECKPOINT_END");
                REQUIRE(ssi_u64(raw) == checkpoint_id && ssi_u32(raw + 8) == checkpoint_chunks &&
                            ssi_u32(raw + 12) == 0 && checkpoint_members == checkpoint_chunks,
                        "checkpoint ID or chunk count mismatch");
                packet.checkpoint_id = checkpoint_id;
                packet.chunks = checkpoint_chunks;
                CHECK(ssi_grow(memory, (void **)&d->checkpoints, &d->checkpoint_capacity,
                               d->checkpoint_count + 1, sizeof(*d->checkpoints)),
                      "checkpoint index resource limit");
                d->checkpoints[d->checkpoint_count++] =
                    (ssi_checkpoint_t){timestamp, checkpoint_begin, d->packet_count};
                /* The staged state becomes current only after all END checks. */
                ssi_map_clear(&scene);
                scene = staging;
                staging = (ssi_map_t){.memory = memory};
                delta_bytes = 0;
                state = d->frame_count ? RUNNING : EXPECT_FIRST_CAMERA;
                break;
            case SS_PACKET_CAMERA_SAMPLE: {
                REQUIRE((state == EXPECT_FIRST_CAMERA || state == RUNNING) && phase <= 1 &&
                            !group_camera,
                        "CAMERA_SAMPLE violates ordering");
                ss_camera_t camera;
                CHECK(ssi_camera_decode(raw, raw_size, timestamp, &camera),
                      "invalid camera sample");
                if (!d->frame_count)
                    CHECK(ssi_camera_validate(&camera, true, true),
                          "first camera must be identity at zero");
                else {
                    REQUIRE(d->info.kind == SS_VIDEO, "multiple camera samples in STILL");
                    REQUIRE(camera.raster_width == d->frames[0].camera.raster_width &&
                                camera.raster_height == d->frames[0].camera.raster_height,
                            "camera raster dimensions changed");
                }
                CHECK(ssi_grow(memory, (void **)&d->frames, &d->frame_capacity, d->frame_count + 1,
                               sizeof(*d->frames)),
                      "camera index resource limit");
                d->frames[d->frame_count++] =
                    (ssi_frame_t){.camera = camera, .depth_packet = SIZE_MAX};
                state = RUNNING;
                phase = 2;
                group_camera = true;
                break;
            }
            case SS_PACKET_DEPTH_SAMPLE:
                REQUIRE(state == RUNNING && phase == 2 && group_camera,
                        "depth lacks matching camera or violates ordering");
                CHECK(ssi_depth_validate_raw(raw, raw_size), "invalid depth sample");
                if (depth_width)
                    REQUIRE(depth_width == ssi_u32(raw) && depth_height == ssi_u32(raw + 4),
                            "depth raster dimensions changed");
                depth_width = ssi_u32(raw);
                depth_height = ssi_u32(raw + 4);
                d->frames[d->frame_count - 1].depth_packet = d->packet_count;
                phase = 3;
                break;
            case SS_PACKET_STREAM_END:
                REQUIRE(state == RUNNING && (!group_geometry || group_camera),
                        "STREAM_END before required samples");
                REQUIRE(raw_size == 0, "nonempty STREAM_END payload");
                REQUIRE(d->info.kind == SS_STILL || timestamp > 0,
                        "VIDEO duration must be positive");
                d->info.duration_ns = timestamp;
                state = ENDED;
                break;
            default:
                FAIL(SS_MALFORMED, "invalid packet state");
            }
        ssi_free(raw);
        raw = NULL;
        CHECK(ssi_grow(memory, (void **)&d->packets, &d->packet_capacity, d->packet_count + 1,
                       sizeof(*d->packets)),
              "packet index resource limit");
        d->packets[d->packet_count++] = packet;
        offset += hs + stored_size;
        REQUIRE(sequence != UINT64_MAX || offset == size, "packet sequence wraparound");
        if (sequence != UINT64_MAX)
            ++sequence;
    }
    REQUIRE(state == ENDED, "missing STREAM_END or incomplete checkpoint");
    d->info.packet_count = d->packet_count;
    d->info.camera_count = d->frame_count;
    d->info.checkpoint_count = d->checkpoint_count;
    ssi_map_clear(&scene);
    ssi_map_clear(&staging);
    ssi_error(error, SS_OK, 0, 0, 0, "OK");
    *out = d;
    return SS_OK;
fail:
    ssi_error(error, status, offset, sequence, type, message);
    ssi_free(raw);
    ssi_mesh_release(mesh);
    ssi_map_clear(&scene);
    ssi_map_clear(&staging);
    ss_document_release(d);
    return status;
#undef FAIL
#undef REQUIRE
#undef CHECK
}

ss_status_t ss_document_open_memory(const void *bytes, size_t size,
                                    const ss_open_options_t *options, ss_document_t **out,
                                    ss_error_t *error) {
    if (out)
        *out = NULL;
    if (!out || (!bytes && size) || (error && !SSI_VALID(error, ss_error_t)))
        return SS_INVALID_ARGUMENT;
    ssi_memory_t *m = NULL;
    ss_status_t s = ssi_memory_create(options, &m);
    if (s != SS_OK) {
        ssi_error(error, s, 0, 0, 0, ss_status_string(s));
        return s;
    }
    uint8_t *copy = size ? ssi_alloc(m, size) : NULL;
    if (size && !copy) {
        s = m->failure;
        ssi_error(error, s, 0, 0, 0, ss_status_string(s));
    } else {
        if (size)
            memcpy(copy, bytes, size);
        s = ssi_document_parse(m, copy, size, true, out, error);
    }
    ssi_memory_release(m);
    return s;
}
ss_status_t ss_document_open(const ss_io_t *io, const ss_open_options_t *options,
                             ss_document_t **out, ss_error_t *error) {
    if (out)
        *out = NULL;
    if (!out || !SSI_VALID(io, ss_io_t) || !io->read_at || !io->size ||
        (error && !SSI_VALID(error, ss_error_t)))
        return SS_INVALID_ARGUMENT;
    ssi_memory_t *m = NULL;
    ss_status_t s = ssi_memory_create(options, &m);
    if (s != SS_OK) {
        ssi_error(error, s, 0, 0, 0, ss_status_string(s));
        return s;
    }
    uint64_t size = io->size(io->context);
    if (size > SIZE_MAX)
        s = SS_RESOURCE_LIMIT;
    else {
        uint8_t *data = size ? ssi_alloc(m, (size_t)size) : NULL;
        if (size && !data)
            s = m->failure;
        else if (size && io->read_at(io->context, 0, data, (size_t)size) != SS_OK) {
            ssi_free(data);
            s = SS_IO_ERROR;
        } else {
            s = ssi_document_parse(m, data, (size_t)size, true, out, error);
            ssi_memory_release(m);
            return s;
        }
    }
    ssi_error(error, s, 0, 0, 0, ss_status_string(s));
    ssi_memory_release(m);
    return s;
}
void ss_document_retain(ss_document_t *d) {
    if (d)
        ++d->references;
}
void ss_document_release(ss_document_t *d) {
    if (!d || --d->references)
        return;
    ssi_memory_t *m = d->memory;
    if (d->owns_data)
        ssi_free(d->data);
    ssi_free(d->packets);
    ssi_free(d->frames);
    ssi_free(d->checkpoints);
    ssi_free(d);
    ssi_memory_release(m);
}
ss_status_t ss_document_get_info(const ss_document_t *d, ss_document_info_t *info) {
    if (!d || !SSI_VALID(info, ss_document_info_t))
        return SS_INVALID_ARGUMENT;
    *info = d->info;
    return SS_OK;
}
ss_status_t ss_document_camera(const ss_document_t *d, uint64_t i, ss_camera_t *camera) {
    if (!d || !SSI_VALID(camera, ss_camera_t))
        return SS_INVALID_ARGUMENT;
    if (i >= d->frame_count)
        return SS_OUT_OF_RANGE;
    *camera = d->frames[(size_t)i].camera;
    return SS_OK;
}
ss_status_t ss_document_packet(const ss_document_t *d, uint64_t i, ss_packet_info_t *info) {
    if (!d || !SSI_VALID(info, ss_packet_info_t))
        return SS_INVALID_ARGUMENT;
    if (i >= d->packet_count)
        return SS_OUT_OF_RANGE;
    *info = d->packets[(size_t)i].info;
    return SS_OK;
}
ss_status_t ss_document_packet_payload(const ss_document_t *d, uint64_t i, const uint8_t **bytes,
                                       size_t *size) {
    if (!d || !bytes || !size)
        return SS_INVALID_ARGUMENT;
    if (i >= d->packet_count)
        return SS_OUT_OF_RANGE;
    const ss_packet_info_t *p = &d->packets[(size_t)i].info;
    *bytes = d->data + (size_t)p->byte_offset + p->header_size;
    *size = p->stored_payload_size;
    return SS_OK;
}
ss_status_t ss_validate(const void *bytes, size_t size, const ss_open_options_t *options,
                        ss_error_t *error) {
    ss_document_t *d = NULL;
    ss_status_t s = ss_document_open_memory(bytes, size, options, &d, error);
    ss_document_release(d);
    return s;
}

ss_status_t ss_decoder_create(const ss_open_options_t *options, ss_decoder_t **out) {
    if (!out)
        return SS_INVALID_ARGUMENT;
    *out = NULL;
    ssi_memory_t *m = NULL;
    ss_status_t s = ssi_memory_create(options, &m);
    if (s != SS_OK)
        return s;
    ss_decoder_t *d = ssi_alloc(m, sizeof(*d));
    if (!d) {
        s = m->failure;
        ssi_memory_release(m);
        return s;
    }
    *d = (ss_decoder_t){.memory = m, .buffer = {.memory = m}};
    *out = d;
    return SS_OK;
}
ss_status_t ss_decoder_feed(ss_decoder_t *d, const void *bytes, size_t size) {
    if (!d || (!bytes && size))
        return SS_INVALID_ARGUMENT;
    if (d->finished)
        return SS_INVALID_STATE;
    if (d->failure)
        return d->failure;
    d->failure = ssi_buffer_append(&d->buffer, bytes, size);
    return d->failure;
}
ss_status_t ss_decoder_finish(ss_decoder_t *d, ss_document_t **out, ss_error_t *error) {
    if (out)
        *out = NULL;
    if (!d || !out || (error && !SSI_VALID(error, ss_error_t)))
        return SS_INVALID_ARGUMENT;
    if (d->finished)
        return SS_INVALID_STATE;
    if (d->failure) {
        ssi_error(error, d->failure, 0, 0, 0, ss_status_string(d->failure));
        return d->failure;
    }
    d->finished = true;
    uint8_t *bytes = d->buffer.data;
    size_t size = d->buffer.size;
    d->buffer.data = NULL;
    d->buffer.size = d->buffer.capacity = 0;
    return ssi_document_parse(d->memory, bytes, size, true, out, error);
}
void ss_decoder_release(ss_decoder_t *d) {
    if (!d)
        return;
    ssi_memory_t *m = d->memory;
    ssi_buffer_clear(&d->buffer);
    ssi_free(d);
    ssi_memory_release(m);
}

size_t ssi_find_frame(const ss_document_t *d, uint64_t time) {
    size_t lo = 0, hi = d->frame_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->frames[mid].camera.timestamp_ns < time)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < d->frame_count && d->frames[lo].camera.timestamp_ns == time ? lo : SIZE_MAX;
}
ss_status_t ss_document_validate_media_binding(const ss_document_t *d, uint32_t w, uint32_t h,
                                               const uint64_t *times, size_t count,
                                               uint64_t duration) {
    if (!d || (!times && count))
        return SS_INVALID_ARGUMENT;
    if (count != d->frame_count || duration != d->info.duration_ns ||
        w != d->frames[0].camera.raster_width || h != d->frames[0].camera.raster_height)
        return SS_MALFORMED;
    for (size_t i = 0; i < count; ++i)
        if (times[i] != d->frames[i].camera.timestamp_ns)
            return SS_MALFORMED;
    return SS_OK;
}
