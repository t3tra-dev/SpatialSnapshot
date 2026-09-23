#include <spatialsnapshot/spatialsnapshot.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct diagnostics {
    ss_binding_diagnostic_t *items;
    size_t count;
} diagnostics_t;
static void diagnostic(void *context, const ss_binding_diagnostic_t *value) {
    diagnostics_t *d = context;
    void *p = realloc(d->items, (d->count + 1) * sizeof(*d->items));
    if (!p)
        return;
    d->items = p;
    d->items[d->count++] = *value;
}

typedef struct file_input {
    FILE *file;
    uint64_t size;
} file_input_t;
static uint64_t input_size(void *context) {
    return ((file_input_t *)context)->size;
}
static ss_status_t read_at(void *context, uint64_t offset, void *destination, size_t size) {
    file_input_t *input = context;
    if (offset > LONG_MAX || fseek(input->file, (long)offset, SEEK_SET) != 0)
        return SS_IO_ERROR;
    return fread(destination, 1, size, input->file) == size ? SS_OK : SS_IO_ERROR;
}
static uint32_t probe_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static ss_container_kind_t detect_kind(file_input_t *input) {
    uint8_t probe[16];
    if (input->size < 8 || read_at(input, 0, probe, 8) != SS_OK ||
        !memcmp(probe, "SSPS\r\n\x1a\n", 8))
        return 0;
    /* Read only top-level atom headers and the ftyp major brand, including extended sizes. */
    uint64_t off = 0;
    for (uint32_t count = 0; count < 2097152 && off <= input->size && input->size - off >= 8;
         ++count) {
        if (read_at(input, off, probe, 8) != SS_OK)
            break;
        uint64_t size = probe_u32(probe), header = 8;
        if (size == 1) {
            if (input->size - off < 16 || read_at(input, off + 8, probe + 8, 8) != SS_OK)
                break;
            size = (uint64_t)probe_u32(probe + 8) << 32 | probe_u32(probe + 12);
            header = 16;
        } else if (!size)
            size = input->size - off;
        if (size < header || size > input->size - off)
            break;
        if (!memcmp(probe + 4, "ftyp", 4)) {
            if (size >= header + 8 && read_at(input, off + header, probe, 4) == SS_OK &&
                !memcmp(probe, "qt  ", 4))
                return SS_CONTAINER_QUICKTIME;
            break;
        }
        off += size;
    }
    return SS_CONTAINER_HEIF;
}
static void json_string(const char *s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c < 32)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}
int main(int argc, char **argv) {
    int json = 0, files = 0, result = 0;
    ss_container_kind_t forced_kind = 0;
    int dump = strstr(argv[0], "ssdump") != NULL, validate = strstr(argv[0], "ssvalidate") != NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--json")) {
            json = 1;
            continue;
        }
        if (!strcmp(argv[i], "--heif")) {
            forced_kind = SS_CONTAINER_HEIF;
            continue;
        }
        if (!strcmp(argv[i], "--quicktime")) {
            forced_kind = SS_CONTAINER_QUICKTIME;
            continue;
        }
        if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--json] [--heif|--quicktime] file.ssps|file.heic|file.mov [...]\n",
                   argv[0]);
            return 0;
        }
        ++files;
        file_input_t input = {0};
        ss_document_t *document = NULL;
        ss_container_t *container = NULL;
        diagnostics_t diagnostics = {0};
        ss_error_t error = SS_INIT(ss_error_t);
        ss_status_t status = SS_IO_ERROR;
        input.file = fopen(argv[i], "rb");
        if (input.file) {
            if (fseek(input.file, 0, SEEK_END) == 0) {
                long length = ftell(input.file);
                if (length >= 0) {
                    input.size = (uint64_t)length;
                    ss_io_t io = {.struct_size = sizeof(io),
                                  .abi_version = SS_ABI_VERSION,
                                  .context = &input,
                                  .read_at = read_at,
                                  .size = input_size};
                    ss_container_kind_t kind = forced_kind ? forced_kind : detect_kind(&input);
                    if (kind) {
                        ss_binding_options_t options;
                        ss_binding_options_init(&options);
                        options.diagnostic = diagnostic;
                        options.diagnostic_context = &diagnostics;
                        status = ss_container_open(kind, &io, &options, &container);
                        if (status == SS_OK)
                            status = ss_container_document(container, &document);
                    } else
                        status = ss_document_open(&io, NULL, &document, &error);
                }
            }
            fclose(input.file);
        }
        ss_document_info_t info = SS_INIT(ss_document_info_t);
        if (document)
            (void)ss_document_get_info(document, &info);
        if (json) {
            printf("{\"file\":");
            json_string(argv[i]);
            printf(",\"status\":");
            json_string(ss_status_string(status));
            if (status != SS_OK) {
                printf(",\"offset\":%" PRIu64 ",\"sequence\":%" PRIu64 ",\"message\":",
                       error.byte_offset, error.sequence_number);
                json_string(error.message);
            } else
                printf(",\"kind\":%u,\"duration_ns\":%" PRIu64 ",\"packets\":%" PRIu64
                       ",\"cameras\":%" PRIu64 ",\"checkpoints\":%" PRIu64,
                       info.kind, info.duration_ns, info.packet_count, info.camera_count,
                       info.checkpoint_count);
            if (diagnostics.count) {
                printf(",\"diagnostics\":[");
                for (size_t n = 0; n < diagnostics.count; ++n) {
                    const ss_binding_diagnostic_t *d = &diagnostics.items[n];
                    if (n)
                        putchar(',');
                    printf("{\"domain\":");
                    json_string(ss_binding_domain_string(d->domain));
                    printf(",\"status\":");
                    json_string(ss_status_string(d->status));
                    printf(",\"offset\":%" PRIu64
                           ",\"entity_id\":%u,\"packet_type\":%u,\"ssps_offset\":",
                           d->file_offset, d->entity_id, d->packet_type);
                    if (d->ssps_offset == UINT64_MAX)
                        printf("null");
                    else
                        printf("%" PRIu64, d->ssps_offset);
                    printf(",\"message\":");
                    json_string(d->message);
                    putchar('}');
                }
                putchar(']');
            }
            puts("}");
        } else if (status != SS_OK)
            fprintf(stderr, "%s: %s at byte %" PRIu64 " (sequence %" PRIu64 "): %s\n", argv[i],
                    ss_status_string(status), error.byte_offset, error.sequence_number,
                    error.message);
        else if (validate)
            printf("%s: OK (SSPS 1.%u)\n", argv[i], info.version_minor);
        else {
            printf("%s: SSPS 1.%u %s, duration %" PRIu64 " ns\n", argv[i], info.version_minor,
                   info.kind == SS_STILL ? "STILL" : "VIDEO", info.duration_ns);
            printf("  packets=%" PRIu64 " cameras=%" PRIu64 " checkpoints=%" PRIu64
                   " gravity=(%.9g, %.9g, %.9g)\n",
                   info.packet_count, info.camera_count, info.checkpoint_count, info.gravity.x,
                   info.gravity.y, info.gravity.z);
            if (dump)
                for (uint64_t n = 0; n < info.packet_count; ++n) {
                    ss_packet_info_t packet = SS_INIT(ss_packet_info_t);
                    (void)ss_document_packet(document, n, &packet);
                    printf("  seq=%" PRIu64 " t=%" PRIu64 " type=0x%04x flags=%u offset=%" PRIu64
                           " stored=%u raw=%u\n",
                           packet.sequence_number, packet.timestamp_ns, packet.type, packet.flags,
                           packet.byte_offset, packet.stored_payload_size, packet.raw_payload_size);
                }
        }
        if (status != SS_OK)
            result = 1;
        if (!json)
            for (size_t n = 0; n < diagnostics.count; ++n)
                fprintf(stderr, "  %s: %s\n", ss_binding_domain_string(diagnostics.items[n].domain),
                        diagnostics.items[n].message);
        free(diagnostics.items);
        ss_container_release(container);
        ss_document_release(document);
    }
    if (!files) {
        fprintf(stderr, "Usage: %s [--json] stream.ssps [...stream.ssps]\n", argv[0]);
        return 2;
    }
    return result;
}
