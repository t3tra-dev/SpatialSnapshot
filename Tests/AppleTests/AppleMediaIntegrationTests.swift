#if canImport(AVFoundation)
import XCTest
import AVFoundation
import CoreGraphics
import CoreImage
import SpatialSnapshot
import SpatialSnapshotAppleMedia

/// Opt in on a machine with access to ImageIO/VideoToolbox services.
/// Also produces the Lab's small, playable, deterministic (before codec compression) sample assets.
final class AppleMediaIntegrationTests: XCTestCase {
    @MainActor
    func testHEIFMovieDecodeStripAndNonKeyframeTrim() async throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Set SPATIALSNAPSHOT_MEDIA_TESTS=1 with access to Apple media services.")
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialMediaTests-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let faces = Self.room()
        let chunks = try MeshChunk.partition(faces)
        let camera = Self.camera(0)
        let geometry = chunks.map { GeometryUpdate.put($0) }
        let baseWriter = try SpatialWriter(kind: .still)
        try baseWriter.append(camera: camera, geometry: geometry)
        let base = try SpatialDocument(data: baseWriter.finish(durationNanoseconds: 0))
        let scene = try base.scene()
        var mm = [UInt16](repeating: 0, count: 80 * 60)
        var confidence = [UInt8](repeating: 0, count: mm.count)
        for y in 0..<60 {
            for x in 0..<80 {
                if let hit = try scene.raycast(pixel: SIMD2(Double(x) * 8, Double(y) * 8)) {
                    mm[y * 80 + x] = UInt16((hit.position.z * 1000).rounded())
                    confidence[y * 80 + x] = 3
                }
            }
        }
        let depth = try DepthSample(timestampNanoseconds: 0, width: 80, height: 60,
            fx: camera.fx / 8, fy: camera.fy / 8, cx: camera.cx / 8, cy: camera.cy / 8,
            millimeters: mm, confidence: confidence)
        let stillWriter = try SpatialWriter(kind: .still)
        try stillWriter.append(camera: camera, geometry: geometry, depth: depth)
        let stillData = try stillWriter.finish(durationNanoseconds: 0)
        let stillURL = directory.appendingPathComponent("Room.heic")
        let firstImage = try Self.render(camera: camera, faces: faces)
        try SpatialHEIFEncoder.encode(image: firstImage, spatialData: stillData).write(to: stillURL)
        let still = try AppleSpatialAsset(url: stillURL)
        XCTAssertEqual(still.spatialData, stillData)
        let decodedStill = try await still.image(at: 0)
        XCTAssertEqual(decodedStill?.width, 640)
        XCTAssertLessThan(try Self.meanPixelError(XCTUnwrap(decodedStill), firstImage), 12)
        XCTAssertNotNil(try still.document.scene().raycast(pixel: SIMD2(320, 240)))
        print("AppleMedia: HEIF binding and RGB decode passed")

        let videoURL = directory.appendingPathComponent("Room.mov")
        let writer = try SpatialMovieWriter(width: 640, height: 480, framesPerSecond: 15, realTime: false)
        print("AppleMedia: movie encoder started")
        let spatial = try SpatialWriter(kind: .video)
        let count = 45
        for index in 0..<count {
            let camera = Self.camera(index)
            let image = try Self.render(camera: camera, faces: faces)
            let buffer = try Self.pixelBuffer(image: image)
            let deadline = Date().addingTimeInterval(15)
            while try !writer.append(buffer, timestampNanoseconds: camera.timestampNanoseconds) {
                guard Date() < deadline else { throw AppleMediaError.backpressure }
                try await Task.sleep(for: .milliseconds(10))
            }
            try spatial.append(camera: camera, geometry: index == 0 ? geometry : [])
        }
        let movieData = try spatial.finish(durationNanoseconds: 3_000_000_000)
        print("AppleMedia: 45 RGB and CAMERA frames accepted")
        do { try await writer.finish(spatialData: movieData, to: videoURL) }
        catch { print("AppleMedia: movie finish failed: \(error)"); throw error }
        print("AppleMedia: movie binding saved")
        let movie = try AppleSpatialAsset(url: videoURL)
        XCTAssertEqual(movie.spatialData, movieData)
        XCTAssertEqual(movie.cameras.count, count)
        // Exercise playback, repeated frames, backwards seeks and non-keyframe jumps.
        // Run with OS_ACTIVITY_DT_MODE=YES to expose Xcode's CoreMedia diagnostics too.
        for index in Array(0..<count) + [44, 7, 35, 0, 22] {
            let image = try await movie.image(at: index)
            XCTAssertEqual(image?.height, 480)
            XCTAssertLessThan(try Self.meanPixelError(XCTUnwrap(image),
                Self.render(camera: Self.camera(index), faces: faces)), 12)
        }
        let strippedURL = directory.appendingPathComponent("plain.mov")
        try movie.exportWithoutSpatialMetadata(to: strippedURL)
        let plainAsset = AVURLAsset(url: strippedURL)
        let plainTracks = try await plainAsset.loadTracks(withMediaType: .video)
        XCTAssertEqual(plainTracks.count, 1)
        let trimmedURL = directory.appendingPathComponent("trimmed.mov")
        print("AppleMedia: movie RGB decode and stripping passed")
        try await SpatialMovieTrimmer.trim(movie, from: movie.cameras[7].timestampNanoseconds,
            to: movie.cameras[31].timestampNanoseconds, destination: trimmedURL)
        let trimmed = try AppleSpatialAsset(url: trimmedURL)
        XCTAssertEqual(trimmed.cameras.count, 24)
        XCTAssertEqual(trimmed.cameras[0].translation, .zero)
        XCTAssertEqual(trimmed.cameras[0].timestampNanoseconds, 0)
        XCTAssertNotEqual(trimmed.document.streamID, movie.document.streamID)
        let trimImage = try await trimmed.image(at: 0)
        XCTAssertEqual(trimImage?.width, 640)
        XCTAssertLessThan(try Self.meanPixelError(XCTUnwrap(trimImage),
            Self.render(camera: Self.camera(7), faces: faces)), 16)
        // Assets are written only after all independent decode/binding checks above succeeded.
        if let path = ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_DEMO_OUTPUT"] {
            let output = URL(fileURLWithPath: path, isDirectory: true)
            try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
            for url in [stillURL, videoURL] {
                try Data(contentsOf: url).write(to: output.appendingPathComponent(url.lastPathComponent), options: .atomic)
            }
        }
    }

    @MainActor
    func testCameraPixelFormatIrregularTimestampsAndAtomicFailure() async throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Set SPATIALSNAPSHOT_MEDIA_TESTS=1 with access to Apple media services.")
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialYUV-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let rgb = try Self.pixelBuffer(image: Self.render(camera: Self.camera(0), faces: Self.room()))
        var buffer: CVPixelBuffer?
        XCTAssertEqual(CVPixelBufferCreate(kCFAllocatorDefault, 640, 480,
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary,
            &buffer), kCVReturnSuccess)
        let yuv = try XCTUnwrap(buffer)
        CIContext().render(CIImage(cvPixelBuffer: rgb), to: yuv)
        let movie = try SpatialMovieWriter(width: 640, height: 480, realTime: false)
        XCTAssertThrowsError(try movie.append(yuv, timestampNanoseconds: 1))
        let spatial = try SpatialWriter(kind: .video)
        for timestamp: UInt64 in [0, 83_333_331, 199_456_789] {
            let deadline = Date().addingTimeInterval(15)
            while try !movie.append(yuv, timestampNanoseconds: timestamp) {
                guard Date() < deadline else { throw AppleMediaError.backpressure }
                try await Task.sleep(for: .milliseconds(10))
            }
            var camera = Self.camera(0); camera.timestampNanoseconds = timestamp
            try spatial.append(camera: camera)
        }
        XCTAssertThrowsError(try movie.append(yuv, timestampNanoseconds: 199_456_789))
        let url = directory.appendingPathComponent("yuv.mov")
        try await movie.finish(spatialData: spatial.finish(durationNanoseconds: 280_000_000), to: url)
        let decoded = try AppleSpatialAsset(url: url)
        XCTAssertEqual(decoded.cameras.map(\.timestampNanoseconds), [0, 83_333_331, 199_456_789])
        let image = try await decoded.image(at: 2)
        XCTAssertLessThan(try Self.meanPixelError(XCTUnwrap(image),
            Self.render(camera: Self.camera(0), faces: Self.room())), 20)

        let invalid = try SpatialMovieWriter(width: 640, height: 480, realTime: false)
        while try !invalid.append(yuv, timestampNanoseconds: 0) { try await Task.sleep(for: .milliseconds(10)) }
        let wrong = try SpatialWriter(kind: .video)
        try wrong.append(camera: Camera(width: 64, height: 48, fx: 52, fy: 52, cx: 31.5, cy: 23.5))
        let destination = directory.appendingPathComponent("must-not-publish.mov")
        let sentinel = Data("existing output".utf8)
        try sentinel.write(to: destination)
        do {
            try await invalid.finish(spatialData: wrong.finish(durationNanoseconds: 100_000_000), to: destination)
            XCTFail("Mismatching raster must fail")
        } catch { XCTAssertEqual(try Data(contentsOf: destination), sentinel) }
    }

    static func meanPixelError(_ a: CGImage, _ b: CGImage) throws -> Double {
        let first = try pixelBuffer(image: a), second = try pixelBuffer(image: b)
        guard a.width == b.width, a.height == b.height else { return .infinity }
        CVPixelBufferLockBaseAddress(first, .readOnly); CVPixelBufferLockBaseAddress(second, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(first, .readOnly); CVPixelBufferUnlockBaseAddress(second, .readOnly) }
        var total: Double = 0
        for y in 0..<a.height {
            let left = CVPixelBufferGetBaseAddress(first)!.advanced(by: y * CVPixelBufferGetBytesPerRow(first)).assumingMemoryBound(to: UInt8.self)
            let right = CVPixelBufferGetBaseAddress(second)!.advanced(by: y * CVPixelBufferGetBytesPerRow(second)).assumingMemoryBound(to: UInt8.self)
            for x in 0..<a.width {
                for channel in 0..<3 { total += Double(abs(Int(left[x * 4 + channel]) - Int(right[x * 4 + channel]))) }
            }
        }
        return total / Double(a.width * a.height * 3)
    }

    static func camera(_ index: Int) -> Camera {
        Camera(timestampNanoseconds: UInt64(index) * 1_000_000_000 / 15,
            width: 640, height: 480, fx: 520, fy: 520, cx: 319.5, cy: 239.5,
            translation: SIMD3(Float(index) / 180, 0, 0))
    }
    static func room() -> [SceneTriangle] {
        var triangles: [SceneTriangle] = []
        func quad(_ a: SIMD3<Double>, _ b: SIMD3<Double>, _ c: SIMD3<Double>, _ d: SIMD3<Double>,
                  _ classification: SurfaceClassification) {
            triangles += [SceneTriangle(a, b, c, classification: classification),
                          SceneTriangle(a, c, d, classification: classification)]
        }
        // Winding points towards the camera. A wall, a gridded floor, a table and its front.
        quad(SIMD3(-2, -1.5, 3), SIMD3(-2, 0.9, 3), SIMD3(2, 0.9, 3), SIMD3(2, -1.5, 3), .wall)
        for x in -4..<4 {
            for z in 1..<6 {
                let a = Double(x) * 0.5, b = Double(z) * 0.5
                quad(SIMD3(a, 0.9, b), SIMD3(a + 0.5, 0.9, b),
                     SIMD3(a + 0.5, 0.9, b + 0.5), SIMD3(a, 0.9, b + 0.5), .floor)
            }
        }
        quad(SIMD3(-0.5, 0.25, 1.4), SIMD3(0.55, 0.25, 1.4), SIMD3(0.55, 0.25, 2.1), SIMD3(-0.5, 0.25, 2.1), .table)
        quad(SIMD3(-0.5, 0.25, 1.4), SIMD3(-0.5, 0.38, 1.4), SIMD3(0.55, 0.38, 1.4), SIMD3(0.55, 0.25, 1.4), .table)
        return triangles
    }
    static func render(camera: Camera, faces: [SceneTriangle]) throws -> CGImage {
        let space = CGColorSpaceCreateDeviceRGB()
        guard let context = CGContext(data: nil, width: 640, height: 480, bitsPerComponent: 8,
            bytesPerRow: 640 * 4, space: space,
            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else {
            throw AppleMediaError.codec("Demo drawing context unavailable")
        }
        context.translateBy(x: 0, y: 480); context.scaleBy(x: 1, y: -1)
        context.setFillColor(CGColor(red: 0.04, green: 0.07, blue: 0.12, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: 640, height: 480))
        for face in faces.sorted(by: { ($0.a.z + $0.b.z + $0.c.z) > ($1.a.z + $1.b.z + $1.c.z) }) {
            let points = try [face.a, face.b, face.c].map { try camera.project($0) }
            context.beginPath()
            context.move(to: CGPoint(x: points[0].x + 0.5, y: points[0].y + 0.5))
            for point in points.dropFirst() { context.addLine(to: CGPoint(x: point.x + 0.5, y: point.y + 0.5)) }
            context.closePath()
            let color: CGColor
            switch face.classification {
            case .table: color = CGColor(red: 0.96, green: 0.48, blue: 0.18, alpha: 1)
            case .floor: color = CGColor(red: 0.07, green: 0.29, blue: 0.33, alpha: 1)
            default: color = CGColor(red: 0.12, green: 0.19, blue: 0.3, alpha: 1)
            }
            context.setFillColor(color)
            context.setStrokeColor(CGColor(red: 0.4, green: 0.65, blue: 0.7, alpha: 0.5))
            context.setLineWidth(1)
            context.drawPath(using: .fillStroke)
        }
        guard let image = context.makeImage() else { throw AppleMediaError.codec("Demo image unavailable") }
        return image
    }
    static func pixelBuffer(image: CGImage) throws -> CVPixelBuffer {
        var output: CVPixelBuffer?
        guard CVPixelBufferCreate(kCFAllocatorDefault, image.width, image.height, kCVPixelFormatType_32BGRA,
            [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &output) == kCVReturnSuccess,
              let buffer = output else { throw AppleMediaError.codec("Pixel buffer unavailable") }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: image.width, height: image.height,
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
        context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        return buffer
    }
}
#endif
