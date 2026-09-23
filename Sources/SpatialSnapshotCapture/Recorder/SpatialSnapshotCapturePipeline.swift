#if os(iOS) && canImport(ARKit)
@preconcurrency import ARKit
import SpatialSnapshot
import SpatialSnapshotAppleMedia

/// Attach to an existing ARSession by forwarding frames. This type never changes its delegate.
@MainActor
public final class SpatialSnapshotCapturePipeline {
    public struct Configuration: Sendable {
        public var framesPerSecond = 15
        public var meshInterval: TimeInterval = 0.5
        /// Optional input face limit. By default, all available ARKit mesh faces are processed.
        /// C resource budgets and SSPS format limits still apply.
        public var maximumMeshFaces: Int? = nil
        public var maximumDuration: TimeInterval = 30
        public var includeDepth = true
        public init() {}
    }
    public struct Statistics: Sendable {
        public var acceptedFrames = 0
        public var droppedFrames = 0
        public var meshCells = 0
        public var duration: Double = 0
        public init() {}
    }
    public let configuration: Configuration
    public private(set) var statistics = Statistics()
    public private(set) var isRecording = false
    public private(set) var reachedDurationLimit = false
    public private(set) var meshSnapshot: CaptureMeshSnapshot?
    /// A render consumer may observe this without taking ownership of the ARSession.
    public var onMeshSnapshot: (@MainActor (CaptureMeshSnapshot?) -> Void)?
    private var clock = CaptureClock()
    private var coordinates: CaptureCoordinates?
    private var spatialWriter: SpatialWriter?
    private var movieWriter: SpatialMovieWriter?
    private var meshes: [SIMD3<Int32>: MeshChunk] = [:]
    private var lastMeshTime: Double?
    private var lastInterval: UInt64 = 66_666_667
    private var lastPreviewTime: Double?
    private var isFinishing = false

    public init(configuration: Configuration = .init()) throws {
        guard (1...30).contains(configuration.framesPerSecond),
              configuration.meshInterval.isFinite, configuration.meshInterval >= 0.1,
              (configuration.maximumMeshFaces.map { $0 > 0 } ?? true),
              configuration.maximumDuration.isFinite, (1...30).contains(configuration.maximumDuration) else {
            throw CaptureError.invalid("収録設定の範囲が不正です. ")
        }
        self.configuration = configuration
    }
    public func startRecording() throws {
        guard !isRecording, !isFinishing else { throw CaptureError.invalid("収録・保存の完了を待ってください. ") }
        cancel()
        clock = CaptureClock(); statistics = Statistics(); meshes = [:]
        coordinates = nil; lastMeshTime = nil; reachedDurationLimit = false
        lastInterval = UInt64((1_000_000_000.0 / Double(configuration.framesPerSecond)).rounded())
        isRecording = true
    }
    public func ingest(_ frame: ARFrame) throws {
        guard !isFinishing, !(isRecording && reachedDurationLimit) else { return }
        guard case .normal = frame.camera.trackingState else {
            if isRecording { statistics.droppedFrames += 1 }
            return
        }
        if !isRecording {
            // Prepare canonical geometry before the shutter is pressed, at the same bounded cadence.
            guard lastPreviewTime.map({ frame.timestamp - $0 >= configuration.meshInterval }) ?? true else { return }
            lastPreviewTime = frame.timestamp
            do {
                let basis = CaptureCoordinates(firstCameraTransform: frame.camera.transform)
                let chunks = try ARMeshConversion.chunks(anchors: frame.anchors, coordinates: basis,
                    maximumFaces: configuration.maximumMeshFaces)
                publishMesh(chunks, coordinates: basis)
            } catch {
                clearMesh()
                throw error
            }
            return
        }
        do {
            let timestamp = try clock.timestamp(for: frame.timestamp)
            if timestamp >= UInt64(configuration.maximumDuration * 1_000_000_000) {
                reachedDurationLimit = true; return
            }
            let minimumInterval = UInt64(1_000_000_000 / configuration.framesPerSecond)
            if let last = clock.lastTimestamp, timestamp - last < minimumInterval {
                statistics.droppedFrames += 1; return
            }
            let basis = coordinates ?? CaptureCoordinates(firstCameraTransform: frame.camera.transform)
            if movieWriter == nil {
                movieWriter = try SpatialMovieWriter(width: CVPixelBufferGetWidth(frame.capturedImage),
                    height: CVPixelBufferGetHeight(frame.capturedImage), framesPerSecond: configuration.framesPerSecond)
            }
            guard movieWriter!.isReadyForMoreMediaData else { statistics.droppedFrames += 1; return }
            let camera = basis.camera(transform: frame.camera.transform, intrinsics: frame.camera.intrinsics,
                width: UInt32(CVPixelBufferGetWidth(frame.capturedImage)),
                height: UInt32(CVPixelBufferGetHeight(frame.capturedImage)), timestamp: timestamp, isFirst: coordinates == nil)
            let updateMesh = lastMeshTime.map { frame.timestamp - $0 >= configuration.meshInterval } ?? true
            let next = updateMesh ? try ARMeshConversion.chunks(anchors: frame.anchors, coordinates: basis,
                maximumFaces: configuration.maximumMeshFaces) : nil
            let updates = next.map { ARMeshConversion.updates(previous: meshes, next: $0) } ?? []
            let depth = configuration.includeDepth ? try ARDepthConversion.sample(frame: frame, timestamp: timestamp) : nil
            guard try movieWriter!.append(frame.capturedImage, timestampNanoseconds: timestamp) else {
                statistics.droppedFrames += 1; return
            }
            if spatialWriter == nil { spatialWriter = try SpatialWriter(kind: .video, gravity: basis.gravity) }
            try spatialWriter!.append(camera: camera, geometry: updates, depth: depth)
            if let last = clock.lastTimestamp { lastInterval = timestamp - last }
            clock.accept(seconds: frame.timestamp, timestamp: timestamp)
            coordinates = basis
            if let next {
                meshes = Dictionary(uniqueKeysWithValues: next.map { ($0.cell, $0) })
                lastMeshTime = frame.timestamp
            }
            statistics.acceptedFrames += 1; statistics.meshCells = meshes.count
            statistics.duration = Double(timestamp) / 1_000_000_000
            if let next, meshSnapshot == nil || !updates.isEmpty {
                // Publish after committing every state field, so observers see the accepted frame.
                // These are the exact chunks passed to SpatialWriter, including removed cells.
                publishMesh(next, coordinates: basis)
            }
        } catch {
            cancel(); throw error
        }
    }
    public func finishRecording(to destination: URL) async throws {
        guard isRecording, let movieWriter, let spatialWriter, let last = clock.lastTimestamp else {
            cancel(); throw CaptureError.invalid("安定したカメラフレームをまだ収録していません. ")
        }
        isRecording = false
        isFinishing = true
        defer { isFinishing = false }
        do {
            let data = try spatialWriter.finish(durationNanoseconds: last + lastInterval)
            try await movieWriter.finish(spatialData: data, to: destination)
            self.movieWriter = nil; self.spatialWriter = nil
        } catch { cancel(); throw error }
    }
    public func captureStill(_ frame: ARFrame, to destination: URL) throws {
        guard !isRecording, !isFinishing else { throw CaptureError.invalid("収録・保存の完了を待ってください. ") }
        guard case .normal = frame.camera.trackingState else { throw CaptureError.unavailable("カメラの追跡が安定してから撮影してください. ") }
        let basis = CaptureCoordinates(firstCameraTransform: frame.camera.transform)
        let camera = basis.camera(transform: frame.camera.transform, intrinsics: frame.camera.intrinsics,
            width: UInt32(CVPixelBufferGetWidth(frame.capturedImage)),
            height: UInt32(CVPixelBufferGetHeight(frame.capturedImage)), timestamp: 0, isFirst: true)
        let chunks = try ARMeshConversion.chunks(anchors: frame.anchors, coordinates: basis, maximumFaces: configuration.maximumMeshFaces)
        let writer = try SpatialWriter(kind: .still, gravity: basis.gravity)
        try writer.append(camera: camera, geometry: chunks.map { .put($0) },
            depth: configuration.includeDepth ? ARDepthConversion.sample(frame: frame, timestamp: 0) : nil)
        let data = try writer.finish(durationNanoseconds: 0)
        let image = try SpatialHEIFEncoder.image(from: frame.capturedImage)
        try SpatialHEIFEncoder.encode(image: image, spatialData: data).write(to: destination, options: .atomic)
        publishMesh(chunks, coordinates: basis)
    }
    public func cancel() {
        movieWriter?.cancel(); movieWriter = nil; spatialWriter = nil
        isRecording = false; coordinates = nil; meshes = [:]
        reachedDurationLimit = false; lastPreviewTime = nil
        clearMesh()
    }
    private func publishMesh(_ chunks: [MeshChunk], coordinates: CaptureCoordinates) {
        let snapshot = CaptureMeshSnapshot(chunks: chunks, coordinates: coordinates)
        meshSnapshot = snapshot
        onMeshSnapshot?(snapshot)
    }
    private func clearMesh() {
        meshSnapshot = nil
        onMeshSnapshot?(nil)
    }
}
#endif
