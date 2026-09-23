#include <spatialsnapshot/spatialsnapshot.h>
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ss_open_options_t options;
    ss_open_options_init(&options);
    options.max_memory_bytes = 32 * 1024 * 1024;
    options.max_packets = 10000;
    options.max_scene_cells = 1024;
    ss_document_t *document = NULL;
    if (ss_document_open_memory(data, size, &options, &document, NULL) == SS_OK) {
        ss_document_info_t info = SS_INIT(ss_document_info_t);
        ss_document_get_info(document, &info);
        ss_scene_cursor_t *cursor = NULL;
        if (ss_scene_cursor_create(document, &cursor) == SS_OK) {
            ss_scene_cursor_seek(cursor, info.duration_ns / 2);
            for (uint32_t i = 0; i < ss_scene_cursor_chunk_count(cursor); ++i) {
                ss_mesh_view_t mesh = SS_INIT(ss_mesh_view_t);
                ss_scene_cursor_chunk(cursor, i, &mesh);
            }
            ss_depth_view_t depth = SS_INIT(ss_depth_view_t);
            ss_scene_cursor_depth(cursor, &depth);
            ss_hit_t hit = SS_INIT(ss_hit_t);
            ss_vec2_t pixel = {0, 0};
            ss_scene_raycast_pixel(cursor, &pixel, &hit);
            ss_scene_cursor_release(cursor);
        }
        ss_document_release(document);
    }
    return 0;
}
