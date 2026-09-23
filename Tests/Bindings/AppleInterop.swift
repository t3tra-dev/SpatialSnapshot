import AVFoundation
import CoreMedia
import CoreVideo
import ImageIO
import Foundation

@main struct AppleInterop {
    static func decodedVideo(_ url: URL) async throws -> [Data] {
        let asset = AVURLAsset(url: url)
        let tracks = try await asset.loadTracks(withMediaType: .video)
        precondition(tracks.count == 1)
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: tracks[0], outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ])
        reader.add(output)
        precondition(reader.startReading(), String(describing: reader.error))
        var frames: [Data] = []
        while let sample = output.copyNextSampleBuffer() {
            guard let image = CMSampleBufferGetImageBuffer(sample) else { continue }
            precondition(CVPixelBufferGetWidth(image) == 64 && CVPixelBufferGetHeight(image) == 48)
            let time = CMSampleBufferGetPresentationTimeStamp(sample)
            precondition(CMTimeCompare(time, CMTime(value: Int64(frames.count) * 100_000_000,
                                                   timescale: 1_000_000_000)) == 0)
            CVPixelBufferLockBaseAddress(image, .readOnly)
            var pixels = Data()
            if let base = CVPixelBufferGetBaseAddress(image) {
                for row in 0..<48 {
                    pixels.append(base.advanced(by: row * CVPixelBufferGetBytesPerRow(image))
                        .assumingMemoryBound(to: UInt8.self), count: 64 * 4)
                }
            }
            CVPixelBufferUnlockBaseAddress(image, .readOnly)
            precondition(pixels.count == 64 * 48 * 4)
            frames.append(pixels)
        }
        precondition(frames.count == 3 && reader.status == .completed, String(describing: reader.error))
        return frames
    }

    static func main() async throws {
        let directory = URL(fileURLWithPath: CommandLine.arguments[1])
        for name in ["bound.heic", "stripped.heic", "grid-bound.heic", "grid-stripped.heic"] {
            let url = directory.appendingPathComponent(name)
            guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { fatalError("ImageIO could not decode \(name)") }
            precondition(image.width == (name.hasPrefix("grid-") ? 128 : 64) && image.height == 64)
        }
        let originalFrames = try await decodedVideo(directory.appendingPathComponent("source.mov"))
        for name in ["bound.mov", "stripped.mov"] {
            let frames = try await decodedVideo(directory.appendingPathComponent(name))
            precondition(frames == originalFrames, "video pixels changed in \(name)")
        }
        let movie = AVURLAsset(url: directory.appendingPathComponent("bound.mov"))
        let video = try await movie.loadTracks(withMediaType: .video)
        let metadata = try await movie.loadTracks(withMediaType: .metadata)
        precondition(video.count == 1 && metadata.count == 1)
        let associated = try await metadata[0].loadAssociatedTracks(ofType: .metadataReferent)
        precondition(associated.count == 1 && associated[0].trackID == video[0].trackID)
        let descriptions = try await metadata[0].load(.formatDescriptions)
        precondition(descriptions.count == 1)
        let identifiers = CMMetadataFormatDescriptionGetIdentifiers(descriptions[0]) as? [String]
        precondition(identifiers == ["mdta/org.spatialsnapshot.ssps.packet-bundle"])
        let reader = try AVAssetReader(asset: movie)
        let output = AVAssetReaderTrackOutput(track: metadata[0], outputSettings: nil)
        reader.add(output)
        precondition(reader.startReading())
        var frames = 0
        var details: [String] = []
        var correctTimes = true
        while let sample = output.copyNextSampleBuffer() {
            // AVAssetReader also emits empty flush/discontinuity marker buffers.
            guard CMSampleBufferGetTotalSampleSize(sample) > 0 else { continue }
            let time = CMSampleBufferGetPresentationTimeStamp(sample)
            correctTimes = correctTimes && CMTimeCompare(time, CMTime(value: Int64(frames) * 100_000_000, timescale: 1_000_000_000)) == 0
            details.append("\(frames): PTS=\(time.value)/\(time.timescale) DTS=\(CMSampleBufferGetDecodeTimeStamp(sample).value), duration=\(CMSampleBufferGetDuration(sample).value), bytes=\(CMSampleBufferGetTotalSampleSize(sample))")
            frames += 1
            if frames > 10 { break }
        }
        precondition(frames == 3 && reader.status == .completed && correctTimes, details.joined(separator: "; "))
        print("ImageIO decode and AVFoundation video pixels/mebx/key/cdsc/timestamps passed")
    }
}
