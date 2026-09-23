#ifndef SPATIALSNAPSHOT_BINDINGS_H
#define SPATIALSNAPSHOT_BINDINGS_H
#include "spatialsnapshot.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SS_HEIF_BINDING_VERSION_MAJOR 1
#define SS_HEIF_BINDING_VERSION_MINOR 0
#define SS_QT_BINDING_VERSION_MAJOR 1
#define SS_QT_BINDING_VERSION_MINOR 0
#define SS_HEIF_CONTENT_TYPE "application/vnd.spatialsnapshot.ssps"
#define SS_QT_METADATA_KEY "org.spatialsnapshot.ssps.packet-bundle"
#define SS_QT_METADATA_DATATYPE "com.apple.metadata.datatype.raw-data"
#define SS_HEIF_MAX_FILE_BYTES UINT64_C(34359738368)
#define SS_HEIF_MAX_SSPS_BYTES UINT64_C(2147483648)
#define SS_QT_MAX_FILE_BYTES UINT64_C(274877906944)
#define SS_QT_MAX_RECONSTRUCTED_SSPS_BYTES UINT64_C(137438953472)
#define SS_QT_MAX_SAMPLE_DURATION_NS UINT32_MAX

typedef uint32_t ss_container_kind_t;
#define SS_CONTAINER_HEIF ((ss_container_kind_t)1)
#define SS_CONTAINER_QUICKTIME ((ss_container_kind_t)2)
typedef uint32_t ss_binding_domain_t;
#define SS_DOMAIN_HEIF_MALFORMED ((ss_binding_domain_t)1)
#define SS_DOMAIN_QUICKTIME_MALFORMED ((ss_binding_domain_t)2)
#define SS_DOMAIN_BINDING_MALFORMED ((ss_binding_domain_t)3)
#define SS_DOMAIN_BINDING_UNREPRESENTABLE ((ss_binding_domain_t)4)
#define SS_DOMAIN_SSPS_MALFORMED ((ss_binding_domain_t)5)
#define SS_DOMAIN_UNSUPPORTED_BINDING ((ss_binding_domain_t)6)
#define SS_DOMAIN_UNSUPPORTED_SSPS ((ss_binding_domain_t)7)
#define SS_DOMAIN_RESOURCE_LIMIT ((ss_binding_domain_t)8)
#define SS_DOMAIN_IMAGE_DECODER_UNAVAILABLE ((ss_binding_domain_t)9)
#define SS_DOMAIN_VIDEO_DECODER_UNAVAILABLE ((ss_binding_domain_t)10)
#define SS_DOMAIN_IO_ERROR ((ss_binding_domain_t)11)

typedef struct ss_binding_diagnostic {
    uint32_t struct_size, abi_version;
    ss_binding_domain_t domain;
    ss_status_t status;
    uint64_t file_offset;
    uint64_t ssps_offset; /* UINT64_MAX when no SSPS offset applies */
    uint32_t entity_id;   /* item ID or track ID; zero when not applicable */
    uint32_t packet_type;
    char message[160];
} ss_binding_diagnostic_t;
typedef void (*ss_binding_diagnostic_fn)(void *context, const ss_binding_diagnostic_t *diagnostic);
typedef struct ss_binding_options {
    uint32_t struct_size, abi_version;
    ss_open_options_t resources;
    ss_binding_diagnostic_fn diagnostic;
    void *diagnostic_context;
} ss_binding_options_t;

typedef struct ss_container ss_container_t;
typedef struct ss_binding_output ss_binding_output_t;
typedef struct ss_container_info {
    uint32_t struct_size, abi_version;
    ss_container_kind_t kind;
    uint32_t media_id, metadata_id, local_key_id;
    uint32_t raster_width, raster_height;
    uint64_t duration_ns, sample_count, checkpoint_count;
} ss_container_info_t;
typedef struct ss_container_sample {
    uint32_t struct_size, abi_version;
    uint64_t timestamp_ns, duration_ns;
    uint64_t media_decode_index, media_offset, media_size;
    uint64_t bundle_offset, bundle_size, ssps_offset;
} ss_container_sample_t;
typedef struct ss_container_checkpoint {
    uint32_t struct_size, abi_version;
    uint64_t timestamp_ns, sample_index, ssps_offset;
} ss_container_checkpoint_t;

SS_API void ss_binding_options_init(ss_binding_options_t *options);
SS_API const char *ss_binding_domain_string(ss_binding_domain_t domain);
/* The I/O callbacks and bytes need live only until open returns. RGB bytes are not decoded or
 * retained. No handle is published on any validation failure. Independent diagnostics are
 * delivered synchronously; a callback must not reenter this operation. */
SS_API ss_status_t ss_container_open(ss_container_kind_t kind, const ss_io_t *io,
                                     const ss_binding_options_t *options, ss_container_t **out);
SS_API ss_status_t ss_container_open_memory(ss_container_kind_t kind, const void *bytes,
                                            size_t size, const ss_binding_options_t *options,
                                            ss_container_t **out);
SS_API void ss_container_release(ss_container_t *container);
SS_API ss_status_t ss_container_get_info(const ss_container_t *container,
                                         ss_container_info_t *info);
/* Returns a retained document, independently releasable. */
SS_API ss_status_t ss_container_document(const ss_container_t *container, ss_document_t **out);
SS_API ss_status_t ss_container_ssps_bytes(const ss_container_t *container, const uint8_t **bytes,
                                           size_t *size);
SS_API ss_status_t ss_container_get_sample(const ss_container_t *container, uint64_t index,
                                           ss_container_sample_t *sample);
SS_API ss_status_t ss_container_get_checkpoint(const ss_container_t *container, uint64_t index,
                                               ss_container_checkpoint_t *checkpoint);

/* Bind an already encoded host to the same capture's validated SSPS. Raster geometry must
 * already be identity; QuickTime timelines must already use exact 1 GHz integer timestamps.
 * A zero video_track_id selects the sole video track. Existing SSPS is replaced atomically.
 * Byte identity of the supplied SSPS is preserved, including unknown noncritical packets. */
/* On an unbound host only, the HEIF writer removes exact irot=0 associations
 * without changing pixels. Bound inputs and all nonidentity transforms remain
 * subject to strict binding validation. */
SS_API ss_status_t ss_heif_bind_memory(const void *host, size_t host_size,
                                       const ss_document_t *document,
                                       const ss_binding_options_t *options,
                                       ss_binding_output_t **out);
SS_API ss_status_t ss_quicktime_bind_memory(const void *host, size_t host_size,
                                            uint32_t video_track_id, const ss_document_t *document,
                                            const ss_binding_options_t *options,
                                            ss_binding_output_t **out);
SS_API ss_status_t ss_container_strip_memory(ss_container_kind_t kind, const void *host,
                                             size_t host_size, const ss_binding_options_t *options,
                                             ss_binding_output_t **out);
SS_API ss_status_t ss_binding_output_bytes(const ss_binding_output_t *output, const uint8_t **bytes,
                                           size_t *size);
SS_API void ss_binding_output_release(ss_binding_output_t *output);

/* Encoded video muxing. Sample entry is a complete visual sample-entry atom. Samples are
 * supplied in decode order; PTS may be reordered. Duration is the stts decode duration.
 * Container validation is independent of codec bitstream validation/decoding. */
typedef struct ss_encoded_video_sample {
    uint32_t struct_size, abi_version;
    const uint8_t *bytes;
    size_t size;
    uint64_t presentation_timestamp_ns;
    uint32_t decode_duration_ns;
    uint32_t is_sync;
} ss_encoded_video_sample_t;
SS_API ss_status_t ss_quicktime_mux(const void *visual_sample_entry, size_t entry_size,
                                    const ss_encoded_video_sample_t *samples, size_t sample_count,
                                    const ss_document_t *document,
                                    const ss_binding_options_t *options, ss_binding_output_t **out);

/* Canonical SSPS trim/rebase plus video sample remux. The portable fast path requires a
 * single video track and independently decodable retained frames (all sync). Inter-frame
 * decoding/re-encoding returns VIDEO_DECODER_UNAVAILABLE; additional tracks/opaque sample
 * dependency groups return UNSUPPORTED_BINDING. No partial output is returned. A caller
 * with a codec backend can trim the video itself, call ss_document_trim, then bind/mux. */
SS_API ss_status_t ss_quicktime_trim_memory(const void *host, size_t host_size, uint64_t start_ns,
                                            uint64_t end_ns,
                                            const ss_writer_options_t *writer_options,
                                            const ss_binding_options_t *options,
                                            ss_binding_output_t **out);

#ifdef __cplusplus
}
#endif
#endif
