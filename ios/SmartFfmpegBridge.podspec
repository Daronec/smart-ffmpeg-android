Pod::Spec.new do |s|
  s.name             = 'SmartFfmpegBridge'
  s.version          = '1.0.4'
  s.summary          = 'FFmpeg-based video processing library for iOS'
  s.description      = <<-DESC
    Smart FFmpeg Bridge provides video thumbnail extraction, metadata reading,
    and video processing capabilities using FFmpeg libraries for iOS.
                       DESC
  s.homepage         = 'https://github.com/Daronec/smart-ffmpeg-android'
  s.license          = { :type => 'MIT', :file => '../LICENSE' }
  s.author           = { 'Daronec' => 'your.email@example.com' }
  s.source           = { :git => 'https://github.com/Daronec/smart-ffmpeg-android.git', :tag => "v#{s.version}" }

  s.ios.deployment_target = '12.0'
  s.swift_version = '5.0'

  s.source_files = 'Classes/**/*.{h,m,swift}'
  s.public_header_files = 'Classes/**/*.h'
  
  # FFmpeg dependencies - using ffmpeg-kit
  s.dependency 'ffmpeg-kit-ios-full', '~> 6.0'
  
  s.frameworks = 'Foundation', 'AVFoundation', 'CoreMedia', 'CoreVideo'
  s.libraries = 'c++'
  
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386'
  }
end
