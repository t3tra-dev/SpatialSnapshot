#include "../Sources/SpatialSnapshotC/internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4)
        return 0;
    size_t raw = ssi_u32(data);
    if (raw > 1024 * 1024)
        return 0;
    uint8_t *out = malloc(raw ? raw : 1);
    if (out) {
        ssi_lz4_decode(data + 4, size - 4, out, raw);
        free(out);
    }
    return 0;
}
