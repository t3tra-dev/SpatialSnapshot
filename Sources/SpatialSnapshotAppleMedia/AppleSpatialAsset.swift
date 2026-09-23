#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import ImageIO
import SpatialSnapshot

/// Owns the validated spatial stream. All handle access is serialized on the main actor.
/// Opening metadata does not require a working RGB decoder.
@MainActor
public final class AppleSpatialAsset {
    public let url: URL
    public let document: SpatialDocument
    public let media: SpatialMedia?
    public let cameras: [Camera]
    public let spatialData: Data
    private var imageGenerator: AVAssetImageGenerator?
    public static let maximumFileBytes = 256 * 1024 * 1024

    public init(url: URL, limits: ResourceLimits = .init()) throws {
        self.url = url
        switch url.pathExtension.lowercased() {
        case "ssps":
            spatialData = try Self.boundedData(url)
            document = try SpatialDocument(data: spatialData, limits: limits)
            media = nil
        case "heic", "heif", "mov":
            let kind: ContainerKind = url.pathExtension.lowercased() == "mov" ? .quickTime : .heif
            let container = try SpatialMedia(contentsOf: url, kind: kind, limits: limits)
            media = container
            document = container.document
            spatialData = container.sspsData
        default: throw AppleMediaError.unsupported("HEIC, HEIF, MOV, SSPS ファイルを選択してください. ")
        }
        cameras = try document.cameras()
    }

    public static func boundedData(_ url: URL) throws -> Data {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let size = try file.seekToEnd()
        guard size <= maximumFileBytes else {
            throw AppleMediaError.unsupported("この操作は 256 MiB 以下のファイルに対応しています. ")
        }
        try file.seek(toOffset: 0)
        let data = try file.read(upToCount: maximumFileBytes + 1) ?? Data()
        guard data.count <= maximumFileBytes else { throw AppleMediaError.invalid("ファイルのサイズが変更されました. ") }
        return data
    }

    /// Decodes the exact camera sample, never an interpolated camera/image pair.
    public func image(at index: Int) async throws -> CGImage? {
        guard cameras.indices.contains(index) else { throw AppleMediaError.invalid("フレーム範囲外です. ") }
        guard let media else { return nil }
        let image: CGImage
        if media.kind == .heif {
            guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let decoded = CGImageSourceCreateImageAtIndex(source, CGImageSourceGetPrimaryImageIndex(source), nil) else {
                throw AppleMediaError.codec("HEIF の画像をデコードできません. 空間データは読み込み済みです. ")
            }
            image = decoded
        } else {
            guard document.durationNanoseconds <= UInt64(Int64.max),
                  cameras[index].timestampNanoseconds <= UInt64(Int64.max) else {
                throw AppleMediaError.unsupported("この時刻は AVFoundation の符号付きタイムラインで表現できません. ")
            }
            if imageGenerator == nil {
                let asset = AVURLAsset(url: url)
                let tracks = try await asset.loadTracks(withMediaType: .video)
                guard let track = tracks.first(where: { $0.trackID == Int32(bitPattern: media.mediaID) }) else {
                    throw AppleMediaError.codec("SSPS が参照する映像トラックが見つかりません. ")
                }
                let source: AVAsset
                if tracks.count == 1 {
                    // Keep single-video movies on AVFoundation's direct image path.
                    // Wrapping them in a composition enters MediaToolbox's remaker path,
                    // which emits BufferNotReady / FigExportCommmon diagnostics per seek.
                    source = asset
                } else {
                    // Select exactly the bound track when a host contains other videos.
                    let composition = AVMutableComposition()
                    guard let selected = composition.addMutableTrack(withMediaType: .video, preferredTrackID: track.trackID) else {
                        throw AppleMediaError.codec("映像トラックを開けません. ")
                    }
                    try selected.insertTimeRange(CMTimeRange(start: .zero,
                        duration: Self.time(document.durationNanoseconds)), of: track, at: .zero)
                    source = composition
                }
                let generator = AVAssetImageGenerator(asset: source)
                generator.appliesPreferredTrackTransform = false
                generator.requestedTimeToleranceBefore = .zero
                generator.requestedTimeToleranceAfter = .zero
                imageGenerator = generator
            }
            let expected = Self.time(cameras[index].timestampNanoseconds)
            let result = try await imageGenerator!.image(at: expected)
            guard CMTimeCompare(result.actualTime, expected) == 0 else {
                throw AppleMediaError.codec("要求した時刻とデコードした映像の時刻が一致しません. ")
            }
            image = result.image
        }
        guard image.width == Int(media.width), image.height == Int(media.height) else {
            throw AppleMediaError.codec("映像の画素数と CAMERA の画素数が一致しません. ")
        }
        return image
    }

    public func exportSpatialData(to destination: URL) throws {
        try spatialData.write(to: destination, options: .atomic)
    }

    public func exportWithoutSpatialMetadata(to destination: URL) throws {
        guard let media else { throw AppleMediaError.unsupported("SSPS 単体には映像がありません. ") }
        let data = try SpatialMedia.removingSpatialMetadata(from: Self.boundedData(url), kind: media.kind)
        try data.write(to: destination, options: .atomic)
    }

    static func time(_ nanoseconds: UInt64) -> CMTime {
        // Callers validate against AVFoundation's signed timeline before reaching here.
        CMTime(value: Int64(clamping: nanoseconds), timescale: 1_000_000_000)
    }
}
#endif
