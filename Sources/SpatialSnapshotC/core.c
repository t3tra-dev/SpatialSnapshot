#include "internal.h"

typedef union ssi_allocation {
    struct {
        size_t bytes;
        ssi_memory_t *memory;
    } info;
    max_align_t alignment;
} ssi_allocation_t;

static void *default_allocate(void *context, size_t bytes) {
    (void)context;
    return malloc(bytes);
}
static void default_deallocate(void *context, void *p) {
    (void)context;
    free(p);
}

uint32_t ss_version(void) {
    return SS_VERSION;
}
const char *ss_status_string(ss_status_t status) {
    switch (status) {
    case SS_OK:
        return "OK";
    case SS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case SS_MALFORMED:
        return "MALFORMED";
    case SS_UNSUPPORTED:
        return "UNSUPPORTED";
    case SS_UNSUPPORTED_CRITICAL_PACKET:
        return "UNSUPPORTED_CRITICAL_PACKET";
    case SS_RESOURCE_LIMIT:
        return "RESOURCE_LIMIT";
    case SS_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case SS_IO_ERROR:
        return "IO_ERROR";
    case SS_NO_SAMPLE:
        return "NO_SAMPLE";
    case SS_NOT_FOUND:
        return "NOT_FOUND";
    case SS_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case SS_INVALID_STATE:
        return "INVALID_STATE";
    case SS_BINDING_UNREPRESENTABLE:
        return "BINDING_UNREPRESENTABLE";
    case SS_DECODER_UNAVAILABLE:
        return "DECODER_UNAVAILABLE";
    default:
        return "UNKNOWN_STATUS";
    }
}

void ss_open_options_init(ss_open_options_t *o) {
    if (!o)
        return;
    memset(o, 0, sizeof(*o));
    o->struct_size = sizeof(*o);
    o->abi_version = SS_ABI_VERSION;
    o->allocator.struct_size = sizeof(o->allocator);
    o->allocator.abi_version = SS_ABI_VERSION;
}
void ss_writer_options_init(ss_writer_options_t *o) {
    if (!o)
        return;
    memset(o, 0, sizeof(*o));
    o->struct_size = sizeof(*o);
    o->abi_version = SS_ABI_VERSION;
    o->kind = SS_VIDEO;
    o->gravity.y = 1;
    ss_open_options_init(&o->resources);
}
void ss_camera_init(ss_camera_t *c) {
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->struct_size = sizeof(*c);
    c->abi_version = SS_ABI_VERSION;
    c->raster_width = c->raster_height = 1;
    c->fx = c->fy = c->qw = 1;
}

ss_status_t ssi_memory_create(const ss_open_options_t *options, ssi_memory_t **out) {
    ss_open_options_t o;
    ss_open_options_init(&o);
    if (options) {
        if (!SSI_VALID(options, ss_open_options_t))
            return SS_INVALID_ARGUMENT;
        o = *options;
    }
    if (!!o.allocator.allocate != !!o.allocator.deallocate)
        return SS_INVALID_ARGUMENT;
    if (o.allocator.allocate && !SSI_VALID(&o.allocator, ss_allocator_t))
        return SS_INVALID_ARGUMENT;
    if (!o.allocator.allocate) {
        o.allocator.allocate = default_allocate;
        o.allocator.deallocate = default_deallocate;
    }
    if (!o.max_memory_bytes)
        o.max_memory_bytes = UINT64_C(268435456);
    if (!o.max_geometry_bytes)
        o.max_geometry_bytes = UINT64_C(134217728);
    if (!o.max_packets)
        o.max_packets = UINT64_C(1000000);
    if (!o.max_partition_work)
        o.max_partition_work = UINT64_C(1000000);
    if (!o.max_scene_cells)
        o.max_scene_cells = SSPS_MAX_SCENE_CELLS;
    if (!o.max_packet_raw_bytes)
        o.max_packet_raw_bytes = SSPS_MAX_PACKET_BYTES;
    if (o.max_memory_bytes < sizeof(ssi_memory_t))
        return SS_RESOURCE_LIMIT;
    ssi_memory_t *m = o.allocator.allocate(o.allocator.context, sizeof(*m));
    if (!m)
        return SS_OUT_OF_MEMORY;
    memset(m, 0, sizeof(*m));
    m->options = o;
    m->used = sizeof(*m);
    m->references = 1;
    *out = m;
    return SS_OK;
}
void ssi_memory_retain(ssi_memory_t *m) {
    ++m->references;
}
void ssi_memory_release(ssi_memory_t *m) {
    if (m && --m->references == 0)
        m->options.allocator.deallocate(m->options.allocator.context, m);
}
bool ssi_add(size_t a, size_t b, size_t *r) {
    if (b > SIZE_MAX - a)
        return false;
    *r = a + b;
    return true;
}
bool ssi_multiply(size_t a, size_t b, size_t *r) {
    if (a && b > SIZE_MAX / a)
        return false;
    *r = a * b;
    return true;
}
void *ssi_alloc(ssi_memory_t *m, size_t bytes) {
    size_t total;
    if (!ssi_add(bytes, sizeof(ssi_allocation_t), &total) || total > SIZE_MAX - m->used ||
        total > m->options.max_memory_bytes - m->used) {
        m->failure = SS_RESOURCE_LIMIT;
        return NULL;
    }
    ssi_allocation_t *p = m->options.allocator.allocate(m->options.allocator.context, total);
    if (!p) {
        m->failure = SS_OUT_OF_MEMORY;
        return NULL;
    }
    p->info.bytes = total;
    p->info.memory = m;
    m->used += total;
    return p + 1;
}
void ssi_free(void *pointer) {
    if (!pointer)
        return;
    ssi_allocation_t *p = (ssi_allocation_t *)pointer - 1;
    ssi_memory_t *m = p->info.memory;
    m->used -= p->info.bytes;
    m->options.allocator.deallocate(m->options.allocator.context, p);
}
ss_status_t ssi_grow(ssi_memory_t *m, void **data, size_t *capacity, size_t count, size_t element) {
    if (count <= *capacity)
        return SS_OK;
    size_t next = *capacity ? *capacity : 8, bytes;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    if (!ssi_multiply(next, element, &bytes))
        return SS_RESOURCE_LIMIT;
    void *p = ssi_alloc(m, bytes);
    if (!p)
        return m->failure;
    if (*data)
        memcpy(p, *data, *capacity * element);
    ssi_free(*data);
    *data = p;
    *capacity = next;
    return SS_OK;
}
ss_status_t ssi_buffer_append(ssi_buffer_t *b, const void *p, size_t n) {
    size_t count;
    if (!ssi_add(b->size, n, &count))
        return SS_RESOURCE_LIMIT;
    ss_status_t s = ssi_grow(b->memory, (void **)&b->data, &b->capacity, count, 1);
    if (s != SS_OK)
        return s;
    if (n)
        memcpy(b->data + b->size, p, n);
    b->size = count;
    return SS_OK;
}
void ssi_buffer_clear(ssi_buffer_t *b) {
    ssi_free(b->data);
    b->data = NULL;
    b->size = b->capacity = 0;
}

uint16_t ssi_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
uint32_t ssi_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t ssi_u64(const uint8_t *p) {
    return ssi_u32(p) | (uint64_t)ssi_u32(p + 4) << 32;
}
int32_t ssi_i32(const uint8_t *p) {
    uint32_t u = ssi_u32(p);
    return u <= INT32_MAX ? (int32_t)u : -1 - (int32_t)(UINT32_MAX - u);
}
float ssi_f32(const uint8_t *p) {
    uint32_t u = ssi_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
void ssi_w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void ssi_w32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
void ssi_w64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
void ssi_wf32(uint8_t *p, float v) {
    uint32_t u = 0;
    if (v != 0)
        memcpy(&u, &v, 4);
    ssi_w32(p, u);
}
bool ssi_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i])
            return false;
    return true;
}
bool ssi_known(uint32_t t) {
    return t == SS_PACKET_SCENE_INFO || t == SS_PACKET_CAMERA_SAMPLE ||
           (t >= SS_PACKET_CHECKPOINT_BEGIN && t <= SS_PACKET_CHECKPOINT_END) ||
           t == SS_PACKET_DEPTH_SAMPLE || t == SS_PACKET_STREAM_END;
}
void ssi_error(ss_error_t *e, ss_status_t s, uint64_t offset, uint64_t sequence, uint32_t type,
               const char *message) {
    if (!SSI_VALID(e, ss_error_t))
        return;
    e->status = s;
    e->byte_offset = offset;
    e->sequence_number = sequence;
    e->packet_type = type;
    size_t n = strlen(message);
    if (n >= sizeof(e->message))
        n = sizeof(e->message) - 1;
    memcpy(e->message, message, n);
    e->message[n] = 0;
}

int ssi_key_compare(ss_cell_key_t a, ss_cell_key_t b) {
    if (a.x != b.x)
        return a.x < b.x ? -1 : 1;
    if (a.y != b.y)
        return a.y < b.y ? -1 : 1;
    return a.z < b.z ? -1 : a.z != b.z;
}
static uint64_t hash_key(ss_cell_key_t k) {
    uint64_t h = (uint32_t)k.x * UINT64_C(0x9e3779b185ebca87);
    h ^= (uint32_t)k.y * UINT64_C(0xc2b2ae3d27d4eb4f);
    h ^= (uint32_t)k.z * UINT64_C(0x165667b19e3779f9);
    h ^= h >> 29;
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 32;
    return h;
}
ssi_entry_t *ssi_map_find(const ssi_map_t *m, ss_cell_key_t key) {
    if (!m->capacity)
        return NULL;
    size_t i = (size_t)hash_key(key) & (m->capacity - 1);
    for (size_t n = 0; n < m->capacity; ++n, i = (i + 1) & (m->capacity - 1)) {
        ssi_entry_t *e = &m->entries[i];
        if (!e->state)
            return NULL;
        if (e->state == 1 && !ssi_key_compare(key, e->key))
            return e;
    }
    return NULL;
}
static void map_insert_unchecked(ssi_map_t *m, ssi_entry_t e) {
    size_t i = (size_t)hash_key(e.key) & (m->capacity - 1);
    while (m->entries[i].state == 1)
        i = (i + 1) & (m->capacity - 1);
    if (!m->entries[i].state)
        ++m->occupied;
    e.state = 1;
    m->entries[i] = e;
    ++m->count;
    m->bytes += e.raw_size;
}
ss_status_t ssi_map_put(ssi_map_t *m, ssi_entry_t e) {
    ssi_entry_t *old = ssi_map_find(m, e.key);
    if (!old && m->count >= SSPS_MAX_SCENE_CELLS)
        return SS_MALFORMED;
    if (!old && m->count >= m->memory->options.max_scene_cells)
        return SS_RESOURCE_LIMIT;
    uint64_t bytes = m->bytes - (old ? old->raw_size : 0) + e.raw_size;
    if (bytes > m->memory->options.max_geometry_bytes)
        return SS_RESOURCE_LIMIT;
    if (old) {
        ssi_mesh_retain(e.mesh);
        ssi_mesh_release(old->mesh);
        e.state = 1;
        *old = e;
        m->bytes = bytes;
        return SS_OK;
    }
    if (!m->capacity || (m->occupied + 1) * 4 >= m->capacity * 3) {
        size_t capacity = m->capacity ? m->capacity : 16;
        if ((m->count + 1) * 4 >= capacity * 3)
            capacity *= 2;
        ssi_entry_t *entries = ssi_alloc(m->memory, capacity * sizeof(*entries));
        if (!entries)
            return m->memory->failure;
        memset(entries, 0, capacity * sizeof(*entries));
        ssi_map_t next = {.memory = m->memory, .entries = entries, .capacity = capacity};
        for (size_t i = 0; i < m->capacity; ++i)
            if (m->entries[i].state == 1)
                map_insert_unchecked(&next, m->entries[i]);
        ssi_free(m->entries);
        *m = next;
    }
    ssi_mesh_retain(e.mesh);
    map_insert_unchecked(m, e);
    return SS_OK;
}
ss_status_t ssi_map_remove(ssi_map_t *m, ss_cell_key_t key) {
    ssi_entry_t *e = ssi_map_find(m, key);
    if (!e)
        return SS_MALFORMED;
    e->state = 2;
    --m->count;
    m->bytes -= e->raw_size;
    ssi_mesh_release(e->mesh);
    e->mesh = NULL;
    return SS_OK;
}
void ssi_map_clear(ssi_map_t *m) {
    for (size_t i = 0; i < m->capacity; ++i)
        if (m->entries[i].state == 1)
            ssi_mesh_release(m->entries[i].mesh);
    ssi_free(m->entries);
    ssi_memory_t *memory = m->memory;
    memset(m, 0, sizeof(*m));
    m->memory = memory;
}
ss_status_t ssi_map_clone(const ssi_map_t *src, ssi_map_t *dst) {
    *dst = (ssi_map_t){.memory = src->memory};
    if (!src->capacity)
        return SS_OK;
    dst->entries = ssi_alloc(src->memory, src->capacity * sizeof(*dst->entries));
    if (!dst->entries)
        return src->memory->failure;
    memcpy(dst->entries, src->entries, src->capacity * sizeof(*dst->entries));
    dst->capacity = src->capacity;
    dst->count = src->count;
    dst->occupied = src->occupied;
    dst->bytes = src->bytes;
    for (size_t i = 0; i < dst->capacity; ++i)
        if (dst->entries[i].state == 1)
            ssi_mesh_retain(dst->entries[i].mesh);
    return SS_OK;
}
static int compare_entries(const void *a, const void *b) {
    const ssi_entry_t *x = *(ssi_entry_t *const *)a, *y = *(ssi_entry_t *const *)b;
    return ssi_key_compare(x->key, y->key);
}
ss_status_t ssi_map_sorted(const ssi_map_t *m, ssi_entry_t ***out) {
    *out = NULL;
    if (!m->count)
        return SS_OK;
    ssi_entry_t **p = ssi_alloc(m->memory, m->count * sizeof(*p));
    if (!p)
        return m->memory->failure;
    size_t n = 0;
    for (size_t i = 0; i < m->capacity; ++i)
        if (m->entries[i].state == 1)
            p[n++] = &m->entries[i];
    qsort(p, n, sizeof(*p), compare_entries);
    *out = p;
    return SS_OK;
}

ss_status_t ss_time_to_nanoseconds(uint64_t value, uint32_t scale, uint64_t *out) {
    if (!out || !scale)
        return SS_INVALID_ARGUMENT;
    uint64_t whole = value / scale, remainder = value % scale;
    if (whole > UINT64_MAX / SSPS_TIME_UNITS_PER_SECOND)
        return SS_OUT_OF_RANGE;
    uint64_t fraction = (remainder * SSPS_TIME_UNITS_PER_SECOND + scale / 2) / scale;
    uint64_t base = whole * SSPS_TIME_UNITS_PER_SECOND;
    if (fraction > UINT64_MAX - base)
        return SS_OUT_OF_RANGE;
    *out = base + fraction;
    return SS_OK;
}
