#if canImport(SwiftUI)
import SwiftUI
import SpatialSnapshot
import SpatialSnapshotAppleMedia
import UniformTypeIdentifiers

struct InspectorView: View {
    @ObservedObject var model: LabModel
    #if !os(iOS)
    @State private var exporting = false
    @State private var exportDocument = ExportDocument(data: Data())
    @State private var exportType: UTType = .data
    @State private var exportName = "SpatialSnapshot.ssps"
    #endif
    @State private var layerSelection: InspectorLayer
    init(model: LabModel) {
        self.model = model
        _layerSelection = State(initialValue: model.layer)
    }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header
                if let asset = model.asset {
                    viewer
                    if model.frameCount > 1 { timeline }
                    ViewThatFits(in: .horizontal) {
                        HStack(alignment: .top, spacing: 16) {
                            statistics.frame(minWidth: 280)
                            cameraDetails.frame(minWidth: 280)
                            placement.frame(minWidth: 240)
                        }
                        VStack(spacing: 16) { statistics; cameraDetails; placement }
                    }
                    if asset.document.kind == .video { trimControls }
                    HStack {
                        Label(model.status, systemImage: "checkmark.shield").foregroundStyle(.secondary)
                        Spacer()
                        Text("SSPS v1").fontDesign(.monospaced).foregroundStyle(.mint)
                    }.font(.caption)
                } else {
                    ContentUnavailableView("空間を開く", systemImage: "cube.transparent",
                        description: Text("HEIC・MOV・SSPS を開くか, サイドバーからデモを選んでください. "))
                        .frame(minHeight: 400)
                }
            }.padding(24)
        }
        .background(Color(red: 0.035, green: 0.045, blue: 0.065))
        .navigationTitle("Inspector")
        .onChange(of: model.layer) { _, value in if layerSelection != value { layerSelection = value } }
        .task(id: layerSelection) {
            guard !Task.isCancelled, model.layer != layerSelection else { return }
            model.layer = layerSelection
        }
        #if !os(iOS)
        .fileExporter(isPresented: $exporting, document: exportDocument, contentType: exportType,
            defaultFilename: exportName) { result in
                if case .failure(let error) = result { model.report(error) }
            }
        #endif
        .onDisappear { model.pause() }
    }
    private var header: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 6) {
                Text("SPATIAL / INSPECTOR").font(.system(.caption, design: .monospaced)).tracking(3).foregroundStyle(.mint)
                Text(model.asset?.url.lastPathComponent ?? "SpatialSnapshot Lab")
                    .font(.system(size: 28, weight: .semibold, design: .rounded)).lineLimit(1)
                Text("archived /w ssnp").font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            if let asset = model.asset {
                #if os(iOS)
                if let shared = SpatialCaptureShare(asset: asset) {
                    let preview = model.image.map { Image(decorative: $0, scale: 1) } ??
                        Image(systemName: shared.contentType == .quickTimeMovie ? "film" : "photo")
                    ShareLink(item: shared, preview: SharePreview(asset.url.lastPathComponent, image: preview)) {
                        Label("共有", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(.bordered).disabled(model.isBusy)
                    .accessibilityIdentifier("capture.share")
                    .accessibilityHint("空間データを含む画像・動画ファイルを共有")
                } else {
                    VStack(alignment: .trailing, spacing: 6) {
                        Button {} label: { Label("共有", systemImage: "square.and.arrow.up") }
                            .buttonStyle(.bordered).disabled(true)
                        Text("画像・動画がないため共有できません")
                            .font(.caption2).foregroundStyle(.secondary)
                            .multilineTextAlignment(.trailing).frame(maxWidth: 120)
                    }
                }
                #else
                Menu {
                    Button("SSPS を書き出す", systemImage: "cube.transparent") {
                        exportDocument = ExportDocument(data: asset.spatialData)
                        exportType = UTType(filenameExtension: "ssps") ?? .data
                        exportName = asset.url.deletingPathExtension().lastPathComponent + ".ssps"
                        exporting = true
                    }
                    Button("元のファイルを書き出す", systemImage: "square.and.arrow.up") {
                        prepareExport(stripped: false)
                    }
                    if asset.media != nil {
                        Button("空間データを除いて書き出す", systemImage: "photo") {
                            prepareExport(stripped: true)
                        }
                    }
                } label: { Label("書き出す", systemImage: "square.and.arrow.up") }
                .buttonStyle(.bordered).disabled(model.isBusy)
                #endif
            }
        }
    }
    private var viewer: some View {
        VStack(spacing: 0) {
            HStack {
                Picker("表示", selection: $layerSelection) {
                    ForEach(InspectorLayer.allCases) { Text($0.rawValue).tag($0) }
                }.pickerStyle(.segmented).frame(maxWidth: 260)
                Spacer()
                if model.layer == .image {
                    Toggle("メッシュ", isOn: $model.showMesh).toggleStyle(.switch).fixedSize()
                }
                if model.layer != .scene {
                    Button {
                        model.rasterQuarterTurns = (model.rasterQuarterTurns + 1) % 4
                    } label: { Image(systemName: "rotate.right") }
                    .accessibilityLabel("表示を時計回りに90度回転")
                    .help("表示を時計回りに90度回転")
                }
            }.padding(14)
            if model.layer == .scene {
                SpatialSceneView(model: model).frame(height: 420)
            } else {
                RasterView(model: model).frame(height: 420)
            }
            HStack(spacing: 8) {
                Circle().fill(.mint).frame(width: 6, height: 6)
                Text(model.layer == .image ? "映像をタップして面を検査・配置" :
                    model.layer == .depth ? "近い面は黄, 遠い面は青. 暗さは信頼度. 未測定は黒. " : "ドラッグで周回 · スライダーで距離を変更")
                Spacer()
                Text(model.camera.map { "\($0.width) × \($0.height)" } ?? "")
                    .monospacedDigit()
            }.font(.caption).foregroundStyle(.secondary).padding(14)
        }
        .background(Color.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).strokeBorder(.white.opacity(0.08)))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }
    private var timeline: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 16) {
                playbackButtons
                frameSlider.frame(minWidth: 120)
                frameTime
            }
            VStack(spacing: 14) {
                HStack { playbackButtons; Spacer(); frameTime }
                frameSlider
            }
        }.buttonStyle(.borderless).padding(16).labCard()
    }
    private var playbackButtons: some View {
        HStack(spacing: 16) {
            Button { model.scrub(to: max(0, model.frameIndex - 1)) } label: { Image(systemName: "backward.frame") }
                .disabled(model.frameIndex == 0)
            Button { model.togglePlayback() } label: { Image(systemName: model.isPlaying ? "pause.fill" : "play.fill").frame(width: 24) }
                .keyboardShortcut(.space, modifiers: [])
            Button { model.scrub(to: min(model.frameCount - 1, model.frameIndex + 1)) } label: { Image(systemName: "forward.frame") }
                .disabled(model.frameIndex == model.frameCount - 1)
        }.fixedSize()
    }
    private var frameSlider: some View {
        Slider(value: Binding(get: { Double(model.frameIndex) }, set: { model.scrub(to: Int($0)) }),
            in: 0...Double(max(1, model.frameCount - 1)), step: 1).accessibilityLabel("フレーム")
    }
    private var frameTime: some View {
        VStack(alignment: .trailing, spacing: 2) {
                Text(String(format: "%.3f / %.3f s", Double(model.camera?.timestampNanoseconds ?? 0) / 1e9, model.duration))
                Text("\(model.frameIndex + 1) / \(model.frameCount) フレーム").foregroundStyle(.secondary)
            }.font(.system(.caption, design: .monospaced)).fixedSize()
    }
    private var statistics: some View {
        VStack(alignment: .leading, spacing: 14) {
            cardTitle("空間データ", icon: "cube.transparent")
            metric("メッシュセル", "\(model.meshCells)")
            metric("三角形", model.triangleCount.formatted())
            metric("深度", model.depthSize)
            metric("フレーム", "\(model.frameCount)")
            if let asset = model.asset {
                metric("形式", asset.media.map { $0.kind == .heif ? "HEIF + SSPS" : "MOV + SSPS" } ?? "SSPS")
                metric("チェックポイント", "\(asset.document.checkpointCount)")
            }
        }.padding(18).labCard()
    }
    private var cameraDetails: some View {
        VStack(alignment: .leading, spacing: 14) {
            cardTitle("カメラ", icon: "camera")
            if let camera = model.camera {
                metric("時刻", "\(camera.timestampNanoseconds) ns")
                metric("fx / fy", String(format: "%.2f / %.2f", camera.fx, camera.fy))
                metric("cx / cy", String(format: "%.2f / %.2f", camera.cx, camera.cy))
                metric("位置 m", String(format: "%.3f, %.3f, %.3f", camera.translation.x, camera.translation.y, camera.translation.z))
                Text(String(format: "q = [%.3f, %.3f, %.3f, %.3f]", camera.quaternion.x,
                    camera.quaternion.y, camera.quaternion.z, camera.quaternion.w))
                    .font(.system(.caption2, design: .monospaced)).foregroundStyle(.secondary)
                Text("+X 右 / +Y 下 / +Z 前").font(.caption).foregroundStyle(.secondary)
            }
        }.padding(18).labCard()
    }
    private var placement: some View {
        VStack(alignment: .leading, spacing: 14) {
            cardTitle("面を検査", icon: "scope")
            if let hit = model.hit {
                metric("分類", String(describing: hit.classification))
                metric("距離", String(format: "%.3f m", hit.distanceMeters))
                metric("位置", String(format: "%.3f, %.3f, %.3f", hit.position.x, hit.position.y, hit.position.z))
                metric("法線", String(format: "%.2f, %.2f, %.2f", hit.normal.x, hit.normal.y, hit.normal.z))
                Button("3D で配置を見る") { model.layer = .scene }
                    .buttonStyle(.bordered)
            } else {
                Text(model.hitMessage ?? "映像上の床やテーブルをタップすると, 距離・分類・法線を確認できます. ")
                    .font(.subheadline).foregroundStyle(.secondary).frame(minHeight: 90, alignment: .topLeading)
            }
        }.frame(maxWidth: .infinity, alignment: .leading).padding(18).labCard()
    }
    private var trimControls: some View {
        VStack(alignment: .leading, spacing: 12) {
            cardTitle("範囲を切り出す", icon: "crop")
            Text("開始フレームから終了フレームまでを保存し, 先頭カメラを新しい原点にします. ")
                .font(.caption).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 12) {
                Stepper("開始フレーム: \(model.trimStart + 1)", value: $model.trimStart, in: 0...max(0, model.trimEnd - 1))
                Stepper("終了フレーム: \(model.trimEnd)", value: $model.trimEnd, in: (model.trimStart + 1)...max(model.trimStart + 1, model.frameCount))
                Button("切り出して保存") { Task { await model.trim() } }.buttonStyle(.borderedProminent)
                    .disabled(model.isBusy)
            }.font(.caption).monospacedDigit()
        }.padding(18).labCard()
    }
    private func cardTitle(_ title: String, icon: String) -> some View {
        Label(title, systemImage: icon).font(.subheadline.weight(.semibold)).foregroundStyle(.mint)
    }
    private func metric(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(.secondary)
            Spacer(minLength: 8)
            Text(value).fontDesign(.monospaced).textSelection(.enabled)
        }.font(.caption)
    }
    #if !os(iOS)
    private func prepareExport(stripped: Bool) {
        guard let asset = model.asset else { return }
        do {
            let source = try AppleSpatialAsset.boundedData(asset.url)
            let data = try stripped ? SpatialMedia.removingSpatialMetadata(from: source, kind: asset.media!.kind) : source
            exportDocument = ExportDocument(data: data)
            exportType = UTType(filenameExtension: asset.url.pathExtension) ?? .data
            exportName = asset.url.deletingPathExtension().lastPathComponent + (stripped ? "-plain." : ".") + asset.url.pathExtension
            exporting = true
        } catch { model.report(error) }
    }
    #endif
}

private struct RasterView: View {
    @ObservedObject var model: LabModel
    var body: some View {
        GeometryReader { geometry in
            let camera = model.camera
            let width = CGFloat(camera?.width ?? 640), height = CGFloat(camera?.height ?? 480)
            let layout = InspectorRasterLayout(rasterSize: CGSize(width: width, height: height),
                viewportSize: geometry.size, quarterTurns: model.rasterQuarterTurns)
            let selectedImage = model.layer == .depth ? model.depthImage : model.image
            ZStack {
                Color.black
                if let selectedImage {
                    Image(decorative: selectedImage, scale: 1)
                        .resizable().interpolation(model.layer == .depth ? .none : .high)
                        .frame(width: layout.sourceSize.width, height: layout.sourceSize.height)
                        .rotationEffect(.degrees(Double(layout.quarterTurns) * 90))
                } else {
                    VStack(spacing: 12) {
                        Image(systemName: model.layer == .depth ? "square.3.layers.3d" : "photo").font(.largeTitle)
                        Text(model.layer == .depth ? "このフレームに深度データはありません" :
                            model.decodeError ?? (model.asset?.media == nil ? "SSPS 単体には映像がありません" : "フレームを読み込んでいます"))
                            .font(.caption).multilineTextAlignment(.center)
                    }.foregroundStyle(.secondary).padding()
                }
                if model.layer == .image {
                    Canvas { context, _ in
                        context.clip(to: Path(layout.contentRect))
                        if model.showMesh {
                            var path = Path()
                            for edge in model.edges {
                                path.move(to: layout.viewPoint(for: edge.a))
                                path.addLine(to: layout.viewPoint(for: edge.b))
                            }
                            context.stroke(path, with: .color(.mint.opacity(0.5)), lineWidth: 0.7)
                        }
                        if let point = model.hitPixel {
                            let displayed = layout.viewPoint(for: point)
                            let rect = CGRect(x: displayed.x - 7, y: displayed.y - 7, width: 14, height: 14)
                            context.fill(Path(ellipseIn: rect), with: .color(.orange))
                            context.stroke(Path(ellipseIn: rect.insetBy(dx: -4, dy: -4)), with: .color(.white), lineWidth: 1)
                        }
                    }
                    .frame(width: geometry.size.width, height: geometry.size.height).clipped()
                    .contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { value in
                        if let pixel = layout.pixel(at: value.location) { model.pick(pixel: pixel) }
                    })
                }
            }.frame(width: geometry.size.width, height: geometry.size.height).clipped()
        }
    }
}

private extension View {
    func labCard() -> some View {
        frame(maxWidth: .infinity, alignment: .leading)
            .background(.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(.white.opacity(0.07)))
    }
}

#endif
