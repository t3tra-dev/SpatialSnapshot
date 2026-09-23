#include <spatialsnapshot/spatialsnapshot.h>
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ss_binding_options_t options;
    ss_binding_options_init(&options);
    options.resources.max_memory_bytes = 32 * 1024 * 1024;
    options.resources.max_packets = 10000;
    options.resources.max_scene_cells = 1024;
    for (ss_container_kind_t kind = SS_CONTAINER_HEIF; kind <= SS_CONTAINER_QUICKTIME; ++kind) {
        ss_container_t *container = NULL;
        if (ss_container_open_memory(kind, data, size, &options, &container) == SS_OK) {
            ss_document_t *doc = NULL;
            ss_container_document(container, &doc);
            ss_binding_output_t *output = NULL;
            if (kind == SS_CONTAINER_HEIF)
                ss_heif_bind_memory(data, size, doc, &options, &output);
            else
                ss_quicktime_bind_memory(data, size, 0, doc, &options, &output);
            ss_binding_output_release(output);
            output = NULL;
            ss_container_strip_memory(kind, data, size, &options, &output);
            ss_binding_output_release(output);
            ss_document_release(doc);
            ss_container_release(container);
        }
    }
    return 0;
}
