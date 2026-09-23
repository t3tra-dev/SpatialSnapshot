import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import ImageIO
import QuickLookThumbnailing

// An external media probe, independent of SpatialSnapshotC. Each invocation examines one
// file so the audit driver can enforce a timeout even for malformed host media.
@main struct AppleMediaProbe {
    static func errorInfo(_ error: Error) -> [String: Any] {
        let value = error as NSError
        var result: [String: Any] = ["domain": value.domain, "code": value.code,
                                     "description": value.localizedDescription]
        if let underlying = value.userInfo[NSUnderlyingErrorKey] as? NSError {
            result["underlying"] = errorInfo(underlying)
        }
        return result
    }

    static func image(_ url: URL) -> [String: Any] {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return ["decoded": false, "reason": "CGImageSourceCreateWithURL failed"]
        }
        var result: [String: Any] = ["imageCount": CGImageSourceGetCount(source)]
        if let type = CGImageSourceGetType(source) { result["type"] = type as String }
        let options = [kCGImageSourceShouldCacheImmediately: true] as CFDictionary
        if let image = CGImageSourceCreateImageAtIndex(source, 0, options) {
            result["decoded"] = true
            result["width"] = image.width
            result["height"] = image.height
        } else {
            result["decoded"] = false
            result["status"] = CGImageSourceGetStatusAtIndex(source, 0).rawValue
        }
        return result
    }

    static func video(_ url: URL) async -> [String: Any] {
        let asset = AVURLAsset(url: url)
        var result: [String: Any] = [:]
        do {
            result["isPlayable"] = try await asset.load(.isPlayable)
            let duration = try await asset.load(.duration)
            if duration.isNumeric {
                result["durationValue"] = duration.value
                result["durationTimescale"] = duration.timescale
            }
            let tracks = try await asset.loadTracks(withMediaType: .video)
            result["videoTracks"] = tracks.count
            result["metadataTracks"] = try await asset.loadTracks(withMediaType: .metadata).count
            guard let track = tracks.first else {
                result["decoded"] = false
                return result
            }
            let descriptions = try await track.load(.formatDescriptions)
            result["codecs"] = descriptions.map { description in
                let code = CMFormatDescriptionGetMediaSubType(description)
                return String(bytes: [UInt8(truncatingIfNeeded: code >> 24),
                                      UInt8(truncatingIfNeeded: code >> 16),
                                      UInt8(truncatingIfNeeded: code >> 8),
                                      UInt8(truncatingIfNeeded: code)], encoding: .ascii) ?? "?"
            }
            let reader = try AVAssetReader(asset: asset)
            let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
            ])
            guard reader.canAdd(output) else {
                result["decoded"] = false
                result["reason"] = "reader.canAdd returned false"
                return result
            }
            reader.add(output)
            let started = reader.startReading()
            result["readerStarted"] = started
            var pts: [[String: Any]] = []
            var sampleBuffers: [[String: Any]] = []
            if started {
                while let sample = output.copyNextSampleBuffer() {
                    let time = CMSampleBufferGetPresentationTimeStamp(sample)
                    sampleBuffers.append(["value": time.value, "timescale": time.timescale,
                                          "samples": CMSampleBufferGetNumSamples(sample),
                                          "bytes": CMSampleBufferGetTotalSampleSize(sample),
                                          "hasImageBuffer": CMSampleBufferGetImageBuffer(sample) != nil])
                    if sampleBuffers.count > 128 { reader.cancelReading(); break }
                    guard let image = CMSampleBufferGetImageBuffer(sample) else { continue }
                    pts.append(["value": time.value, "timescale": time.timescale,
                                "width": CVPixelBufferGetWidth(image), "height": CVPixelBufferGetHeight(image)])
                    if pts.count == 64 { reader.cancelReading(); break }
                }
            }
            result["frames"] = pts
            result["sampleBuffers"] = sampleBuffers
            result["readerStatus"] = reader.status.rawValue
            result["decoded"] = !pts.isEmpty && reader.status == .completed
            if let error = reader.error { result["error"] = errorInfo(error) }
            do {
                let generator = AVAssetImageGenerator(asset: asset)
                let generated = try await generator.image(at: .zero)
                result["generatedImage"] = ["decoded": true, "width": generated.image.width,
                                            "height": generated.image.height]
            } catch { result["generatedImage"] = ["decoded": false, "error": errorInfo(error)] }
        } catch {
            result["decoded"] = false
            result["error"] = errorInfo(error)
        }
        return result
    }

    static func quickLook(_ url: URL) async -> [String: Any] {
        let request = QLThumbnailGenerator.Request(fileAt: url, size: CGSize(width: 256, height: 256),
                                                   scale: 1, representationTypes: .thumbnail)
        do {
            let representation = try await QLThumbnailGenerator.shared.generateBestRepresentation(for: request)
            return ["thumbnail": representation.type == .thumbnail,
                    "representationType": representation.type.rawValue,
                    "width": representation.cgImage.width, "height": representation.cgImage.height]
        } catch { return ["thumbnail": false, "error": errorInfo(error)] }
    }

    static func main() async throws {
        guard CommandLine.arguments.count == 3 else { return }
        let url = URL(fileURLWithPath: CommandLine.arguments[2])
        let result: [String: Any]
        switch CommandLine.arguments[1] {
        case "image": result = image(url)
        case "video": result = await video(url)
        case "quicklook": result = await quickLook(url)
        default: return
        }
        let bytes = try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
        print(String(decoding: bytes, as: UTF8.self))
    }
}
