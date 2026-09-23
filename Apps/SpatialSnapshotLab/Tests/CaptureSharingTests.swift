#if canImport(CoreTransferable) && canImport(AVFoundation)
import XCTest
import CoreTransferable
import UniformTypeIdentifiers
import SpatialSnapshot
import SpatialSnapshotAppleMedia

final class CaptureSharingTests: XCTestCase {
    private var resources: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Resources", isDirectory: true)
    }
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialSnapshot-share-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    @MainActor
    func testTransferExportsOneOriginalMediaFileWithSSPS() async throws {
        guard #available(macOS 15.2, iOS 18.2, *) else {
            throw XCTSkip("Inspecting a Transferable export requires macOS 15.2 / iOS 18.2.")
        }
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        for (ext, type) in [("heic", UTType.heic), ("heif", .heif), ("mov", .quickTimeMovie)] {
            let source = resources.appendingPathComponent("Room." + (ext == "heif" ? "heic" : ext))
            let input = root.appendingPathComponent("Capture." + ext)
            try FileManager.default.copyItem(at: source, to: input)
            let asset = try AppleSpatialAsset(url: input)
            let shared = try XCTUnwrap(SpatialCaptureShare(asset: asset))
            XCTAssertEqual(shared.exportedContentTypes(), [type])
            let destination = root.appendingPathComponent("Shared-" + ext, isDirectory: true)
            try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)

            let exported = try await shared.export(to: destination, contentType: type)

            XCTAssertNotEqual(exported, input)
            XCTAssertEqual(exported.lastPathComponent, input.lastPathComponent)
            XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: destination.path).count, 1)
            XCTAssertEqual(try Data(contentsOf: exported), try Data(contentsOf: input))
            let reopened = try AppleSpatialAsset(url: exported)
            XCTAssertNotNil(reopened.media)
            XCTAssertEqual(reopened.spatialData, asset.spatialData)
            XCTAssertEqual(reopened.document.streamID, asset.document.streamID)
            XCTAssertEqual(reopened.cameras.count, asset.cameras.count)
        }
    }

    @MainActor
    func testStandaloneSSPSDoesNotOfferMediaSharing() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let media = try AppleSpatialAsset(url: resources.appendingPathComponent("Room.heic"))
        let url = root.appendingPathComponent("SpatialOnly.ssps")
        try media.spatialData.write(to: url)
        let standalone = try AppleSpatialAsset(url: url)
        XCTAssertNil(SpatialCaptureShare(asset: standalone))
    }
}
#endif
