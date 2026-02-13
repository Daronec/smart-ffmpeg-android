//
//  SmartFfmpegBridge.swift
//  SmartFfmpegBridge
//
//  Swift wrapper for SmartFfmpegBridge
//

import Foundation
import UIKit

/// Swift wrapper for FFmpeg video processing operations
@objc public class SmartFfmpegBridgeSwift: NSObject {
    
    /// Extract thumbnail from video file and convert to UIImage
    ///
    /// - Parameters:
    ///   - videoPath: Absolute path to video file
    ///   - timeMs: Time position in milliseconds
    ///   - width: Target width
    ///   - height: Target height
    /// - Returns: UIImage containing the thumbnail, or nil on error
    @objc public static func extractThumbnailImage(
        fromVideo videoPath: String,
        atTime timeMs: Int64,
        width: Int,
        height: Int
    ) -> UIImage? {
        guard let rgbaData = SmartFfmpegBridge.extractThumbnail(
            fromVideo: videoPath,
            atTime: timeMs,
            width: Int32(width),
            height: Int32(height)
        ) else {
            return nil
        }
        
        return createImage(from: rgbaData, width: width, height: height)
    }
    
    /// Extract thumbnail as raw RGBA data
    ///
    /// - Parameters:
    ///   - videoPath: Absolute path to video file
    ///   - timeMs: Time position in milliseconds
    ///   - width: Target width
    ///   - height: Target height
    /// - Returns: Data containing RGBA pixel data, or nil on error
    @objc public static func extractThumbnailData(
        fromVideo videoPath: String,
        atTime timeMs: Int64,
        width: Int,
        height: Int
    ) -> Data? {
        return SmartFfmpegBridge.extractThumbnail(
            fromVideo: videoPath,
            atTime: timeMs,
            width: Int32(width),
            height: Int32(height)
        )
    }
    
    /// Get video duration in milliseconds
    ///
    /// - Parameter videoPath: Absolute path to video file
    /// - Returns: Duration in milliseconds, or -1 on error
    @objc public static func getVideoDuration(_ videoPath: String) -> Int64 {
        return SmartFfmpegBridge.getVideoDuration(videoPath)
    }
    
    /// Get video metadata
    ///
    /// - Parameter videoPath: Absolute path to video file
    /// - Returns: Dictionary containing metadata (width, height, duration, codec, etc.)
    @objc public static func getVideoMetadata(_ videoPath: String) -> [String: Any]? {
        return SmartFfmpegBridge.getVideoMetadata(videoPath)
    }
    
    /// Get FFmpeg version string
    ///
    /// - Returns: FFmpeg version
    @objc public static func getFFmpegVersion() -> String {
        return SmartFfmpegBridge.getFFmpegVersion()
    }
    
    // MARK: - Private Helpers
    
    private static func createImage(from rgbaData: Data, width: Int, height: Int) -> UIImage? {
        let bytesPerPixel = 4
        let bytesPerRow = width * bytesPerPixel
        let bitsPerComponent = 8
        
        guard let provider = CGDataProvider(data: rgbaData as CFData) else {
            return nil
        }
        
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
        
        guard let cgImage = CGImage(
            width: width,
            height: height,
            bitsPerComponent: bitsPerComponent,
            bitsPerPixel: bytesPerPixel * 8,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: true,
            intent: .defaultIntent
        ) else {
            return nil
        }
        
        return UIImage(cgImage: cgImage)
    }
}

/// Video metadata structure for type-safe access
@objc public class VideoMetadata: NSObject {
    @objc public let width: Int
    @objc public let height: Int
    @objc public let duration: Int64
    @objc public let codec: String?
    @objc public let bitrate: Int64
    @objc public let format: String?
    @objc public let frameRate: String?
    
    init?(from dictionary: [String: Any]) {
        guard let width = dictionary["width"] as? Int,
              let height = dictionary["height"] as? Int,
              let duration = dictionary["duration"] as? Int64 else {
            return nil
        }
        
        self.width = width
        self.height = height
        self.duration = duration
        self.codec = dictionary["codec"] as? String
        self.bitrate = dictionary["bitrate"] as? Int64 ?? 0
        self.format = dictionary["format"] as? String
        self.frameRate = dictionary["frameRate"] as? String
    }
    
    @objc public static func from(dictionary: [String: Any]) -> VideoMetadata? {
        return VideoMetadata(from: dictionary)
    }
}
