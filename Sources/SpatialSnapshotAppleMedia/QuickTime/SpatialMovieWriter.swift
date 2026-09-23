#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import SpatialSnapshot

/// Encodes RGB with AVFoundation and builds the normative MOV with the C binding.
/// No AVFoundation-generated edit lists or time scales are carried into the final file.
@MainActor
public final class SpatialMovieWriter {
    private let directory: URL
    private let temporaryURL: URL
    private let writer: AVAssetWriter
    private let input: AVAssetWriterInput
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor
    private let width: Int
    private let height: Int
    private var timestamps: [UInt64] = []
    private var finished = false
    public var isReadyForMoreMediaData: Bool { input.isReadyForMoreMediaData && !finished }

    public init(width: Int, height: Int, framesPerSecond: Int = 30, realTime: Bool = true) throws {
        guard width > 0, height > 0, width <= 4096, height <= 4096,
              width.isMultiple(of: 2), height.isMultiple(of: 2), (1...60).contains(framesPerSecond) else {
            throw AppleMediaError.unsupported("動画は偶数の画素数, 最大 4096 × 4096, 1〜60 fps に対応します. ")
        }
        self.width = width; self.height = height
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialMovie-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        temporaryURL = directory.appendingPathComponent("encoded.mov")
        writer = try AVAssetWriter(outputURL: temporaryURL, fileType: .mov)
        writer.movieTimeScale = 1_000_000_000
        input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: width, AVVideoHeightKey: height,
            AVVideoCompressionPropertiesKey: [
                AVVideoAllowFrameReorderingKey: false,
                AVVideoExpectedSourceFrameRateKey: framesPerSecond,
                AVVideoMaxKeyFrameIntervalKey: framesPerSecond,
                AVVideoAverageBitRateKey: min(20_000_000, max(500_000, width * height * 5))
            ]
        ])
        input.mediaTimeScale = 1_000_000_000
        input.expectsMediaDataInRealTime = realTime
        input.transform = .identity
        adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ])
        guard writer.canAdd(input) else { throw AppleMediaError.codec("H.264 エンコーダーを構成できません. ") }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? AppleMediaError.codec("動画のエンコードを開始できません. ") }
        writer.startSession(atSourceTime: .zero)
    }

    deinit {
        if FileManager.default.fileExists(atPath: directory.path) { try? FileManager.default.removeItem(at: directory) }
    }

    /// False means backpressure: the caller must also drop the corresponding CAMERA/DEPTH.
    @discardableResult
    public func append(_ buffer: CVPixelBuffer, timestampNanoseconds: UInt64) throws -> Bool {
        guard !finished, writer.status == .writing else {
            throw writer.error ?? AppleMediaError.invalid("動画ライターは停止しています. ")
        }
        guard CVPixelBufferGetWidth(buffer) == width, CVPixelBufferGetHeight(buffer) == height,
              timestampNanoseconds <= UInt64(Int64.max),
              timestamps.last.map({ timestampNanoseconds > $0 && timestampNanoseconds - $0 <= UInt32.max }) ?? (timestampNanoseconds == 0) else {
            throw AppleMediaError.invalid("映像の画素数またはフレーム時刻が不正です. ")
        }
        guard timestamps.count < 1800 else { throw AppleMediaError.unsupported("動画は最大 1800 フレームです. ") }
        guard input.isReadyForMoreMediaData else { return false }
        guard adaptor.append(buffer, withPresentationTime: AppleSpatialAsset.time(timestampNanoseconds)) else {
            throw writer.error ?? AppleMediaError.codec("映像フレームのエンコードに失敗しました. ")
        }
        timestamps.append(timestampNanoseconds)
        return true
    }

    public func cancel() {
        guard !finished else { return }
        finished = true
        writer.cancelWriting()
        try? FileManager.default.removeItem(at: directory)
    }

    public func finish(spatialData: Data, to destination: URL) async throws {
        guard !finished, !timestamps.isEmpty else { throw AppleMediaError.invalid("保存するフレームがありません. ") }
        finished = true
        defer {
            if writer.status == .writing { writer.cancelWriting() }
            if FileManager.default.fileExists(atPath: directory.path) { try? FileManager.default.removeItem(at: directory) }
        }
        let document = try SpatialDocument(data: spatialData)
        try document.validateMediaBinding(width: UInt32(width), height: UInt32(height),
            presentationTimes: timestamps, duration: document.durationNanoseconds)
        guard document.kind == .video, document.durationNanoseconds <= UInt64(Int64.max),
              document.durationNanoseconds > timestamps.last!,
              document.durationNanoseconds - timestamps.last! <= UInt32.max else {
            writer.cancelWriting()
            throw AppleMediaError.invalid("動画の終端時刻が不正です. ")
        }
        writer.endSession(atSourceTime: AppleSpatialAsset.time(document.durationNanoseconds))
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else { throw writer.error ?? AppleMediaError.codec("動画のエンコードを完了できません. ") }
        let asset = AVURLAsset(url: temporaryURL)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw AppleMediaError.codec("符号化済み映像がありません. ")
        }
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
        output.alwaysCopiesSampleData = false
        reader.add(output)
        guard reader.startReading() else { throw reader.error ?? AppleMediaError.codec("符号化済み映像を読み出せません. ") }
        var samples: [SpatialDocument.EncodedVideoSample] = []
        var description: Data?
        var totalBytes = 0
        while let sample = output.copyNextSampleBuffer() {
            // AVAssetReader may emit zero-sample edit/decoder-reset markers.
            // Exact timestamps and total RGB frame count are still checked below.
            if CMSampleBufferGetNumSamples(sample) == 0 { continue }
            let index = samples.count
            guard index < timestamps.count, CMSampleBufferGetNumSamples(sample) == 1,
                  CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample), AppleSpatialAsset.time(timestamps[index])) == 0,
                  let block = CMSampleBufferGetDataBuffer(sample),
                  let format = CMSampleBufferGetFormatDescription(sample) else {
                let time = CMSampleBufferGetPresentationTimeStamp(sample)
                throw AppleMediaError.codec("エンコーダーのフレーム \(index) が CAMERA と一致しません. PTS=\(time.value)/\(time.timescale), 要求=\(timestamps.indices.contains(index) ? timestamps[index] : 0) ns, samples=\(CMSampleBufferGetNumSamples(sample))")
            }
            let entry = try Self.visualSampleEntry(format)
            if let description, description != entry { throw AppleMediaError.unsupported("途中で映像の符号化形式が変化しました. ") }
            description = entry
            let count = CMBlockBufferGetDataLength(block)
            totalBytes += count
            guard totalBytes <= 96 * 1024 * 1024 else { throw AppleMediaError.unsupported("符号化済み映像が 96 MiB を超えました. ") }
            var bytes = Data(count: count)
            let status = bytes.withUnsafeMutableBytes {
                CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: count, destination: $0.baseAddress!)
            }
            guard status == kCMBlockBufferNoErr else { throw AppleMediaError.codec("映像サンプルを読み出せません. ") }
            let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
            let notSync = attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
            let next = index + 1 < timestamps.count ? timestamps[index + 1] : document.durationNanoseconds
            samples.append(.init(data: bytes, presentationTimestampNanoseconds: timestamps[index],
                decodeDurationNanoseconds: UInt32(next - timestamps[index]), isSync: !notSync))
        }
        guard reader.status == .completed, samples.count == timestamps.count, let description else {
            throw reader.error ?? AppleMediaError.codec("符号化済み映像のフレーム数が一致しません. ")
        }
        let movie = try document.quickTime(visualSampleEntry: description, samples: samples)
        try movie.write(to: destination, options: .atomic)
    }

    /// A codec sample entry, not a second BMFF parser. AVFoundation provides the codec configuration.
    private static func visualSampleEntry(_ format: CMFormatDescription) throws -> Data {
        let codec = CMFormatDescriptionGetMediaSubType(format)
        guard codec == kCMVideoCodecType_H264 || codec == kCMVideoCodecType_HEVC else {
            throw AppleMediaError.unsupported("H.264 / HEVC 以外の符号化形式です. ")
        }
        let dimensions = CMVideoFormatDescriptionGetDimensions(format)
        guard dimensions.width > 0, dimensions.height > 0,
              let atoms = CMFormatDescriptionGetExtension(format, extensionKey: kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms) as? [String: Any],
              let config = atoms[codec == kCMVideoCodecType_H264 ? "avcC" : "hvcC"] as? Data else {
            throw AppleMediaError.codec("映像のコーデック構成を取得できません. ")
        }
        var data = Data(repeating: 0, count: 86)
        func set16(_ offset: Int, _ value: UInt16) { data[offset] = UInt8(value >> 8); data[offset + 1] = UInt8(value & 255) }
        func set32(_ offset: Int, _ value: UInt32) {
            for i in 0..<4 { data[offset + i] = UInt8(truncatingIfNeeded: value >> (24 - i * 8)) }
        }
        set32(4, codec); set16(14, 1)
        set16(32, UInt16(dimensions.width)); set16(34, UInt16(dimensions.height))
        set32(36, 72 << 16); set32(40, 72 << 16); set16(48, 1)
        set16(82, 24); set16(84, 65535)
        func atom(_ name: String, _ payload: Data) -> Data {
            var size = UInt32(payload.count + 8).bigEndian
            var result = withUnsafeBytes(of: &size) { Data($0) }
            result.append(contentsOf: name.utf8); result.append(payload); return result
        }
        data.append(atom(codec == kCMVideoCodecType_H264 ? "avcC" : "hvcC", config))
        if let color = atoms["colr"] as? Data { data.append(atom("colr", color)) }
        // We encode a complete, square-pixel raster. Fail if the encoder cropped it.
        let aperture = CMVideoFormatDescriptionGetCleanAperture(format, originIsAtTopLeft: true)
        guard aperture == CGRect(x: 0, y: 0, width: Int(dimensions.width), height: Int(dimensions.height)) else {
            throw AppleMediaError.unsupported("映像エンコーダーが非恒等の clean aperture を生成しました. ")
        }
        set32(0, UInt32(data.count))
        return data
    }
}
#endif
