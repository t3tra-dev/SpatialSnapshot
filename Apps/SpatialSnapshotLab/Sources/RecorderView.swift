#if os(iOS)
import SwiftUI
@preconcurrency import AVFoundation
import ARKit
import RealityKit
import SpatialSnapshotCapture
import SpatialSnapshotRealityKit

@MainActor
final class RecorderModel: ObservableObject {
    let recorder: SpatialSnapshotRecorder?
    @Published var running = false
    @Published var recording = false
    @Published var busy = false
    @Published var tracking = "カメラは停止中です"
    @Published var statistics = SpatialSnapshotCapturePipeline.Statistics()
    @Published var error: String?
    @Published var meshSnapshot: CaptureMeshSnapshot?
    @Published var meshDisplayError: String?
    @Published var showMesh = true
    var onSaved: (@MainActor (URL) -> Void)?
    init() {
        do { recorder = try SpatialSnapshotRecorder() }
        catch { recorder = nil; self.error = error.localizedDescription }
        recorder?.onUpdate = { [weak self] statistics, tracking in
            self?.statistics = statistics; self?.tracking = tracking
        }
        recorder?.onError = { [weak self] error in
            self?.error = error.localizedDescription
            self?.recording = false; self?.running = self?.recorder?.isRunning ?? false
        }
        recorder?.onDurationLimit = { [weak self] in self?.finishVideo() }
        recorder?.pipeline.onMeshSnapshot = { [weak self] snapshot in
            self?.meshSnapshot = snapshot
            if snapshot == nil { self?.meshDisplayError = nil }
        }
    }
    func start() async {
        guard let recorder, !busy else { return }
        busy = true
        defer { busy = false }
        guard await AVCaptureDevice.requestAccess(for: .video) else {
            error = "設定アプリで SpatialSnapshot Lab のカメラへのアクセスを許可してください. "; return
        }
        do { try recorder.startSession(); running = true }
        catch { self.error = error.localizedDescription }
    }
    func stop() {
        if recording { error = "カメラを停止したため, 保存前の収録を破棄しました. " }
        recorder?.stopSession(); running = false; recording = false
    }
    func photo() {
        guard let recorder, !busy else { return }
        busy = true
        defer { busy = false }
        do {
            let url = try LabModel.newRecordingURL(extension: "heic")
            try recorder.captureStill(to: url); onSaved?(url)
        } catch { self.error = error.localizedDescription }
    }
    func startVideo() {
        guard let recorder, !busy else { return }
        do { try recorder.pipeline.startRecording(); recording = true }
        catch { self.error = error.localizedDescription }
    }
    func finishVideo() {
        guard let recorder, recording, !busy else { return }
        busy = true; recording = false
        Task {
            defer { busy = false }
            do {
                let url = try LabModel.newRecordingURL(extension: "mov")
                try await recorder.pipeline.finishRecording(to: url); onSaved?(url)
            } catch { self.error = error.localizedDescription }
        }
    }
}

struct RecorderView: View {
    @ObservedObject var model: LabModel
    @StateObject private var capture = RecorderModel()
    @Environment(\.scenePhase) private var scenePhase
    var body: some View {
        GeometryReader { geometry in
            ZStack {
                cameraBackground
                    .ignoresSafeArea()
                LinearGradient(colors: [.black.opacity(0.6), .clear, .clear, .black.opacity(0.75)],
                    startPoint: .top, endPoint: .bottom)
                    .ignoresSafeArea()
                    .allowsHitTesting(false)
                VStack(spacing: 12) {
                    header
                    Spacer(minLength: 12)
                    controls(compact: geometry.size.height < 450)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(.black)
        .statusBarHidden()
        .interactiveDismissDisabled()
        .onAppear {
            capture.onSaved = { url in
                model.refreshFiles()
                Task { await model.open(url) }
            }
        }
        .onDisappear { capture.stop() }
        .onChange(of: scenePhase) { _, phase in if phase != .active { capture.stop() } }
        .alert("収録を完了できません", isPresented: Binding(get: { capture.error != nil },
            set: { if !$0 { capture.error = nil } })) {
                Button("閉じる", role: .cancel) { capture.error = nil }
            } message: { Text(capture.error ?? "") }
    }
    private var capabilities: SpatialSnapshotRecorder.Capabilities { SpatialSnapshotRecorder.capabilities }

    @ViewBuilder
    private var cameraBackground: some View {
        if capture.running, let recorder = capture.recorder {
            CapturePreview(session: recorder.session, snapshot: capture.meshSnapshot, showMesh: capture.showMesh,
                onMeshError: { capture.meshDisplayError = $0 })
                .accessibilityLabel("カメラ映像と保存メッシュ")
        } else {
            ZStack {
                Color.black
                VStack(spacing: 18) {
                    Image(systemName: "viewfinder").font(.system(size: 64, weight: .ultraLight)).foregroundStyle(.mint)
                    Text(capabilities.worldTracking ? "空間を持ち帰る" : "この端末では収録できません")
                        .font(.title2.weight(.semibold))
                    Text(capabilities.worldTracking ? "カメラを開始し, 端末をゆっくり動かして\n周囲を認識させてください. " :
                        "ARKit 対応の iPhone / iPad で収録できます. \nInspector では保存済みファイルを検査できます. ")
                        .font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center)
                }.padding(28)
            }
        }
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 12) {
            Button {
                capture.stop()
                model.section = .inspector
            } label: {
                Image(systemName: "xmark").font(.body.weight(.semibold)).frame(width: 44, height: 44)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .accessibilityLabel("Inspector に戻る")
            .disabled(capture.busy || capture.recording)
            VStack(alignment: .leading, spacing: 5) {
                Text("RECORDER").font(.system(.caption, design: .monospaced).weight(.semibold)).tracking(2)
                    .foregroundStyle(.mint)
                Label(capture.tracking, systemImage: capture.running ? "location.fill" : "location.slash")
                    .font(.caption).lineLimit(2)
            }.padding(.top, 5)
            Spacer(minLength: 0)
            if capture.recording {
                Label(String(format: "%04.1f s", capture.statistics.duration), systemImage: "record.circle.fill")
                    .font(.system(.subheadline, design: .monospaced).weight(.semibold)).foregroundStyle(.red)
                    .padding(12).background(.ultraThinMaterial, in: Capsule())
            } else {
                Text("写真 / 動画").font(.caption).foregroundStyle(.white.opacity(0.8)).padding(.top, 10)
            }
        }
        .foregroundStyle(.white)
    }

    private func controls(compact: Bool) -> some View {
        VStack(spacing: compact ? 10 : 16) {
            if capture.running {
                HStack(spacing: 10) {
                    Button {
                        capture.showMesh.toggle()
                    } label: {
                        Label("保存メッシュ", systemImage: capture.showMesh ? "cube.transparent.fill" : "cube.transparent")
                            .font(.caption.weight(.semibold))
                    }
                    .tint(capture.showMesh ? .mint : .white)
                    .accessibilityValue(capture.showMesh ? "表示中" : "非表示")
                    .disabled(!capabilities.mesh)
                    Spacer(minLength: 0)
                    if capabilities.mesh, let mesh = capture.meshSnapshot {
                        Text("\(mesh.chunks.count) セル · \(mesh.triangleCount.formatted()) 面")
                            .font(.system(.caption2, design: .monospaced)).monospacedDigit()
                    } else {
                        Text(capabilities.mesh ? "メッシュを認識中" : "メッシュ・深度には LiDAR が必要です")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
                if let error = capture.meshDisplayError {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.caption).foregroundStyle(.orange)
                }
            }
            captureButtons
            if !compact {
                HStack(spacing: 16) {
                    capability("メッシュ", available: capabilities.mesh)
                    capability("分類", available: capabilities.classification)
                    capability("深度", available: capabilities.depth)
                }
                Text(capture.recording ?
                    "保存対象 \(capture.statistics.acceptedFrames) フレーム · 間引き \(capture.statistics.droppedFrames)" :
                    "動画は最大 30 秒 · 15 fps · 音声なし")
                    .font(.caption2).foregroundStyle(.white.opacity(0.65)).monospacedDigit()
            }
        }
        .foregroundStyle(.white)
        .padding(compact ? 14 : 18)
        .frame(maxWidth: 600)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24))
        .overlay(RoundedRectangle(cornerRadius: 24).strokeBorder(.white.opacity(0.12)))
        .frame(maxWidth: .infinity)
    }

    private var captureButtons: some View {
        HStack(spacing: 16) {
            if capture.running {
                if capture.recording {
                    Button { capture.finishVideo() } label: {
                        Label("停止して保存", systemImage: "stop.circle.fill")
                            .frame(maxWidth: .infinity, minHeight: 36)
                    }.buttonStyle(.borderedProminent).tint(.red)
                } else {
                    Button { capture.stop() } label: {
                        Image(systemName: "power").font(.title3).frame(width: 44, height: 60)
                    }.accessibilityLabel("カメラを停止")
                    Spacer(minLength: 0)
                    Button { capture.photo() } label: {
                        ZStack {
                            Circle().strokeBorder(.white, lineWidth: 3).frame(width: 66, height: 66)
                            Circle().fill(.white).frame(width: 54, height: 54)
                            Image(systemName: "camera.fill").foregroundStyle(.black).font(.title3)
                        }
                    }.accessibilityLabel("写真を撮影")
                    Spacer(minLength: 0)
                    Button { capture.startVideo() } label: {
                        Image(systemName: "record.circle").font(.system(size: 36)).foregroundStyle(.red)
                            .frame(width: 44, height: 60)
                    }.accessibilityLabel("動画を収録")
                }
            } else {
                Button { Task { await capture.start() } } label: {
                    Label("カメラを開始", systemImage: "camera")
                        .frame(maxWidth: .infinity, minHeight: 36)
                }.buttonStyle(.borderedProminent).disabled(!capabilities.worldTracking)
            }
            if capture.busy { ProgressView().tint(.white).accessibilityLabel("処理中") }
        }
        .disabled(capture.busy)
        .buttonStyle(.plain)
    }

    private func capability(_ label: String, available: Bool) -> some View {
        Label(label, systemImage: available ? "checkmark.circle.fill" : "minus.circle")
            .font(.caption).foregroundStyle(available ? .mint : .secondary)
    }
}

private struct CapturePreview: UIViewRepresentable {
    let session: ARSession
    let snapshot: CaptureMeshSnapshot?
    let showMesh: Bool
    let onMeshError: @MainActor (String?) -> Void
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeUIView(context: Context) -> ARView {
        let view = ARView(frame: .zero, cameraMode: .ar, automaticallyConfigureSession: false)
        view.session = session
        // Depth/occlusion from ARKit's raw scene must not hide the canonical debug mesh.
        view.environment.sceneUnderstanding.options = []
        view.renderOptions.insert(.disablePersonOcclusion)
        view.scene.addAnchor(context.coordinator.anchor)
        return view
    }
    func updateUIView(_ view: ARView, context: Context) {
        context.coordinator.update(snapshot: snapshot, showMesh: showMesh, onError: onMeshError)
    }
    static func dismantleUIView(_ view: ARView, coordinator: Coordinator) {
        coordinator.anchor.removeFromParent()
    }

    @MainActor
    final class Coordinator {
        let anchor = AnchorEntity(world: .zero)
        private var revision: UUID?
        private var mesh: Entity?
        func update(snapshot: CaptureMeshSnapshot?, showMesh: Bool, onError: @escaping @MainActor (String?) -> Void) {
            anchor.isEnabled = showMesh
            guard let snapshot else {
                mesh?.removeFromParent(); mesh = nil; revision = nil
                return
            }
            // Hiding the overlay does not change capture; defer GPU rebuilds until shown again.
            guard showMesh, snapshot.id != revision else { return }
            mesh?.removeFromParent(); mesh = nil
            revision = snapshot.id
            do {
                let entity = try SpatialSnapshotRealityKit.debugMeshEntity(from: snapshot.chunks)
                // The snapshot already maps SSPS axes into AR world. Replace the adapter's
                // standalone axis conversion rather than applying that conversion twice.
                entity.transform.matrix = snapshot.worldFromScene
                anchor.addChild(entity)
                mesh = entity
                Task { @MainActor [weak self] in
                    if self?.revision == snapshot.id { onError(nil) }
                }
            } catch {
                Task { @MainActor [weak self] in
                    if self?.revision == snapshot.id { onError("メッシュを表示できません: " + error.localizedDescription) }
                }
            }
        }
    }
}
#endif
