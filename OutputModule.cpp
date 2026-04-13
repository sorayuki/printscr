#include "OutputModule.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {

struct Float3 {
    float r;
    float g;
    float b;
};

constexpr float kBt1886Gamma = 2.4f;
constexpr float kHlgA = 0.17883277f;
constexpr float kHlgB = 1.0f - 4.0f * kHlgA;
constexpr float kHlgC = 0.55991073f;
constexpr float kReferencePeakNits = 1000.0f;

float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

float SrgbLinearToBt2020LinearR(float r, float g, float b) { return 0.6274040f * r + 0.3292820f * g + 0.0433136f * b; }

float SrgbLinearToBt2020LinearG(float r, float g, float b) { return 0.0690970f * r + 0.9195400f * g + 0.0113612f * b; }

float SrgbLinearToBt2020LinearB(float r, float g, float b) { return 0.0163916f * r + 0.0880132f * g + 0.8955950f * b; }

float Bt2020LinearToBt709LinearR(float r, float g, float b) { return 1.6604910f * r - 0.5876411f * g - 0.0728499f * b; }

float Bt2020LinearToBt709LinearG(float r, float g, float b) { return -0.1245505f * r + 1.1328999f * g - 0.0083494f * b; }

float Bt2020LinearToBt709LinearB(float r, float g, float b) { return -0.0181508f * r - 0.1005789f * g + 1.1187297f * b; }

float HlgOetf(float linearValue) {
    linearValue = Clamp01(linearValue);
    if (linearValue <= (1.0f / 12.0f)) {
        return std::sqrt(3.0f * linearValue);
    }
    return kHlgA * std::log(12.0f * linearValue - kHlgB) + kHlgC;
}

float Bt1886Eotf(float signalValue) { return std::pow(Clamp01(signalValue), kBt1886Gamma); }

float Bt1886Oetf(float linearValue) { return std::pow(Clamp01(linearValue), 1.0f / kBt1886Gamma); }

uint8_t FloatToByte(float value) {
    const float scaled = Clamp01(value) * 255.0f;
    return static_cast<uint8_t>(std::lround(scaled));
}

float HalfToFloat(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x03FFu;

    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

SelectionRect ClampSelectionToFrame(const SelectionRect &selection, const FrameMetadata &metadata) {
    SelectionRect clamped = selection;
    clamped.x1 = std::clamp(clamped.x1, 0, static_cast<int>(metadata.width));
    clamped.x2 = std::clamp(clamped.x2, 0, static_cast<int>(metadata.width));
    clamped.y1 = std::clamp(clamped.y1, 0, static_cast<int>(metadata.height));
    clamped.y2 = std::clamp(clamped.y2, 0, static_cast<int>(metadata.height));
    return clamped;
}

void WriteBitmapToClipboard(const std::vector<uint8_t> &bgraPixels, int width, int height) {
    const SIZE_T headerSize = sizeof(BITMAPV5HEADER);
    const SIZE_T pixelBytes = static_cast<SIZE_T>(width) * static_cast<SIZE_T>(height) * 4;
    HGLOBAL dibMemory = GlobalAlloc(GMEM_MOVEABLE, headerSize + pixelBytes);
    if (!dibMemory) {
        throw std::runtime_error("GlobalAlloc failed for clipboard bitmap");
    }

    void *memory = GlobalLock(dibMemory);
    if (!memory) {
        GlobalFree(dibMemory);
        throw std::runtime_error("GlobalLock failed for clipboard bitmap");
    }

    auto *header = static_cast<BITMAPV5HEADER *>(memory);
    std::memset(header, 0, headerSize);
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = width;
    header->bV5Height = -height;
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_sRGB;

    auto *dstPixels = reinterpret_cast<uint8_t *>(header + 1);
    std::memcpy(dstPixels, bgraPixels.data(), pixelBytes);
    GlobalUnlock(dibMemory);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(dibMemory);
        throw std::runtime_error("OpenClipboard failed");
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(dibMemory);
        throw std::runtime_error("EmptyClipboard failed");
    }

    if (!SetClipboardData(CF_DIBV5, dibMemory)) {
        CloseClipboard();
        GlobalFree(dibMemory);
        throw std::runtime_error("SetClipboardData failed");
    }

    CloseClipboard();
}

class OutputModuleImpl final : public OutputModule {
public:
    void CopySelectionToClipboard(const CapturedFrame &frame, const SelectionRect &selection,
                                  const DisplayHdrInfo &hdrInfo) override {
        const SelectionRect clampedSelection = ClampSelectionToFrame(selection, frame.metadata);
        if (!clampedSelection.IsValid()) {
            throw std::runtime_error("Selection is empty after clamping");
        }

        const int outputWidth = clampedSelection.Width();
        const int outputHeight = clampedSelection.Height();
        std::vector<uint8_t> bgraPixels(static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight) * 4);

        const float sdrWhiteNits = hdrInfo.sdrWhiteLevel > 0.0f ? hdrInfo.sdrWhiteLevel : 80.0f;
        LOG("Copying selection to clipboard. Rect=(" + std::to_string(clampedSelection.Left()) + "," +
            std::to_string(clampedSelection.Top()) + ")-(" + std::to_string(clampedSelection.Right()) + "," +
            std::to_string(clampedSelection.Bottom()) + "), SDR white=" + std::to_string(sdrWhiteNits));

        for (int y = 0; y < outputHeight; ++y) {
            const int srcY = clampedSelection.Top() + y;
            const uint8_t *srcRow = frame.pixelData.data() + static_cast<size_t>(srcY) * frame.metadata.rowPitch;
            uint8_t *dstRow = bgraPixels.data() + static_cast<size_t>(y) * static_cast<size_t>(outputWidth) * 4;

            for (int x = 0; x < outputWidth; ++x) {
                const int srcX = clampedSelection.Left() + x;
                const auto *srcPixel =
                    reinterpret_cast<const uint16_t *>(srcRow + static_cast<size_t>(srcX) * sizeof(uint16_t) * 4);

                const float scrgbR = (std::max)(0.0f, HalfToFloat(srcPixel[0]));
                const float scrgbG = (std::max)(0.0f, HalfToFloat(srcPixel[1]));
                const float scrgbB = (std::max)(0.0f, HalfToFloat(srcPixel[2]));

                const float bt2020LinearR = SrgbLinearToBt2020LinearR(scrgbR, scrgbG, scrgbB);
                const float bt2020LinearG = SrgbLinearToBt2020LinearG(scrgbR, scrgbG, scrgbB);
                const float bt2020LinearB = SrgbLinearToBt2020LinearB(scrgbR, scrgbG, scrgbB);

                const float hlgR = HlgOetf(((std::max)(0.0f, bt2020LinearR) * sdrWhiteNits) / kReferencePeakNits);
                const float hlgG = HlgOetf(((std::max)(0.0f, bt2020LinearG) * sdrWhiteNits) / kReferencePeakNits);
                const float hlgB = HlgOetf(((std::max)(0.0f, bt2020LinearB) * sdrWhiteNits) / kReferencePeakNits);

                const float interpretedLinearR = Bt1886Eotf(hlgR);
                const float interpretedLinearG = Bt1886Eotf(hlgG);
                const float interpretedLinearB = Bt1886Eotf(hlgB);

                const float bt709LinearR =
                    Bt2020LinearToBt709LinearR(interpretedLinearR, interpretedLinearG, interpretedLinearB);
                const float bt709LinearG =
                    Bt2020LinearToBt709LinearG(interpretedLinearR, interpretedLinearG, interpretedLinearB);
                const float bt709LinearB =
                    Bt2020LinearToBt709LinearB(interpretedLinearR, interpretedLinearG, interpretedLinearB);

                const float bt709SignalR = Bt1886Oetf(bt709LinearR);
                const float bt709SignalG = Bt1886Oetf(bt709LinearG);
                const float bt709SignalB = Bt1886Oetf(bt709LinearB);

                uint8_t *dstPixel = dstRow + static_cast<size_t>(x) * 4;
                dstPixel[0] = FloatToByte(bt709SignalB);
                dstPixel[1] = FloatToByte(bt709SignalG);
                dstPixel[2] = FloatToByte(bt709SignalR);
                dstPixel[3] = 255;
            }
        }

        WriteBitmapToClipboard(bgraPixels, outputWidth, outputHeight);
        LOG("Selection copied to clipboard as 8-bit BT.709 bitmap.");
    }
};

} // namespace

std::unique_ptr<OutputModule> OutputModule::Create() { return std::make_unique<OutputModuleImpl>(); }
