import Foundation
import SpatialSnapshotC

public enum ContainerKind: UInt32, Sendable { case heif = 1, quickTime = 2 }

public struct BindingDiagnostic: Sendable {
    public enum Domain: UInt32, Sendable {
        case heifMalformed = 1, quickTimeMalformed, bindingMalformed, bindingUnrepresentable
        case sspsMalformed, unsupportedBinding, unsupportedSSPS, resourceLimit
        case imageDecoderUnavailable, videoDecoderUnavailable, ioError
    }
    public let domain: Domain
    public let code: Int32
    public let fileOffset: UInt64
    public let sspsOffset: UInt64?
    public let entityID: UInt32
    public let packetType: UInt32
    public let message: String
    fileprivate init(_ value: ss_binding_diagnostic_t) {
        domain = Domain(rawValue: value.domain) ?? .unsupportedBinding
        code = value.status; fileOffset = value.file_offset
        sspsOffset = value.ssps_offset == UInt64.max ? nil : value.ssps_offset
        entityID = value.entity_id
        packetType = value.packet_type
        var value = value
        message = withUnsafePointer(to: &value.message) {
            $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) }
        }
    }
}

public struct BindingError: Error, Sendable, CustomStringConvertible {
    public let code: Int32
    public let diagnostics: [BindingDiagnostic]
    public var description: String {
        diagnostics.isEmpty ? String(cString: ss_status_string(code)) :
            diagnostics.map { "\($0.domain): \($0.message)" }.joined(separator: "; ")
    }
}

private final class DiagnosticCollector { var values: [BindingDiagnostic] = [] }
private func bindingOperation<R>(limits: ResourceLimits,
    _ operation: (UnsafePointer<ss_binding_options_t>) throws -> (Int32, R)) throws -> R {
    let collector = DiagnosticCollector()
    var options = ss_binding_options_t()
    ss_binding_options_init(&options)
    options.resources = limits.cValue
    options.diagnostic_context = Unmanaged.passUnretained(collector).toOpaque()
    options.diagnostic = { context, diagnostic in
        guard let context, let diagnostic else { return }
        Unmanaged<DiagnosticCollector>.fromOpaque(context).takeUnretainedValue()
            .values.append(BindingDiagnostic(diagnostic.pointee))
    }
    let (status, result) = try withExtendedLifetime(collector) { try operation(&options) }
    guard status == SS_OK else { throw BindingError(code: status, diagnostics: collector.values) }
    return result
}
private func outputData(_ output: OpaquePointer?) throws -> Data {
    guard let output else { throw SpatialSnapshotError(SS_INVALID_STATE) }
    defer { ss_binding_output_release(output) }
    var bytes: UnsafePointer<UInt8>?, count = 0
    let status = ss_binding_output_bytes(output, &bytes, &count)
    guard status == SS_OK, let bytes else { throw SpatialSnapshotError(status) }
    return Data(bytes: bytes, count: count)
}

/// A validated container and its spatial stream. RGB decoding is a separate operation.
/// The input bytes and callbacks are not retained after initialization. Serialize operations
/// involving this handle and its document, as with SpatialDocument.
public final class SpatialMedia {
    private let handle: OpaquePointer
    public let document: SpatialDocument
    public let kind: ContainerKind
    public let mediaID: UInt32
    public let metadataID: UInt32
    public let localKeyID: UInt32?
    public let width: UInt32
    public let height: UInt32
    public let sampleCount: UInt64
    public let checkpointCount: UInt64

    private init(taking handle: OpaquePointer) throws {
        var info = ss_container_info_t()
        info.struct_size = UInt32(MemoryLayout<ss_container_info_t>.size); info.abi_version = SS_ABI_VERSION
        var doc: OpaquePointer?
        guard ss_container_get_info(handle, &info) == SS_OK,
              ss_container_document(handle, &doc) == SS_OK, let doc,
              let kind = ContainerKind(rawValue: info.kind) else {
            ss_container_release(handle); throw SpatialSnapshotError(SS_INVALID_STATE)
        }
        self.handle = handle; self.document = SpatialDocument(taking: doc); self.kind = kind
        mediaID = info.media_id; metadataID = info.metadata_id
        localKeyID = kind == .quickTime ? info.local_key_id : nil
        width = info.raster_width; height = info.raster_height
        sampleCount = info.sample_count; checkpointCount = info.checkpoint_count
    }

    public convenience init(data: Data, kind: ContainerKind, limits: ResourceLimits = .init()) throws {
        let result: OpaquePointer? = try bindingOperation(limits: limits) { options in
            var result: OpaquePointer?
            let status = data.withUnsafeBytes {
                ss_container_open_memory(kind.rawValue, $0.baseAddress, $0.count, options, &result)
            }
            return (status, result)
        }
        guard let result else { throw SpatialSnapshotError(SS_INVALID_STATE) }
        try self.init(taking: result)
    }

    /// Uses random reads; opening a large movie does not load its RGB payload into memory.
    public convenience init(contentsOf url: URL, kind: ContainerKind, limits: ResourceLimits = .init()) throws {
        final class Input {
            let file: FileHandle
            let size: UInt64
            init(_ url: URL) throws {
                file = try FileHandle(forReadingFrom: url)
                do { size = try file.seekToEnd() }
                catch { try? file.close(); throw error }
            }
            deinit { try? file.close() }
        }
        let input: Input
        do { input = try Input(url) }
        catch { throw BindingError(code: SS_IO_ERROR, diagnostics: []) }
        var io = ss_io_t()
        io.struct_size = UInt32(MemoryLayout<ss_io_t>.size); io.abi_version = SS_ABI_VERSION
        io.context = Unmanaged.passUnretained(input).toOpaque()
        io.size = { context in
            guard let context else { return 0 }
            return Unmanaged<Input>.fromOpaque(context).takeUnretainedValue().size
        }
        io.read_at = { context, offset, destination, count in
            guard let context, let destination else { return SS_IO_ERROR }
            let input = Unmanaged<Input>.fromOpaque(context).takeUnretainedValue()
            do {
                try input.file.seek(toOffset: offset)
                guard let data = try input.file.read(upToCount: count), data.count == count else { return SS_IO_ERROR }
                data.withUnsafeBytes { raw in if let base = raw.baseAddress { destination.copyMemory(from: base, byteCount: count) } }
                return SS_OK
            } catch { return SS_IO_ERROR }
        }
        let result: OpaquePointer? = try withExtendedLifetime(input) {
            try bindingOperation(limits: limits) { options in
                var result: OpaquePointer?
                return (ss_container_open(kind.rawValue, &io, options, &result), result)
            }
        }
        guard let result else { throw SpatialSnapshotError(SS_INVALID_STATE) }
        try self.init(taking: result)
    }
    deinit { ss_container_release(handle) }

    public var sspsData: Data {
        var bytes: UnsafePointer<UInt8>?, count = 0
        precondition(ss_container_ssps_bytes(handle, &bytes, &count) == SS_OK)
        return Data(bytes: bytes!, count: count)
    }
    public struct Sample: Sendable {
        public let timestampNanoseconds: UInt64, durationNanoseconds: UInt64
        public let mediaDecodeIndex: UInt64
        public let mediaByteRange: Range<UInt64>?
        public let bundleByteRange: Range<UInt64>
        public let sspsOffset: UInt64
    }
    public func sample(at index: UInt64) throws -> Sample {
        var s = ss_container_sample_t()
        s.struct_size = UInt32(MemoryLayout<ss_container_sample_t>.size); s.abi_version = SS_ABI_VERSION
        let status = ss_container_get_sample(handle, index, &s)
        guard status == SS_OK else { throw SpatialSnapshotError(status) }
        return Sample(timestampNanoseconds: s.timestamp_ns, durationNanoseconds: s.duration_ns,
            mediaDecodeIndex: s.media_decode_index,
            mediaByteRange: s.media_size > 0 ? s.media_offset ..< s.media_offset + s.media_size : nil,
            bundleByteRange: s.bundle_offset ..< s.bundle_offset + s.bundle_size, sspsOffset: s.ssps_offset)
    }
    public struct Checkpoint: Sendable {
        public let timestampNanoseconds: UInt64, sampleIndex: UInt64, sspsOffset: UInt64
    }
    public func checkpoint(at index: UInt64) throws -> Checkpoint {
        var cp = ss_container_checkpoint_t()
        cp.struct_size = UInt32(MemoryLayout<ss_container_checkpoint_t>.size); cp.abi_version = SS_ABI_VERSION
        let status = ss_container_get_checkpoint(handle, index, &cp)
        guard status == SS_OK else { throw SpatialSnapshotError(status) }
        return Checkpoint(timestampNanoseconds: cp.timestamp_ns, sampleIndex: cp.sample_index, sspsOffset: cp.ssps_offset)
    }

    /// Removes the complete binding and its relationships. HEIF storage is rebuilt so no
    /// removed SSPS payload remains. QuickTime may retain unreachable old mdat storage.
    public static func removingSpatialMetadata(from host: Data, kind: ContainerKind,
                                               limits: ResourceLimits = .init()) throws -> Data {
        let output: OpaquePointer? = try bindingOperation(limits: limits) { options in
            var output: OpaquePointer?
            let status = host.withUnsafeBytes { ss_container_strip_memory(kind.rawValue, $0.baseAddress, $0.count, options, &output) }
            return (status, output)
        }
        return try outputData(output)
    }

    /// Trims and rebases spatial geometry, camera poses, stream identity, and video timestamps
    /// together. Inter-frame codecs needing a decoder fail with videoDecoderUnavailable.
    /// The portable path accepts a movie containing one video track and its SSPS track.
    public static func trimmingQuickTime(_ host: Data, from start: UInt64, to end: UInt64,
                                        limits: ResourceLimits = .init()) throws -> Data {
        var writer = writerOptions(kind: .video, gravity: SIMD3(0, 1, 0), limits: limits)
        let output: OpaquePointer? = try bindingOperation(limits: limits) { options in
            var output: OpaquePointer?
            let status = host.withUnsafeBytes {
                ss_quicktime_trim_memory($0.baseAddress, $0.count, start, end, &writer, options, &output)
            }
            return (status, output)
        }
        return try outputData(output)
    }
}

extension SpatialDocument {
    /// The host must depict this same capture with unchanged pixel geometry. QuickTime
    /// video PTS/durations must already equal the camera timeline at exactly 1 GHz.
    public func binding(to host: Data, kind: ContainerKind, videoTrackID: UInt32 = 0,
                        limits: ResourceLimits = .init()) throws -> Data {
        let output: OpaquePointer? = try bindingOperation(limits: limits) { options in
            var output: OpaquePointer?
            let status = host.withUnsafeBytes { raw in
                if kind == .heif { return ss_heif_bind_memory(raw.baseAddress, raw.count, handle, options, &output) }
                return ss_quicktime_bind_memory(raw.baseAddress, raw.count, videoTrackID, handle, options, &output)
            }
            return (status, output)
        }
        return try outputData(output)
    }

    public struct EncodedVideoSample: Sendable {
        public var data: Data
        public var presentationTimestampNanoseconds: UInt64
        public var decodeDurationNanoseconds: UInt32
        public var isSync: Bool
        public init(data: Data, presentationTimestampNanoseconds: UInt64,
                    decodeDurationNanoseconds: UInt32, isSync: Bool) {
            self.data = data; self.presentationTimestampNanoseconds = presentationTimestampNanoseconds
            self.decodeDurationNanoseconds = decodeDurationNanoseconds; self.isSync = isSync
        }
    }
    /// Muxes encoded samples in decode order, retaining their exact presentation times.
    /// visualSampleEntry includes its complete 8-byte atom header and codec configuration.
    public func quickTime(visualSampleEntry: Data, samples: [EncodedVideoSample],
                          limits: ResourceLimits = .init()) throws -> Data {
        var buffers: [UnsafeMutablePointer<UInt8>] = []
        defer { for p in buffers { p.deallocate() } }
        var values: [ss_encoded_video_sample_t] = []
        for sample in samples {
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: max(1, sample.data.count)); buffers.append(p)
            sample.data.copyBytes(to: p, count: sample.data.count)
            var value = ss_encoded_video_sample_t()
            value.struct_size = UInt32(MemoryLayout<ss_encoded_video_sample_t>.size); value.abi_version = SS_ABI_VERSION
            value.bytes = UnsafePointer(p); value.size = sample.data.count
            value.presentation_timestamp_ns = sample.presentationTimestampNanoseconds
            value.decode_duration_ns = sample.decodeDurationNanoseconds; value.is_sync = sample.isSync ? 1 : 0
            values.append(value)
        }
        let output: OpaquePointer? = try bindingOperation(limits: limits) { options in
            var output: OpaquePointer?
            let status = visualSampleEntry.withUnsafeBytes { entry in
                values.withUnsafeBufferPointer { samples in
                    ss_quicktime_mux(entry.baseAddress, entry.count, samples.baseAddress, samples.count, handle, options, &output)
                }
            }
            return (status, output)
        }
        return try outputData(output)
    }
}
