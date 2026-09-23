#if os(iOS) && canImport(ARKit)
@preconcurrency import ARKit
import AVFoundation

/// Managed-session convenience. Use SpatialSnapshotCapturePipeline when the host already has a session.
@MainActor
public final class SpatialSnapshotRecorder: NSObject, @preconcurrency ARSessionDelegate {
    public struct Capabilities: Sendable {
        public let worldTracking: Bool
        public let mesh: Bool
        public let classification: Bool
        public let depth: Bool
    }
    public static var capabilities: Capabilities {
        Capabilities(worldTracking: ARWorldTrackingConfiguration.isSupported,
            mesh: ARWorldTrackingConfiguration.supportsSceneReconstruction(.mesh),
            classification: ARWorldTrackingConfiguration.supportsSceneReconstruction(.meshWithClassification),
            depth: ARWorldTrackingConfiguration.supportsFrameSemantics(.sceneDepth))
    }
    public let session: ARSession
    public let pipeline: SpatialSnapshotCapturePipeline
    public var onUpdate: (@MainActor (SpatialSnapshotCapturePipeline.Statistics, String) -> Void)?
    public var onError: (@MainActor (Error) -> Void)?
    public var onDurationLimit: (@MainActor () -> Void)?
    public private(set) var isRunning = false

    public init(configuration: SpatialSnapshotCapturePipeline.Configuration = .init()) throws {
        pipeline = try SpatialSnapshotCapturePipeline(configuration: configuration)
        session = ARSession()
        super.init()
        session.delegateQueue = .main
        session.delegate = self
    }
    public func startSession() throws {
        guard Self.capabilities.worldTracking else { throw CaptureError.unavailable("この端末は ARKit の収録に対応していません. ") }
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            throw CaptureError.unavailable("設定でカメラへのアクセスを許可してください. ")
        }
        guard !isRunning else { return }
        let config = ARWorldTrackingConfiguration()
        config.worldAlignment = .gravity
        if Self.capabilities.classification { config.sceneReconstruction = .meshWithClassification }
        else if Self.capabilities.mesh { config.sceneReconstruction = .mesh }
        if Self.capabilities.depth && pipeline.configuration.includeDepth { config.frameSemantics.insert(.sceneDepth) }
        session.run(config, options: [.resetTracking, .removeExistingAnchors])
        isRunning = true
    }
    public func stopSession() {
        pipeline.cancel(); session.pause(); isRunning = false
    }
    public func captureStill(to destination: URL) throws {
        guard let frame = session.currentFrame else { throw CaptureError.unavailable("カメラのフレームを待っています. ") }
        try pipeline.captureStill(frame, to: destination)
    }
    public func session(_ session: ARSession, didUpdate frame: ARFrame) {
        do { try pipeline.ingest(frame) }
        catch { onError?(error) }
        let tracking: String
        switch frame.camera.trackingState {
        case .normal: tracking = "追跡中"
        case .notAvailable: tracking = "追跡できません"
        case .limited(let reason):
            switch reason {
            case .initializing: tracking = "空間を認識しています"
            case .excessiveMotion: tracking = "端末をゆっくり動かしてください"
            case .insufficientFeatures: tracking = "模様のある場所に向けてください"
            case .relocalizing: tracking = "位置を復元しています"
            @unknown default: tracking = "追跡が不安定です"
            }
        }
        onUpdate?(pipeline.statistics, tracking)
        if pipeline.isRecording && pipeline.reachedDurationLimit { onDurationLimit?() }
    }
    public func sessionWasInterrupted(_ session: ARSession) {
        let recording = pipeline.isRecording
        stopSession()
        if recording { onError?(CaptureError.unavailable("カメラが中断されたため収録を破棄しました. 再開してから撮り直してください. ")) }
    }
    public func session(_ session: ARSession, didFailWithError error: Error) {
        stopSession(); onError?(error)
    }
}
#endif
