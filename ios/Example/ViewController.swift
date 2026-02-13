//
//  ViewController.swift
//  SmartFfmpegBridge Example
//
//  Example usage of SmartFfmpegBridge for iOS
//

import UIKit
import SmartFfmpegBridge

class ViewController: UIViewController {
    
    @IBOutlet weak var imageView: UIImageView!
    @IBOutlet weak var metadataLabel: UILabel!
    @IBOutlet weak var progressView: UIProgressView!
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        // Print FFmpeg version
        let version = SmartFfmpegBridgeSwift.getFFmpegVersion()
        print("FFmpeg version: \(version)")
    }
    
    @IBAction func selectVideoButtonTapped(_ sender: UIButton) {
        let picker = UIImagePickerController()
        picker.delegate = self
        picker.sourceType = .photoLibrary
        picker.mediaTypes = ["public.movie"]
        present(picker, animated: true)
    }
    
    private func processVideo(at url: URL) {
        // Get local file path
        guard let videoPath = copyVideoToTemp(url: url) else {
            showAlert(message: "Failed to access video file")
            return
        }
        
        // Get video metadata
        if let metadata = SmartFfmpegBridgeSwift.getVideoMetadata(videoPath),
           let videoMetadata = VideoMetadata.from(dictionary: metadata) {
            
            let metadataText = """
            Resolution: \(videoMetadata.width)x\(videoMetadata.height)
            Duration: \(formatDuration(videoMetadata.duration))
            Codec: \(videoMetadata.codec ?? "unknown")
            Bitrate: \(formatBitrate(videoMetadata.bitrate))
            Format: \(videoMetadata.format ?? "unknown")
            Frame Rate: \(videoMetadata.frameRate ?? "unknown")
            """
            
            metadataLabel.text = metadataText
            print("Video metadata:\n\(metadataText)")
        }
        
        // Extract thumbnail at 5 seconds
        extractThumbnail(from: videoPath, at: 5000)
    }
    
    private func extractThumbnail(from videoPath: String, at timeMs: Int64) {
        progressView.isHidden = false
        progressView.progress = 0.0
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            // Extract thumbnail
            let thumbnail = SmartFfmpegBridgeSwift.extractThumbnailImage(
                fromVideo: videoPath,
                atTime: timeMs,
                width: 640,
                height: 360
            )
            
            DispatchQueue.main.async {
                self?.progressView.isHidden = true
                
                if let thumbnail = thumbnail {
                    self?.imageView.image = thumbnail
                    print("Successfully extracted thumbnail")
                } else {
                    self?.showAlert(message: "Failed to extract thumbnail")
                }
            }
        }
    }
    
    // MARK: - Helper Methods
    
    private func copyVideoToTemp(url: URL) -> String? {
        let tempDir = NSTemporaryDirectory()
        let tempPath = (tempDir as NSString).appendingPathComponent("temp_video.mp4")
        
        do {
            // Remove existing file
            try? FileManager.default.removeItem(atPath: tempPath)
            
            // Copy video to temp location
            try FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: tempPath))
            return tempPath
        } catch {
            print("Error copying video: \(error)")
            return nil
        }
    }
    
    private func formatDuration(_ durationMs: Int64) -> String {
        let seconds = Double(durationMs) / 1000.0
        let minutes = Int(seconds / 60)
        let secs = Int(seconds.truncatingRemainder(dividingBy: 60))
        return String(format: "%d:%02d", minutes, secs)
    }
    
    private func formatBitrate(_ bitrate: Int64) -> String {
        let kbps = Double(bitrate) / 1000.0
        if kbps > 1000 {
            return String(format: "%.1f Mbps", kbps / 1000.0)
        } else {
            return String(format: "%.0f kbps", kbps)
        }
    }
    
    private func showAlert(message: String) {
        let alert = UIAlertController(
            title: "Error",
            message: message,
            preferredStyle: .alert
        )
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        present(alert, animated: true)
    }
}

// MARK: - UIImagePickerControllerDelegate

extension ViewController: UIImagePickerControllerDelegate, UINavigationControllerDelegate {
    
    func imagePickerController(
        _ picker: UIImagePickerController,
        didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey : Any]
    ) {
        picker.dismiss(animated: true)
        
        if let videoURL = info[.mediaURL] as? URL {
            processVideo(at: videoURL)
        }
    }
    
    func imagePickerControllerDidCancel(_ picker: UIImagePickerController) {
        picker.dismiss(animated: true)
    }
}
