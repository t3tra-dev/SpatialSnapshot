import Foundation
import SpatialSnapshotC

public struct SpatialSnapshotError: Error, CustomStringConvertible, Sendable {
    public let code: Int32
    public let message: String
    public let byteOffset: UInt64?
    public let sequenceNumber: UInt64?
    public var description: String {
        if let byteOffset { return "\(message) (byte \(byteOffset))" }
        return message
    }
    init(_ code: Int32, detail: ss_error_t? = nil) {
        self.code = code
        if var detail {
            message = withUnsafePointer(to: &detail.message) {
                $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) }
            }
            byteOffset = detail.byte_offset
            sequenceNumber = detail.sequence_number
        } else {
            message = String(cString: ss_status_string(code))
            byteOffset = nil
            sequenceNumber = nil
        }
    }
}

private func check(_ status: Int32) throws {
    if status != SS_OK { throw SpatialSnapshotError(status) }
}

public enum StreamKind: UInt32, Sendable { case still = 1, video = 2 }
public enum SurfaceClassification: UInt8, Sendable {
    case unknown, wall, floor, ceiling, table, seat, window, door, other
}

public struct ResourceLimits: Sendable {
    public var memoryBytes: UInt64 = 0
    public var geometryBytes: UInt64 = 0
    public var packets: UInt64 = 0
    public var partitionWork: UInt64 = 0
    public var sceneCells: UInt32 = 0
    public var packetRawBytes: UInt32 = 0
    public init() {}
    var cValue: ss_open_options_t {
        var value = ss_open_options_t()
        ss_open_options_init(&value)
        value.max_memory_bytes = memoryBytes
        value.max_geometry_bytes = geometryBytes
        value.max_packets = packets
        value.max_partition_work = partitionWork
        value.max_scene_cells = sceneCells
        value.max_packet_raw_bytes = packetRawBytes
        return value
    }
}

public struct Camera: Sendable {
    public var timestampNanoseconds: UInt64
    public var width: UInt32
    public var height: UInt32
    public var fx: Float
    public var fy: Float
    public var cx: Float
    public var cy: Float
    public var translation: SIMD3<Float>
    /// Hamilton (x, y, z, w), camera to scene.
    public var quaternion: SIMD4<Float>

    public init(timestampNanoseconds: UInt64 = 0, width: UInt32, height: UInt32,
                fx: Float, fy: Float, cx: Float, cy: Float,
                translation: SIMD3<Float> = .zero,
                quaternion: SIMD4<Float> = SIMD4(0, 0, 0, 1)) {
        self.timestampNanoseconds = timestampNanoseconds
        self.width = width; self.height = height
        self.fx = fx; self.fy = fy; self.cx = cx; self.cy = cy
        self.translation = translation; self.quaternion = quaternion
    }

    fileprivate init(_ c: ss_camera_t) {
        self.init(timestampNanoseconds: c.timestamp_ns, width: c.raster_width, height: c.raster_height,
                  fx: c.fx, fy: c.fy, cx: c.cx, cy: c.cy,
                  translation: SIMD3(c.tx, c.ty, c.tz), quaternion: SIMD4(c.qx, c.qy, c.qz, c.qw))
    }
    fileprivate var cValue: ss_camera_t {
        var c = ss_camera_t()
        ss_camera_init(&c)
        c.timestamp_ns = timestampNanoseconds
        c.raster_width = width; c.raster_height = height
        c.fx = fx; c.fy = fy; c.cx = cx; c.cy = cy
        c.tx = translation.x; c.ty = translation.y; c.tz = translation.z
        c.qx = quaternion.x; c.qy = quaternion.y; c.qz = quaternion.z; c.qw = quaternion.w
        return c
    }
    public func project(_ point: SIMD3<Double>) throws -> SIMD2<Double> {
        var c = cValue, p = ss_vec3_t(point), pixel = ss_vec2_t()
        try check(ss_camera_project(&c, &p, &pixel))
        return SIMD2(pixel.x, pixel.y)
    }
    /// Depth is camera-local +Z, in meters; it is not ray length.
    public func unproject(_ pixel: SIMD2<Double>, depthMeters: Double) throws -> SIMD3<Double> {
        var c = cValue, p = ss_vec2_t(x: pixel.x, y: pixel.y), point = ss_vec3_t()
        try check(ss_camera_unproject(&c, &p, depthMeters, &point))
        return point.vector
    }
}

extension ss_vec3_t {
    fileprivate init(_ v: SIMD3<Double>) { self.init(x: v.x, y: v.y, z: v.z) }
    fileprivate var vector: SIMD3<Double> { SIMD3(x, y, z) }
}

public struct SceneTriangle: Sendable {
    public var a: SIMD3<Double>
    public var b: SIMD3<Double>
    public var c: SIMD3<Double>
    public var classification: SurfaceClassification
    public init(_ a: SIMD3<Double>, _ b: SIMD3<Double>, _ c: SIMD3<Double>,
                classification: SurfaceClassification = .unknown) {
        self.a = a; self.b = b; self.c = c; self.classification = classification
    }
}

public struct MeshChunk: Sendable {
    public let cell: SIMD3<Int32>
    public let quantizedVertices: [SIMD3<UInt16>]
    public let triangles: [SIMD3<UInt16>]
    public let classifications: [SurfaceClassification]

    public init(cell: SIMD3<Int32>, quantizedVertices: [SIMD3<UInt16>],
                triangles: [SIMD3<UInt16>], classifications: [SurfaceClassification]) throws {
        guard (3...65_535).contains(quantizedVertices.count), (1...262_144).contains(triangles.count),
              triangles.count == classifications.count,
              triangles.allSatisfy({ Int($0.x) < quantizedVertices.count && Int($0.y) < quantizedVertices.count && Int($0.z) < quantizedVertices.count }) else {
            throw SpatialSnapshotError(SS_INVALID_ARGUMENT)
        }
        self.cell = cell; self.quantizedVertices = quantizedVertices
        self.triangles = triangles; self.classifications = classifications
    }
    fileprivate init(_ view: ss_mesh_view_t) {
        cell = SIMD3(view.cell.x, view.cell.y, view.cell.z)
        quantizedVertices = (0..<Int(view.vertex_count)).map {
            SIMD3(view.quantized_xyz[3 * $0], view.quantized_xyz[3 * $0 + 1], view.quantized_xyz[3 * $0 + 2])
        }
        triangles = (0..<Int(view.triangle_count)).map {
            SIMD3(view.triangle_indices[3 * $0], view.triangle_indices[3 * $0 + 1], view.triangle_indices[3 * $0 + 2])
        }
        classifications = (0..<Int(view.triangle_count)).map {
            SurfaceClassification(rawValue: view.classifications[$0]) ?? .unknown
        }
    }
    public func positions() throws -> [SIMD3<Double>] {
        let pinned = PinnedMesh(self)
        var view = pinned.view
        return try withExtendedLifetime(pinned) {
            try quantizedVertices.indices.map { i in
                var point = ss_vec3_t()
                try check(ss_mesh_vertex(&view, UInt32(i), &point))
                return point.vector
            }
        }
    }
    public static func partition(_ triangles: [SceneTriangle], limits: ResourceLimits = .init()) throws -> [MeshChunk] {
        let input: [ss_triangle_t] = triangles.map { triangle in
            var c = ss_triangle_t()
            c.struct_size = UInt32(MemoryLayout<ss_triangle_t>.size); c.abi_version = SS_ABI_VERSION
            c.vertices = (ss_vec3_t(triangle.a), ss_vec3_t(triangle.b), ss_vec3_t(triangle.c))
            c.classification = UInt32(triangle.classification.rawValue)
            return c
        }
        var options = limits.cValue, set: OpaquePointer?
        try input.withUnsafeBufferPointer { buffer in
            try check(ss_mesh_partition(buffer.baseAddress, buffer.count, &options, &set))
        }
        defer { ss_mesh_set_release(set) }
        return try (0..<ss_mesh_set_count(set)).map { i in
            var mesh = emptyMeshView()
            try check(ss_mesh_set_chunk(set, i, &mesh))
            return MeshChunk(mesh)
        }
    }
}

private func emptyMeshView() -> ss_mesh_view_t {
    var view = ss_mesh_view_t()
    view.struct_size = UInt32(MemoryLayout<ss_mesh_view_t>.size); view.abi_version = SS_ABI_VERSION
    return view
}
private final class PinnedBuffer<Element> {
    let pointer: UnsafeMutablePointer<Element>
    let count: Int
    init(_ values: [Element]) {
        count = values.count
        pointer = .allocate(capacity: max(1, count))
        values.withUnsafeBufferPointer { input in
            if let base = input.baseAddress, !input.isEmpty { pointer.initialize(from: base, count: input.count) }
        }
    }
    deinit { pointer.deinitialize(count: count); pointer.deallocate() }
}
private final class PinnedMesh {
    let xyz: PinnedBuffer<UInt16>
    let indices: PinnedBuffer<UInt16>
    let classes: PinnedBuffer<UInt8>
    let view: ss_mesh_view_t
    init(_ mesh: MeshChunk) {
        xyz = PinnedBuffer(mesh.quantizedVertices.flatMap { [$0.x, $0.y, $0.z] })
        indices = PinnedBuffer(mesh.triangles.flatMap { [$0.x, $0.y, $0.z] })
        classes = PinnedBuffer(mesh.classifications.map(\.rawValue))
        var view = emptyMeshView()
        view.cell = ss_cell_key_t(x: mesh.cell.x, y: mesh.cell.y, z: mesh.cell.z)
        view.vertex_count = UInt32(mesh.quantizedVertices.count); view.triangle_count = UInt32(mesh.triangles.count)
        view.quantized_xyz = UnsafePointer(xyz.pointer)
        view.triangle_indices = UnsafePointer(indices.pointer)
        view.classifications = UnsafePointer(classes.pointer)
        self.view = view
    }
}

public struct DepthSample: Sendable {
    public let timestampNanoseconds: UInt64
    public let width: UInt32
    public let height: UInt32
    public let fx: Float
    public let fy: Float
    public let cx: Float
    public let cy: Float
    public let millimeters: [UInt16]
    /// One unpacked confidence value per pixel: invalid=0, low=1, medium=2, high=3.
    public let confidence: [UInt8]
    public init(timestampNanoseconds: UInt64, width: UInt32, height: UInt32,
                fx: Float, fy: Float, cx: Float, cy: Float, millimeters: [UInt16], confidence: [UInt8]) throws {
        let count = UInt64(width) * UInt64(height)
        guard width > 0, height > 0, width <= 4096, height <= 4096, count <= 4_194_304,
              count == millimeters.count, count == confidence.count else { throw SpatialSnapshotError(SS_INVALID_ARGUMENT) }
        self.timestampNanoseconds = timestampNanoseconds; self.width = width; self.height = height
        self.fx = fx; self.fy = fy; self.cx = cx; self.cy = cy
        self.millimeters = millimeters; self.confidence = confidence
    }
    fileprivate func withCView<R>(_ body: (UnsafePointer<ss_depth_view_t>) throws -> R) rethrows -> R {
        try millimeters.withUnsafeBufferPointer { values in
            try confidence.withUnsafeBufferPointer { confidence in
                var view = ss_depth_view_t()
                view.struct_size = UInt32(MemoryLayout<ss_depth_view_t>.size); view.abi_version = SS_ABI_VERSION
                view.timestamp_ns = timestampNanoseconds; view.width = width; view.height = height
                view.fx = fx; view.fy = fy; view.cx = cx; view.cy = cy
                view.depth_mm = values.baseAddress; view.confidence = confidence.baseAddress
                return try body(&view)
            }
        }
    }
}

public enum GeometryUpdate: Sendable { case put(MeshChunk), remove(SIMD3<Int32>) }

public struct Packet: Sendable {
    public let sequenceNumber: UInt64
    public let timestampNanoseconds: UInt64
    public let byteOffset: UInt64
    public let type: UInt32
    public let flags: UInt32
    public let rawPayloadBytes: UInt32
    public let storedPayload: Data
}

public final class SpatialDocument {
    let handle: OpaquePointer
    public init(data: Data, limits: ResourceLimits = .init()) throws {
        var options = limits.cValue, handle: OpaquePointer?, error = ss_error_t()
        error.struct_size = UInt32(MemoryLayout<ss_error_t>.size); error.abi_version = SS_ABI_VERSION
        let status = data.withUnsafeBytes { ss_document_open_memory($0.baseAddress, $0.count, &options, &handle, &error) }
        guard status == SS_OK, let handle else { throw SpatialSnapshotError(status, detail: error) }
        self.handle = handle
    }
    init(taking handle: OpaquePointer) { self.handle = handle }
    deinit { ss_document_release(handle) }

    private var info: ss_document_info_t {
        var value = ss_document_info_t()
        value.struct_size = UInt32(MemoryLayout<ss_document_info_t>.size); value.abi_version = SS_ABI_VERSION
        precondition(ss_document_get_info(handle, &value) == SS_OK)
        return value
    }
    public var kind: StreamKind { StreamKind(rawValue: info.kind)! }
    public var durationNanoseconds: UInt64 { info.duration_ns }
    public var gravity: SIMD3<Double> { info.gravity.vector }
    public var streamID: Data {
        var id = info.stream_id
        return withUnsafeBytes(of: &id) { Data($0) }
    }
    public var checkpointCount: UInt64 { info.checkpoint_count }
    public func cameras() throws -> [Camera] {
        try (0..<info.camera_count).map { i in
            var camera = ss_camera_t(); ss_camera_init(&camera)
            try check(ss_document_camera(handle, i, &camera))
            return Camera(camera)
        }
    }
    public func packets() throws -> [Packet] {
        try (0..<info.packet_count).map { i in
            var packet = ss_packet_info_t(), bytes: UnsafePointer<UInt8>?, size = 0
            packet.struct_size = UInt32(MemoryLayout<ss_packet_info_t>.size); packet.abi_version = SS_ABI_VERSION
            try check(ss_document_packet(handle, i, &packet))
            try check(ss_document_packet_payload(handle, i, &bytes, &size))
            return Packet(sequenceNumber: packet.sequence_number, timestampNanoseconds: packet.timestamp_ns,
                          byteOffset: packet.byte_offset, type: packet.type, flags: packet.flags,
                          rawPayloadBytes: packet.raw_payload_size, storedPayload: size == 0 ? Data() : Data(bytes: bytes!, count: size))
        }
    }
    public func scene(at timestampNanoseconds: UInt64 = 0) throws -> SpatialScene {
        var cursor: OpaquePointer?
        try check(ss_scene_cursor_create(handle, &cursor))
        let scene = SpatialScene(taking: cursor!)
        try scene.seek(to: timestampNanoseconds)
        return scene
    }
    public func trim(from start: UInt64, to end: UInt64, limits: ResourceLimits = .init()) throws -> Data {
        var options = writerOptions(kind: .video, gravity: gravity, limits: limits), writer: OpaquePointer?
        try check(ss_document_trim(handle, start, end, &options, &writer))
        defer { ss_writer_release(writer) }
        return try encodedData(writer!)
    }
    public func validateMediaBinding(width: UInt32, height: UInt32, presentationTimes: [UInt64], duration: UInt64) throws {
        try presentationTimes.withUnsafeBufferPointer {
            try check(ss_document_validate_media_binding(handle, width, height, $0.baseAddress, $0.count, duration))
        }
    }
}

public struct SurfaceHit: Sendable {
    public let position: SIMD3<Double>
    public let normal: SIMD3<Double>
    public let distanceMeters: Double
    public let cell: SIMD3<Int32>
    public let triangleIndex: UInt32
    public let classification: SurfaceClassification
    fileprivate init(_ hit: ss_hit_t) {
        position = hit.position.vector; normal = hit.normal.vector; distanceMeters = hit.distance
        cell = SIMD3(hit.cell.x, hit.cell.y, hit.cell.z); triangleIndex = hit.triangle_index
        classification = SurfaceClassification(rawValue: UInt8(hit.classification)) ?? .unknown
    }
}

/// Mutable cursor. Use one serial executor for a document and its cursors.
public final class SpatialScene {
    private let handle: OpaquePointer
    fileprivate init(taking handle: OpaquePointer) { self.handle = handle }
    deinit { ss_scene_cursor_release(handle) }
    public func seek(to timestampNanoseconds: UInt64) throws { try check(ss_scene_cursor_seek(handle, timestampNanoseconds)) }
    public func camera() throws -> Camera? {
        var value = ss_camera_t(); ss_camera_init(&value)
        let status = ss_scene_cursor_camera(handle, &value)
        if status == SS_NO_SAMPLE { return nil }
        try check(status); return Camera(value)
    }
    public func meshes() throws -> [MeshChunk] {
        try (0..<ss_scene_cursor_chunk_count(handle)).map { i in
            var mesh = emptyMeshView()
            try check(ss_scene_cursor_chunk(handle, i, &mesh))
            return MeshChunk(mesh)
        }
    }
    public func depth() throws -> DepthSample? {
        var d = ss_depth_view_t()
        d.struct_size = UInt32(MemoryLayout<ss_depth_view_t>.size); d.abi_version = SS_ABI_VERSION
        let status = ss_scene_cursor_depth(handle, &d)
        if status == SS_NO_SAMPLE { return nil }
        try check(status)
        let count = Int(d.width) * Int(d.height)
        return try DepthSample(timestampNanoseconds: d.timestamp_ns, width: d.width, height: d.height,
                               fx: d.fx, fy: d.fy, cx: d.cx, cy: d.cy,
                               millimeters: Array(UnsafeBufferPointer(start: d.depth_mm, count: count)),
                               confidence: Array(UnsafeBufferPointer(start: d.confidence, count: count)))
    }
    public func depthPoint(u: UInt32, v: UInt32) throws -> (point: SIMD3<Double>, confidence: UInt32)? {
        var point = ss_vec3_t(), confidence: UInt32 = 0
        let status = ss_scene_depth_point(handle, u, v, &point, &confidence)
        if status == SS_NO_SAMPLE { return nil }
        try check(status); return (point.vector, confidence)
    }
    public func raycast(pixel: SIMD2<Double>) throws -> SurfaceHit? {
        var pixel = ss_vec2_t(x: pixel.x, y: pixel.y), hit = ss_hit_t()
        hit.struct_size = UInt32(MemoryLayout<ss_hit_t>.size); hit.abi_version = SS_ABI_VERSION
        let status = ss_scene_raycast_pixel(handle, &pixel, &hit)
        if status == SS_NOT_FOUND || status == SS_NO_SAMPLE { return nil }
        try check(status); return SurfaceHit(hit)
    }
    public func raycast(origin: SIMD3<Double>, direction: SIMD3<Double>, minDistance: Double = 0, maxDistance: Double = 0) throws -> SurfaceHit? {
        var ray = ss_ray_t(), hit = ss_hit_t()
        ray.struct_size = UInt32(MemoryLayout<ss_ray_t>.size); ray.abi_version = SS_ABI_VERSION
        ray.origin = ss_vec3_t(origin); ray.direction = ss_vec3_t(direction)
        ray.min_distance = minDistance; ray.max_distance = maxDistance
        hit.struct_size = UInt32(MemoryLayout<ss_hit_t>.size); hit.abi_version = SS_ABI_VERSION
        let status = ss_scene_raycast(handle, &ray, &hit)
        if status == SS_NOT_FOUND { return nil }
        try check(status); return SurfaceHit(hit)
    }
    public func isVisible(_ point: SIMD3<Double>, toleranceMeters: Double = 0.001) throws -> Bool {
        var point = ss_vec3_t(point), visible: UInt32 = 0
        try check(ss_scene_point_visible(handle, &point, toleranceMeters, &visible))
        return visible != 0
    }
}

func writerOptions(kind: StreamKind, gravity: SIMD3<Double>, limits: ResourceLimits) -> ss_writer_options_t {
    var options = ss_writer_options_t(); ss_writer_options_init(&options)
    options.kind = kind.rawValue; options.gravity = ss_vec3_t(gravity); options.resources = limits.cValue
    options.random_bytes = { _, destination, count in
        guard let destination else { return SS_INVALID_ARGUMENT }
        var generator = SystemRandomNumberGenerator()
        for i in 0..<count { destination[i] = UInt8.random(in: .min ... .max, using: &generator) }
        return SS_OK
    }
    return options
}
private func encodedData(_ writer: OpaquePointer) throws -> Data {
    var pointer: UnsafePointer<UInt8>?, count = 0
    try check(ss_writer_bytes(writer, &pointer, &count))
    return Data(bytes: pointer!, count: count)
}

public final class SpatialWriter {
    private let handle: OpaquePointer
    public init(kind: StreamKind, gravity: SIMD3<Double> = SIMD3(0, 1, 0), limits: ResourceLimits = .init()) throws {
        var options = writerOptions(kind: kind, gravity: gravity, limits: limits), handle: OpaquePointer?
        try check(ss_writer_create(&options, &handle))
        self.handle = handle!
    }
    deinit { ss_writer_release(handle) }
    public func append(camera: Camera, geometry: [GeometryUpdate] = [], depth: DepthSample? = nil) throws {
        var pinned: [PinnedMesh] = []
        let updates: [ss_geometry_update_t] = geometry.map { update in
            var c = ss_geometry_update_t()
            c.struct_size = UInt32(MemoryLayout<ss_geometry_update_t>.size); c.abi_version = SS_ABI_VERSION
            switch update {
            case .put(let mesh):
                let storage = PinnedMesh(mesh); pinned.append(storage)
                c.operation = SS_GEOMETRY_PUT; c.mesh = storage.view
            case .remove(let cell):
                c.operation = SS_GEOMETRY_REMOVE; c.mesh = emptyMeshView()
                c.mesh.cell = ss_cell_key_t(x: cell.x, y: cell.y, z: cell.z)
            }
            return c
        }
        var camera = camera.cValue
        try withExtendedLifetime(pinned) {
            try updates.withUnsafeBufferPointer { updates in
                let append: (UnsafePointer<ss_depth_view_t>?) throws -> Void = { depth in
                    try check(ss_writer_append_frame(self.handle, &camera, updates.baseAddress, updates.count, depth))
                }
                if let depth { try depth.withCView { try append($0) } }
                else { try append(nil) }
            }
        }
    }
    public func finish(durationNanoseconds: UInt64) throws -> Data {
        try check(ss_writer_finish(handle, durationNanoseconds))
        return try encodedData(handle)
    }
}

public final class SpatialStreamDecoder {
    private let handle: OpaquePointer
    public init(limits: ResourceLimits = .init()) throws {
        var options = limits.cValue, handle: OpaquePointer?
        try check(ss_decoder_create(&options, &handle)); self.handle = handle!
    }
    deinit { ss_decoder_release(handle) }
    public func feed(_ data: Data) throws {
        try data.withUnsafeBytes { try check(ss_decoder_feed(handle, $0.baseAddress, $0.count)) }
    }
    public func finish() throws -> SpatialDocument {
        var document: OpaquePointer?, error = ss_error_t()
        error.struct_size = UInt32(MemoryLayout<ss_error_t>.size); error.abi_version = SS_ABI_VERSION
        let status = ss_decoder_finish(handle, &document, &error)
        guard status == SS_OK, let document else { throw SpatialSnapshotError(status, detail: error) }
        return SpatialDocument(taking: document)
    }
}

public func presentationNanoseconds(value: UInt64, timescale: UInt32) throws -> UInt64 {
    var result: UInt64 = 0
    try check(ss_time_to_nanoseconds(value, timescale, &result))
    return result
}
