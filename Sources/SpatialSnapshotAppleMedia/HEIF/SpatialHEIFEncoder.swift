#if canImport(ImageIO)
import CoreImage
import ImageIO
import UniformTypeIdentifiers
import SpatialSnapshot

@MainActor
public enum SpatialHEIFEncoder {
    /// Encodes the supplied raster without EXIF rotation, then delegates the binding to the C core.
    /// The core removes ImageIO's redundant irot=0 association from this unbound host.
    public static func encode(image: CGImage, spatialData: Data, quality: Double = 0.9) throws -> Data {
        let document = try SpatialDocument(data: spatialData)
        guard document.kind == .still, let camera = try document.cameras().first,
              image.width == Int(camera.width), image.height == Int(camera.height) else {
            throw AppleMediaError.invalid("静止画の画素数と STILL CAMERA を一致させてください. ")
        }
        let bytes = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(bytes, UTType.heic.identifier as CFString, 1, nil) else {
            throw AppleMediaError.codec("この端末は HEIC エンコードに対応していません. ")
        }
        CGImageDestinationAddImage(destination, image, [
            kCGImageDestinationLossyCompressionQuality: min(1, max(0, quality))
        ] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else {
            throw AppleMediaError.codec("HEIC エンコードに失敗しました. ")
        }
        return try document.binding(to: bytes as Data, kind: .heif)
    }

    public static func image(from buffer: CVPixelBuffer) throws -> CGImage {
        let input = CIImage(cvPixelBuffer: buffer)
        guard let image = CIContext().createCGImage(input, from: input.extent) else {
            throw AppleMediaError.codec("カメラ画像を変換できません. ")
        }
        return image
    }
}
#endif
