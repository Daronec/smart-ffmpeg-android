//
//  SmartFfmpegBridge.h
//  SmartFfmpegBridge
//
//  iOS bridge for FFmpeg video processing
//

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Bridge class for FFmpeg native operations on iOS.
 * Provides high-level API for video thumbnail extraction and metadata reading.
 */
@interface SmartFfmpegBridge : NSObject

/**
 * Extract thumbnail from video file.
 *
 * @param videoPath Absolute path to video file
 * @param timeMs Time position in milliseconds
 * @param width Target width (will maintain aspect ratio if height is 0)
 * @param height Target height (will maintain aspect ratio if width is 0)
 * @return NSData containing RGBA pixel data, or nil on error
 */
+ (nullable NSData *)extractThumbnailFromVideo:(NSString *)videoPath
                                        atTime:(int64_t)timeMs
                                         width:(int)width
                                        height:(int)height;

/**
 * Get video duration in milliseconds.
 *
 * @param videoPath Absolute path to video file
 * @return Duration in milliseconds, or -1 on error
 */
+ (int64_t)getVideoDuration:(NSString *)videoPath;

/**
 * Get video metadata.
 *
 * @param videoPath Absolute path to video file
 * @return Dictionary containing metadata (width, height, duration, codec, etc.)
 */
+ (nullable NSDictionary<NSString *, id> *)getVideoMetadata:(NSString *)videoPath;

/**
 * Get FFmpeg version string.
 *
 * @return FFmpeg version
 */
+ (NSString *)getFFmpegVersion;

@end

NS_ASSUME_NONNULL_END
