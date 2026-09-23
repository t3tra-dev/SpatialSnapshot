#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import SpatialSnapshot

@MainActor
public enum SpatialMovieTrimmer {
    /// Canonical spatial rebase plus RGB decode/re-encode, including non-keyframe boundaries.
    /// Additional audio/video/metadata tracks are rejected rather than silently discarded.
    public static func trim(_ source: AppleSpatialAsset, from start: UInt64, to end: UInt64,
                            destination: URL) async throws {
        guard source.media?.kind == .quickTime else { throw AppleMediaError.unsupported("トリムは MOV に対応します. ") }
        let data = try source.document.trim(from: start, to: end)
        guard end <= UInt64(Int64.max) else {
            throw AppleMediaError.unsupported("この時刻は AVFoundation の符号付きタイムラインで表現できません. ")
        }
        let trimmed = try SpatialDocument(data: data)
        let cameras = try trimmed.cameras()
        guard let first = cameras.first else { throw AppleMediaError.invalid("トリム範囲にフレームがありません. ") }
        let asset = AVURLAsset(url: source.url)
        let tracks = try await asset.load(.tracks)
        guard tracks.count == 2,
              Set(tracks.map(\.trackID)) == Set([Int32(bitPattern: source.media!.mediaID), Int32(bitPattern: source.media!.metadataID)]),
              let track = tracks.first(where: { $0.trackID == Int32(bitPattern: source.media!.mediaID) }) else {
            throw AppleMediaError.unsupported("追加の音声・映像・メタデータトラックを持つ MOV のトリムには対応していません. ")
        }
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ])
        output.alwaysCopiesSampleData = false
        reader.add(output)
        reader.timeRange = CMTimeRange(start: AppleSpatialAsset.time(start), end: AppleSpatialAsset.time(end))
        let writer = try SpatialMovieWriter(width: Int(first.width), height: Int(first.height), realTime: false)
        do {
            guard reader.startReading() else { throw reader.error ?? AppleMediaError.codec("映像を読み出せません. ") }
            var index = 0
            while let sample = output.copyNextSampleBuffer() {
                try Task.checkCancellation()
                if CMSampleBufferGetNumSamples(sample) == 0 { continue }
                guard index < cameras.count, let buffer = CMSampleBufferGetImageBuffer(sample),
                      CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample),
                                    AppleSpatialAsset.time(start + cameras[index].timestampNanoseconds)) == 0 else {
                    throw AppleMediaError.codec("トリムした映像とカメラの時刻が一致しません. ")
                }
                let deadline = Date().addingTimeInterval(15)
                while try !writer.append(buffer, timestampNanoseconds: cameras[index].timestampNanoseconds) {
                    guard Date() < deadline else { throw AppleMediaError.backpressure }
                    try await Task.sleep(for: .milliseconds(10))
                }
                index += 1
            }
            guard reader.status == .completed, index == cameras.count else {
                throw reader.error ?? AppleMediaError.codec("トリムした映像のフレーム数が一致しません. ")
            }
            try await writer.finish(spatialData: data, to: destination)
        } catch {
            reader.cancelReading(); writer.cancel(); throw error
        }
    }
}
#endif
