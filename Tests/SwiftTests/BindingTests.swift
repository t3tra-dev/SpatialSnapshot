import Foundation
import XCTest
@testable import SpatialSnapshot

final class BindingTests: XCTestCase {
    private func url(_ name: String) -> URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("Fixtures/bindings/\(name)")
    }
    func testHEIFBindingAndRemoval() throws {
        let data = try Data(contentsOf: url("heif-minimal.heif"))
        let original = try SpatialMedia(data: data, kind: .heif)
        let host = try SpatialMedia.removingSpatialMetadata(from: data, kind: .heif)
        XCTAssertThrowsError(try SpatialMedia(data: host, kind: .heif))
        let rebound = try SpatialMedia(data: original.document.binding(to: host, kind: .heif), kind: .heif)
        XCTAssertEqual(rebound.sspsData, original.sspsData)
        XCTAssertEqual(rebound.document.streamID, original.document.streamID)
        XCTAssertEqual(try rebound.document.scene().meshes().count, 1)
        XCTAssertEqual(try rebound.document.scene().depth()?.millimeters, [1000])
        XCTAssertEqual(try rebound.checkpoint(at: 0).sampleIndex, 0)
    }
    func testQuickTimePresentationOrderAndFileIO() throws {
        let media = try SpatialMedia(contentsOf: url("qt-bframes.mov"), kind: .quickTime)
        XCTAssertEqual(media.sampleCount, 3)
        XCTAssertEqual(try media.sample(at: 1).mediaDecodeIndex, 2)
        XCTAssertEqual(try media.sample(at: 1).timestampNanoseconds, 1_000_000_000)
        XCTAssertEqual(try media.sample(at: 2).durationNanoseconds, 1_000_000_000)
        XCTAssertThrowsError(try media.sample(at: 3))
        let host = try Data(contentsOf: url("qt-bframes.mov"))
        let rebound = try SpatialMedia(data: media.document.binding(to: host, kind: .quickTime), kind: .quickTime)
        XCTAssertEqual(rebound.sspsData, media.sspsData)
    }
    func testIndependentBindingDiagnosticsAndNoPartialDocument() throws {
        let data = try Data(contentsOf: url("qt-independent-errors.mov"))
        XCTAssertThrowsError(try SpatialMedia(data: data, kind: .quickTime)) { error in
            let error = error as? BindingError
            XCTAssertEqual(error?.code, -2)
            XCTAssertGreaterThanOrEqual(error?.diagnostics.count ?? 0, 4)
            XCTAssertTrue(error?.diagnostics.allSatisfy { $0.domain == .bindingMalformed } ?? false)
        }
        let badCRC = try Data(contentsOf: url("heif-crc.heif"))
        XCTAssertThrowsError(try SpatialMedia(data: badCRC, kind: .heif)) { error in
            XCTAssertEqual((error as? BindingError)?.diagnostics.first?.domain, .sspsMalformed)
        }
    }
    func testEncodedVideoMux() throws {
        let source = try SpatialMedia(contentsOf: url("qt-minimal.mov"), kind: .quickTime)
        var entry = Data(repeating: 0, count: 86)
        func u16(_ offset: Int, _ value: UInt16) { entry[offset] = UInt8(value >> 8); entry[offset+1] = UInt8(truncatingIfNeeded: value) }
        entry[3] = 86; entry.replaceSubrange(4..<8, with: Data("raw ".utf8))
        u16(14, 1); u16(32, 4); u16(34, 3); u16(36, 72); u16(40, 72); u16(48, 1); u16(82, 24); u16(84, 65535)
        let samples = [0, 2, 1].map { SpatialDocument.EncodedVideoSample(data: Data(repeating: 200, count: 36),
            presentationTimestampNanoseconds: UInt64($0) * 1_000_000_000, decodeDurationNanoseconds: 1_000_000_000, isSync: true) }
        let data = try source.document.quickTime(visualSampleEntry: entry, samples: samples)
        let media = try SpatialMedia(data: data, kind: .quickTime)
        XCTAssertEqual(media.sspsData, source.sspsData)
        XCTAssertEqual(try media.sample(at: 1).mediaDecodeIndex, 2)
        let writer = try SpatialWriter(kind: .video)
        try writer.append(camera: Camera(width: 4, height: 3, fx: 2, fy: 3, cx: 1.5, cy: 1))
        try writer.append(camera: Camera(timestampNanoseconds: 5_000_000_000, width: 4, height: 3, fx: 2, fy: 3, cx: 1.5, cy: 1))
        let longInterval = try SpatialDocument(data: writer.finish(durationNanoseconds: 6_000_000_000))
        let sparse = [UInt64(0), 5_000_000_000].map { SpatialDocument.EncodedVideoSample(data: Data(repeating: 0, count: 36),
            presentationTimestampNanoseconds: $0, decodeDurationNanoseconds: 3_000_000_000, isSync: true) }
        XCTAssertThrowsError(try longInterval.quickTime(visualSampleEntry: entry, samples: sparse)) { error in
            XCTAssertEqual((error as? BindingError)?.diagnostics.first?.domain, .bindingUnrepresentable)
        }
    }
    func testCanonicalMovieTrim() throws {
        let source = try Data(contentsOf: url("qt-bframes.mov"))
        let before = try SpatialMedia(data: source, kind: .quickTime)
        let data = try SpatialMedia.trimmingQuickTime(source, from: 1_000_000_000, to: 3_000_000_000)
        let after = try SpatialMedia(data: data, kind: .quickTime)
        XCTAssertEqual(after.sampleCount, 2)
        XCTAssertEqual(after.document.durationNanoseconds, 2_000_000_000)
        XCTAssertNotEqual(after.document.streamID, before.document.streamID)
        XCTAssertEqual(try after.document.cameras().first?.translation, .zero)
        XCTAssertEqual(try after.sample(at: 0).timestampNanoseconds, 0)
        XCTAssertEqual(try after.sample(at: 1).timestampNanoseconds, 1_000_000_000)
        XCTAssertThrowsError(try SpatialMedia.trimmingQuickTime(source, from: 500_000_000, to: 3_000_000_000))
        let interframe = try Data(contentsOf: url("qt-interframe.mov"))
        XCTAssertThrowsError(try SpatialMedia.trimmingQuickTime(interframe, from: 1_000_000_000, to: 3_000_000_000)) { error in
            XCTAssertEqual((error as? BindingError)?.diagnostics.first?.domain, .videoDecoderUnavailable)
        }
    }
}
