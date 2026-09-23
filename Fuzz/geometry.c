#include "../Sources/SpatialSnapshotC/internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ss_open_options_t options;
    ss_open_options_init(&options);
    options.max_memory_bytes = 16 * 1024 * 1024;
    ssi_memory_t *memory = NULL;
    if (ssi_memory_create(&options, &memory) != SS_OK)
        return 0;
    ssi_mesh_t *mesh = NULL;
    if (ssi_mesh_decode(memory, data, size, &mesh) == SS_OK) {
        ss_mesh_view_t view;
        ssi_mesh_view(mesh, &view);
        ss_vec3_t point;
        for (uint32_t i = 0; i < view.vertex_count; ++i)
            ss_mesh_vertex(&view, i, &point);
    }
    ssi_mesh_release(mesh);
    ssi_memory_release(memory);
    return 0;
}
