//
//  SmartFfmpegBridge.m
//  SmartFfmpegBridge
//
//  iOS bridge for FFmpeg video processing
//

#import "SmartFfmpegBridge.h"
#import <ffmpegkit/FFmpegKit.h>
#import <ffmpegkit/FFprobeKit.h>
#import <ffmpegkit/MediaInformation.h>
#import <ffmpegkit/StreamInformation.h>

@implementation SmartFfmpegBridge

+ (nullable NSData *)extractThumbnailFromVideo:(NSString *)videoPath
                                        atTime:(int64_t)timeMs
                                         width:(int)width
                                        height:(int)height {
    @autoreleasepool {
        if (![[NSFileManager defaultManager] fileExistsAtPath:videoPath]) {
            NSLog(@"[SmartFfmpegBridge] Video file not found: %@", videoPath);
            return nil;
        }
        
        // Create temporary output path
        NSString *tempDir = NSTemporaryDirectory();
        NSString *outputPath = [tempDir stringByAppendingPathComponent:
                               [NSString stringWithFormat:@"thumb_%lld.raw", (long long)timeMs]];
        
        // Remove existing file if any
        [[NSFileManager defaultManager] removeItemAtPath:outputPath error:nil];
        
        // Convert milliseconds to seconds
        double timeSeconds = timeMs / 1000.0;
        
        // Build FFmpeg command for thumbnail extraction
        // -ss: seek to position
        // -i: input file
        // -vframes 1: extract one frame
        // -s: scale to target size
        // -f rawvideo: output raw video
        // -pix_fmt rgba: RGBA pixel format
        NSString *command = [NSString stringWithFormat:
                            @"-ss %.3f -i \"%@\" -vframes 1 -s %dx%d -f rawvideo -pix_fmt rgba \"%@\"",
                            timeSeconds, videoPath, width, height, outputPath];
        
        NSLog(@"[SmartFfmpegBridge] Executing FFmpeg command: %@", command);
        
        // Execute FFmpeg command
        FFmpegSession *session = [FFmpegKit execute:command];
        ReturnCode *returnCode = [session getReturnCode];
        
        if (![ReturnCode isSuccess:returnCode]) {
            NSLog(@"[SmartFfmpegBridge] FFmpeg command failed with code: %@", returnCode);
            NSLog(@"[SmartFfmpegBridge] Output: %@", [session getOutput]);
            return nil;
        }
        
        // Read the raw RGBA data
        NSData *rgbaData = [NSData dataWithContentsOfFile:outputPath];
        
        // Clean up temporary file
        [[NSFileManager defaultManager] removeItemAtPath:outputPath error:nil];
        
        if (!rgbaData || rgbaData.length == 0) {
            NSLog(@"[SmartFfmpegBridge] Failed to read thumbnail data");
            return nil;
        }
        
        // Verify data size matches expected RGBA size
        NSUInteger expectedSize = width * height * 4; // 4 bytes per pixel (RGBA)
        if (rgbaData.length != expectedSize) {
            NSLog(@"[SmartFfmpegBridge] Unexpected data size: %lu (expected %lu)",
                  (unsigned long)rgbaData.length, (unsigned long)expectedSize);
            return nil;
        }
        
        NSLog(@"[SmartFfmpegBridge] Successfully extracted thumbnail: %dx%d, %lu bytes",
              width, height, (unsigned long)rgbaData.length);
        
        return rgbaData;
    }
}

+ (int64_t)getVideoDuration:(NSString *)videoPath {
    @autoreleasepool {
        if (![[NSFileManager defaultManager] fileExistsAtPath:videoPath]) {
            NSLog(@"[SmartFfmpegBridge] Video file not found: %@", videoPath);
            return -1;
        }
        
        MediaInformationSession *session = [FFprobeKit getMediaInformation:videoPath];
        MediaInformation *mediaInfo = [session getMediaInformation];
        
        if (!mediaInfo) {
            NSLog(@"[SmartFfmpegBridge] Failed to get media information");
            return -1;
        }
        
        NSString *durationStr = [mediaInfo getDuration];
        if (!durationStr) {
            NSLog(@"[SmartFfmpegBridge] Duration not available");
            return -1;
        }
        
        // Convert duration from seconds to milliseconds
        double durationSeconds = [durationStr doubleValue];
        int64_t durationMs = (int64_t)(durationSeconds * 1000);
        
        NSLog(@"[SmartFfmpegBridge] Video duration: %lld ms", (long long)durationMs);
        
        return durationMs;
    }
}

+ (nullable NSDictionary<NSString *, id> *)getVideoMetadata:(NSString *)videoPath {
    @autoreleasepool {
        if (![[NSFileManager defaultManager] fileExistsAtPath:videoPath]) {
            NSLog(@"[SmartFfmpegBridge] Video file not found: %@", videoPath);
            return nil;
        }
        
        MediaInformationSession *session = [FFprobeKit getMediaInformation:videoPath];
        MediaInformation *mediaInfo = [session getMediaInformation];
        
        if (!mediaInfo) {
            NSLog(@"[SmartFfmpegBridge] Failed to get media information");
            return nil;
        }
        
        NSMutableDictionary *metadata = [NSMutableDictionary dictionary];
        
        // Get duration
        NSString *durationStr = [mediaInfo getDuration];
        if (durationStr) {
            double durationSeconds = [durationStr doubleValue];
            int64_t durationMs = (int64_t)(durationSeconds * 1000);
            metadata[@"duration"] = @(durationMs);
        }
        
        // Get bitrate
        NSString *bitrateStr = [mediaInfo getBitrate];
        if (bitrateStr) {
            metadata[@"bitrate"] = @([bitrateStr longLongValue]);
        }
        
        // Get format
        NSString *format = [mediaInfo getFormat];
        if (format) {
            metadata[@"format"] = format;
        }
        
        // Find video stream
        NSArray<StreamInformation *> *streams = [mediaInfo getStreams];
        for (StreamInformation *stream in streams) {
            if ([[stream getCodecType] isEqualToString:@"video"]) {
                // Get video codec
                NSString *codec = [stream getCodecName];
                if (codec) {
                    metadata[@"codec"] = codec;
                }
                
                // Get width and height
                NSNumber *width = [stream getWidth];
                NSNumber *height = [stream getHeight];
                if (width) metadata[@"width"] = width;
                if (height) metadata[@"height"] = height;
                
                // Get frame rate
                NSString *frameRate = [stream getAverageFrameRate];
                if (frameRate) {
                    metadata[@"frameRate"] = frameRate;
                }
                
                break; // Use first video stream
            }
        }
        
        NSLog(@"[SmartFfmpegBridge] Video metadata: %@", metadata);
        
        return [metadata copy];
    }
}

+ (NSString *)getFFmpegVersion {
    return [FFmpegKitConfig getFFmpegVersion];
}

@end
