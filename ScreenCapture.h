#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct FrameMetadata {
    uint32_t width;
    uint32_t height;
    uint32_t rowPitch;
};

class CapturedFrame {
public:
    // Data is in R16G16B16A16_FLOAT format (scRGB)
    std::vector<uint8_t> pixelData;
    FrameMetadata metadata;
};

class ScreenCapturer {
public:
    virtual ~ScreenCapturer() = default;

    // Starts capturing the primary monitor.
    virtual void StartCapture() = 0;

    // Stops the capture session.
    virtual void StopCapture() = 0;

    // Returns the latest captured frame. Thread-safe.
    // Returns null if no frame captured yet.
    virtual std::shared_ptr<CapturedFrame> GetLatestFrame() = 0;

    virtual bool IsCapturing() const = 0;

    // Factory method to create an instance
    static std::unique_ptr<ScreenCapturer> Create();
};
