#if canImport(SwiftUI)
import SwiftUI
import UniformTypeIdentifiers
import SpatialSnapshot
import SpatialSnapshotAppleMedia

enum LabSection: String, CaseIterable, Identifiable {
    case inspector = "Inspector"
    case recorder = "Recorder"
    case editor = "Editor"
    var id: String { rawValue }
}
enum InspectorLayer: String, CaseIterable, Identifiable {
    case image = "映像", depth = "深度", scene = "3D"
    var id: String { rawValue }
}
struct ProjectedEdge: Identifiable {
    let id: Int
    let a: CGPoint
    let b: CGPoint
}
struct ExportDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }
    var data: Data
    init(data: Data) { self.data = data }
    init(configuration: ReadConfiguration) throws { data = configuration.file.regularFileContents ?? Data() }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { FileWrapper(regularFileWithContents: data) }
}

@MainActor
final class LabModel: ObservableObject {
    #if os(macOS)
    @Published var section: LabSection? = .editor
    #else
    @Published var section: LabSection? = .inspector
    #endif
    @Published var asset: AppleSpatialAsset?
    @Published var scene: SpatialScene?
    @Published var frameIndex = 0
    @Published var image: CGImage?
    @Published var depthImage: CGImage?
    @Published var edges: [ProjectedEdge] = []
    @Published var meshCells = 0
    @Published var triangleCount = 0
    @Published var depthSize: String = "なし"
    @Published var hit: SurfaceHit?
    @Published var hitPixel: CGPoint?
    @Published var hitMessage: String?
    @Published var layer: InspectorLayer = .image
    @Published var rasterQuarterTurns = 0
    @Published var showMesh = true
    @Published var isPlaying = false
    @Published var isBusy = false
    @Published var status = "ファイルを開くか, デモを選んでください"
    @Published var error: String?
    @Published var decodeError: String?
    @Published var revision = UUID()
    @Published var recentFiles: [URL] = []
    @Published var trimStart = 0
    @Published var trimEnd = 1
    private var generation = 0
    private var playbackTask: Task<Void, Never>?
    private var playbackID = UUID()
    private let libraryDirectoryOverride: URL?
    var camera: Camera? { asset.flatMap { $0.cameras.indices.contains(frameIndex) ? $0.cameras[frameIndex] : nil } }
    var frameCount: Int { asset?.cameras.count ?? 0 }
    var duration: Double { Double(asset?.document.durationNanoseconds ?? 0) / 1e9 }

    init(libraryDirectory: URL? = nil) {
        libraryDirectoryOverride = libraryDirectory
        refreshFiles()
    }
    private func captureDirectory() throws -> URL {
        try libraryDirectoryOverride ?? Self.libraryDirectory()
    }
    func editorProjectDirectory() throws -> URL {
        try captureDirectory().appendingPathComponent("EditorProjects", isDirectory: true)
    }
    private func captureFileURL(_ url: URL) -> URL {
        // Normalize directory aliases such as /var and /private/var, but never follow
        // the final component: deleting a symlink must remove the link itself.
        url.deletingLastPathComponent().resolvingSymlinksInPath()
            .appendingPathComponent(url.lastPathComponent)
    }
    static func libraryDirectory() throws -> URL {
        let documents = try FileManager.default.url(for: .documentDirectory, in: .userDomainMask,
            appropriateFor: nil, create: true)
        let directory = documents.appendingPathComponent("SpatialSnapshotLab", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }
    static func newRecordingURL(extension ext: String) throws -> URL {
        let name = Date().formatted(.iso8601.year().month().day().time(includingFractionalSeconds: false))
            .replacingOccurrences(of: ":", with: "-")
        return try libraryDirectory().appendingPathComponent("Capture-" + name + "-" + UUID().uuidString.prefix(6) + "." + ext)
    }
    func refreshFiles() {
        guard let directory = try? captureDirectory() else { return }
        recentFiles = ((try? FileManager.default.contentsOfDirectory(at: directory,
            includingPropertiesForKeys: [.creationDateKey], options: .skipsHiddenFiles)) ?? [])
            .filter { ["heic", "heif", "mov", "ssps"].contains($0.pathExtension.lowercased()) }
            .sorted { $0.lastPathComponent > $1.lastPathComponent }
    }
    func deleteCapture(_ url: URL) {
        guard !isBusy else { return }
        do {
            let target = captureFileURL(url)
            let directory = try captureDirectory().resolvingSymlinksInPath()
            guard url.isFileURL, target.deletingLastPathComponent().path == directory.path,
                  recentFiles.contains(where: { captureFileURL($0) == target }) else {
                throw AppleMediaError.invalid("ライブラリ内のファイルを選択してください. ")
            }
            let values = try target.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            guard values.isRegularFile == true || values.isSymbolicLink == true else {
                throw AppleMediaError.invalid("削除できるのは収録ファイルだけです. ")
            }
            try FileManager.default.removeItem(at: target)
            if asset.map({ captureFileURL($0.url) }) == target {
                pause()
                // A pending video decode must not republish the deleted capture's image.
                generation += 1
                asset = nil; scene = nil; frameIndex = 0
                image = nil; depthImage = nil; edges = []
                meshCells = 0; triangleCount = 0; depthSize = "なし"
                hit = nil; hitPixel = nil; hitMessage = nil; decodeError = nil
                layer = .image; rasterQuarterTurns = 0
                trimStart = 0; trimEnd = 1
                revision = UUID()
                status = "収録ファイルを削除しました"
            }
            recentFiles.removeAll { captureFileURL($0) == target }
        } catch { report(error) }
    }
    func openDemo(movie: Bool) async {
        guard let url = Bundle.main.url(forResource: "Room", withExtension: movie ? "mov" : "heic") else {
            error = "デモファイルがアプリに見つかりません. "; return
        }
        await open(url)
    }
    func importFile(_ url: URL) async {
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try AppleSpatialAsset.boundedData(url)
            let destination = try captureDirectory().appendingPathComponent(
                UUID().uuidString.prefix(8) + "-" + url.lastPathComponent)
            // Validate the exact bytes being copied, even if the source changes while importing.
            switch url.pathExtension.lowercased() {
            case "ssps": _ = try SpatialDocument(data: data)
            case "heic", "heif": _ = try SpatialMedia(data: data, kind: .heif)
            case "mov": _ = try SpatialMedia(data: data, kind: .quickTime)
            default: throw AppleMediaError.unsupported("HEIC, HEIF, MOV, SSPS ファイルを選択してください. ")
            }
            try data.write(to: destination, options: .atomic)
            await open(destination)
            refreshFiles()
        } catch { report(error) }
    }
    func open(_ url: URL) async {
        pause(); isBusy = true
        defer { isBusy = false }
        do {
            let loaded = try AppleSpatialAsset(url: url)
            let loadedScene = try loaded.document.scene()
            asset = loaded; scene = loadedScene; frameIndex = 0
            rasterQuarterTurns = InspectorPresentation.initialQuarterTurns(gravity: loaded.document.gravity)
            image = nil; depthImage = nil; hit = nil; hitPixel = nil; hitMessage = nil; error = nil; decodeError = nil
            trimStart = 0; trimEnd = loaded.cameras.count
            #if os(macOS)
            if section != .inspector { section = .editor }
            #else
            section = .inspector
            #endif
            status = "SSPS と binding の検証が完了しました"
            await selectFrame(0)
        } catch { report(error) }
    }
    func selectFrame(_ index: Int) async {
        guard let asset, let scene, asset.cameras.indices.contains(index) else { return }
        generation += 1
        let request = generation
        frameIndex = index
        image = nil; decodeError = nil; hitPixel = nil
        do {
            try scene.seek(to: asset.cameras[index].timestampNanoseconds)
            let chunks = try scene.meshes()
            meshCells = chunks.count; triangleCount = chunks.reduce(0) { $0 + $1.triangles.count }
            edges = try projectedEdges(chunks, camera: asset.cameras[index])
            if let hit, try scene.isVisible(hit.position, toleranceMeters: 0.01),
               let pixel = try? asset.cameras[index].project(hit.position) {
                hitPixel = CGPoint(x: pixel.x, y: pixel.y)
            }
            if let depth = try scene.depth() {
                depthSize = "\(depth.width) × \(depth.height)"
                depthImage = Self.depthPreview(depth)
            } else { depthSize = "なし"; depthImage = nil }
            revision = UUID()
        } catch { report(error); pause(); return }
        do {
            let decoded = try await asset.image(at: index)
            guard generation == request else { return }
            image = decoded
        } catch {
            guard generation == request else { return }
            decodeError = "映像を表示できません. 空間データは検査できます. \n" + error.localizedDescription
        }
    }
    func scrub(to index: Int) {
        pause()
        Task { await selectFrame(index) }
    }
    func togglePlayback() {
        if isPlaying { pause(); return }
        guard frameCount > 1 else { return }
        isPlaying = true
        let playback = UUID()
        playbackID = playback
        playbackTask = Task { [weak self] in
            guard let self, !Task.isCancelled, playbackID == playback else { return }
            if frameIndex == frameCount - 1 { await selectFrame(0) }
            while !Task.isCancelled, isPlaying, let asset, frameIndex + 1 < frameCount {
                let current = frameIndex
                let interval = asset.cameras[current + 1].timestampNanoseconds - asset.cameras[current].timestampNanoseconds
                let start = ContinuousClock.now
                await selectFrame(current + 1)
                do { try await Task.sleep(until: start.advanced(by: .nanoseconds(Int64(clamping: interval))), clock: .continuous) }
                catch { break }
            }
            if playbackID == playback { isPlaying = false; playbackTask = nil }
        }
    }
    func pause() {
        playbackID = UUID(); playbackTask?.cancel(); playbackTask = nil; isPlaying = false
    }
    func pick(pixel: CGPoint) {
        guard let scene else { return }
        do {
            hit = try scene.raycast(pixel: SIMD2(Double(pixel.x), Double(pixel.y)))
            hitPixel = hit == nil ? nil : pixel
            hitMessage = hit == nil ? "この位置にメッシュはありません" : "面にマーカーを配置しました"
            revision = UUID()
        } catch { report(error) }
    }
    func trim() async {
        guard let asset, trimStart >= 0, trimEnd <= frameCount, trimStart < trimEnd else { return }
        pause(); isBusy = true
        defer { isBusy = false }
        do {
            let start = asset.cameras[trimStart].timestampNanoseconds
            let end = trimEnd == frameCount ? asset.document.durationNanoseconds : asset.cameras[trimEnd].timestampNanoseconds
            let url = try Self.newRecordingURL(extension: asset.media == nil ? "ssps" : "mov")
            if asset.media == nil { try asset.document.trim(from: start, to: end).write(to: url, options: .atomic) }
            else { try await SpatialMovieTrimmer.trim(asset, from: start, to: end, destination: url) }
            refreshFiles(); await open(url)
            status = "選択範囲を新しいファイルに保存しました"
        } catch { report(error) }
    }
    func report(_ error: Error) {
        if let binding = error as? BindingError {
            self.error = binding.diagnostics.map {
                "\($0.domain) · \($0.code)\n\($0.message)\nfile offset: \($0.fileOffset)"
            }.joined(separator: "\n\n")
        } else { self.error = error.localizedDescription }
    }
    private func projectedEdges(_ chunks: [MeshChunk], camera: Camera) throws -> [ProjectedEdge] {
        var result: [ProjectedEdge] = []
        // This is only a display budget. Full geometry remains available for 3D and C raycasting.
        let stride = max(1, triangleCount / 1500)
        var index = 0
        for chunk in chunks {
            let positions = try chunk.positions()
            for triangle in chunk.triangles {
                defer { index += 1 }
                guard index.isMultiple(of: stride) else { continue }
                let vertices = [triangle.x, triangle.y, triangle.z].map { positions[Int($0)] }
                let projected = vertices.compactMap { try? camera.project($0) }
                guard projected.count == 3,
                      projected.allSatisfy({ abs($0.x) < 100_000 && abs($0.y) < 100_000 }) else { continue }
                for corner in 0..<3 {
                    let a = projected[corner], b = projected[(corner + 1) % 3]
                    result.append(.init(id: result.count, a: CGPoint(x: a.x, y: a.y), b: CGPoint(x: b.x, y: b.y)))
                }
            }
        }
        return result
    }
    private static func depthPreview(_ depth: DepthSample) -> CGImage? {
        var rgba: [UInt8] = []
        rgba.reserveCapacity(depth.millimeters.count * 4)
        for (i, mm) in depth.millimeters.enumerated() {
            guard mm != 0, depth.confidence[i] != 0 else { rgba += [12, 18, 26, 255]; continue }
            let t = min(1, Double(mm) / 5000)
            let confidence = 0.35 + Double(depth.confidence[i]) * 0.65 / 3
            rgba += [UInt8(255 * (1 - t) * confidence), UInt8(210 * confidence),
                     UInt8(255 * t * confidence), 255]
        }
        guard let provider = CGDataProvider(data: Data(rgba) as CFData) else { return nil }
        return CGImage(width: Int(depth.width), height: Int(depth.height), bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: Int(depth.width) * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
#endif
