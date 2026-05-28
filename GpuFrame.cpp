#include "GpuFrame.h"
#include "Logger.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr float kBt1886Gamma = 2.4f;
constexpr float kReferencePeakNits = 1000.0f;
constexpr float kScRgbReferenceWhiteNits = 80.0f;
constexpr float kSdrReferenceWhiteNits = 80.0f;
constexpr float kMinWhiteLinear = 0.000001f;
constexpr float kTargetWhiteSignal = 0.9345f;
constexpr float kHlgA = 0.17883277f;
constexpr float kHlgB = 1.0f - 4.0f * kHlgA;
constexpr float kHlgC = 0.55991073f;

struct Float3 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
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
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

Float3 SrgbLinearToBt2020Linear(const Float3 &color) {
    return {
        0.6274040f * color.r + 0.3292820f * color.g + 0.0433136f * color.b,
        0.0690970f * color.r + 0.9195400f * color.g + 0.0113612f * color.b,
        0.0163916f * color.r + 0.0880132f * color.g + 0.8955950f * color.b
    };
}

Float3 Bt2020LinearToBt709Linear(const Float3 &color) {
    return {
        1.6604910f * color.r - 0.5876411f * color.g - 0.0728499f * color.b,
        -0.1245505f * color.r + 1.1328999f * color.g - 0.0083494f * color.b,
        -0.0181508f * color.r - 0.1005789f * color.g + 1.1187297f * color.b
    };
}

float HlgOetf(float linearValue) {
    linearValue = Clamp01(linearValue);
    if (linearValue <= (1.0f / 12.0f)) {
        return std::sqrt(3.0f * linearValue);
    }
    return kHlgA * std::log(12.0f * linearValue - kHlgB) + kHlgC;
}

float Bt1886Eotf(float signalValue) {
    return std::pow(Clamp01(signalValue), kBt1886Gamma);
}

float Bt1886Oetf(float linearValue) {
    return std::pow(Clamp01(linearValue), 1.0f / kBt1886Gamma);
}

float ToneMapHdrToSdrLinear(float linearValue, float whiteLinear) {
    whiteLinear = (std::max)(whiteLinear, kMinWhiteLinear);
    linearValue = (std::max)(linearValue, 0.0f);

    const float normalized = linearValue / whiteLinear;
    const float targetWhiteLinear = Bt1886Eotf(kTargetWhiteSignal);
    if (normalized <= 1.0f) {
        return normalized * targetWhiteLinear;
    }

    const float peakNormalized = (std::max)(1.0f / whiteLinear, 1.000001f);
    const float t = std::clamp((normalized - 1.0f) / (peakNormalized - 1.0f), 0.0f, 1.0f);
    const float shoulder = 1.0f - std::pow(1.0f - t, 2.0f);
    return targetWhiteLinear + (1.0f - targetWhiteLinear) * shoulder;
}

uint8_t FloatToByte(float value) {
    return static_cast<uint8_t>(std::round(Clamp01(value) * 255.0f));
}

std::vector<uint8_t> ConvertCapturedFrameToSdr8(const CapturedFrame &frame, float sdrWhiteNits) {
    const size_t expectedTightPitch = static_cast<size_t>(frame.metadata.width) * sizeof(uint16_t) * 4;
    const size_t expectedTotal = expectedTightPitch * frame.metadata.height;
    if (frame.metadata.rowPitch != expectedTightPitch || frame.pixelData.size() < expectedTotal) {
        throw std::runtime_error("GpuFrame: 无法提前转换 SDR，捕捉帧不是紧凑 RGBA16F 布局");
    }

    const float lw = (std::max)(sdrWhiteNits / kSdrReferenceWhiteNits, kMinWhiteLinear);
    const float sdrWhiteHlg = HlgOetf(lw * kScRgbReferenceWhiteNits / kReferencePeakNits);
    const float sdrWhiteLinear = (std::max)(Bt1886Eotf(sdrWhiteHlg), kMinWhiteLinear);

    const size_t pixelCount = static_cast<size_t>(frame.metadata.width) * frame.metadata.height;
    std::vector<uint8_t> pixels(pixelCount * 4);
    const auto *source = reinterpret_cast<const uint16_t *>(frame.pixelData.data());

    for (size_t i = 0; i < pixelCount; ++i) {
        const Float3 scRgb = {
            (std::max)(HalfToFloat(source[i * 4 + 0]), 0.0f),
            (std::max)(HalfToFloat(source[i * 4 + 1]), 0.0f),
            (std::max)(HalfToFloat(source[i * 4 + 2]), 0.0f)
        };

        const Float3 bt2020 = SrgbLinearToBt2020Linear(scRgb);
        const Float3 hlg = {
            HlgOetf(bt2020.r * kScRgbReferenceWhiteNits / kReferencePeakNits),
            HlgOetf(bt2020.g * kScRgbReferenceWhiteNits / kReferencePeakNits),
            HlgOetf(bt2020.b * kScRgbReferenceWhiteNits / kReferencePeakNits)
        };
        const Float3 interpreted = {Bt1886Eotf(hlg.r), Bt1886Eotf(hlg.g), Bt1886Eotf(hlg.b)};
        const Float3 bt709Raw = Bt2020LinearToBt709Linear(interpreted);
        const Float3 bt709ToneMapped = {
            ToneMapHdrToSdrLinear(bt709Raw.r, sdrWhiteLinear),
            ToneMapHdrToSdrLinear(bt709Raw.g, sdrWhiteLinear),
            ToneMapHdrToSdrLinear(bt709Raw.b, sdrWhiteLinear)
        };

        pixels[i * 4 + 0] = FloatToByte(Bt1886Oetf(bt709ToneMapped.r));
        pixels[i * 4 + 1] = FloatToByte(Bt1886Oetf(bt709ToneMapped.g));
        pixels[i * 4 + 2] = FloatToByte(Bt1886Oetf(bt709ToneMapped.b));
        pixels[i * 4 + 3] = 255;
    }

    return pixels;
}

std::string DescribeEglError(EGLint error) {
    switch (error) {
    case EGL_SUCCESS:           return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:   return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:         return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:     return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:       return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:       return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:       return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:         return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:     return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:      return "EGL_CONTEXT_LOST";
    default:                    return "Unknown EGL error";
    }
}

class GpuFrameImpl final : public GpuFrame {
public:
    GpuFrameImpl(const CapturedFrame &frame, EGLDisplay display, EGLSurface dummySurface, EGLContext context)
        : m_display(display), m_surface(dummySurface), m_context(context) {
        LOG("GpuFrame: 上传纹理 " + std::to_string(frame.metadata.width) +
            "x" + std::to_string(frame.metadata.height) + "...");

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            throw std::runtime_error("GpuFrame: eglMakeCurrent 失败: " +
                                     DescribeEglError(eglGetError()));
        }

        // 验证行距确实为紧凑布局（ScreenCapture 已消除填充行，此处再次确认）
        const size_t expectedTightPitch =
            static_cast<size_t>(frame.metadata.width) * sizeof(uint16_t) * 4; // R16G16B16A16
        const size_t expectedTotal = expectedTightPitch * frame.metadata.height;
        if (frame.metadata.rowPitch != expectedTightPitch) {
            throw std::runtime_error(
                "GpuFrame: 帧数据行距不为紧凑布局，无法直接上传 "
                "(rowPitch=" + std::to_string(frame.metadata.rowPitch) +
                ", expected=" + std::to_string(expectedTightPitch) + ")");
        }
        if (frame.pixelData.size() < expectedTotal) {
            throw std::runtime_error(
                "GpuFrame: 帧数据缓冲区大小不足 "
                "(size=" + std::to_string(frame.pixelData.size()) +
                ", expected=" + std::to_string(expectedTotal) + ")");
        }

        UploadTexture(frame.metadata.width, frame.metadata.height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT,
                  frame.pixelData.data());

        // 释放 current，由各使用模块自行绑定
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        m_width  = frame.metadata.width;
        m_height = frame.metadata.height;
        m_format = GpuFrameFormat::ScRgbFloat16;

        LOG("GpuFrame: 纹理上传完成。");
    }

    GpuFrameImpl(uint32_t width, uint32_t height, const std::vector<uint8_t> &rgbaPixels,
                 EGLDisplay display, EGLSurface dummySurface, EGLContext context)
        : m_display(display), m_surface(dummySurface), m_context(context), m_width(width), m_height(height),
          m_format(GpuFrameFormat::Sdr8) {
        LOG("GpuFrame: 上传提前转换后的 SDR 8bit 纹理 " + std::to_string(width) + "x" + std::to_string(height) + "...");

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            throw std::runtime_error("GpuFrame: eglMakeCurrent 失败: " + DescribeEglError(eglGetError()));
        }

        UploadTexture(width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels.data());
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        LOG("GpuFrame: SDR 8bit 纹理上传完成。");
    }

    ~GpuFrameImpl() {
        if (m_display != EGL_NO_DISPLAY && m_texture != 0) {
            eglMakeCurrent(m_display, m_surface, m_surface, m_context);
            glDeleteTextures(1, &m_texture);
            m_texture = 0;
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    EGLDisplay  GetDisplay()   const override { return m_display;  }
    EGLContext  GetContext()   const override { return m_context;  }
    EGLSurface  GetSurface()   const override { return m_surface;  }
    GLuint      GetTextureId() const override { return m_texture;  }
    uint32_t    Width()        const override { return m_width;    }
    uint32_t    Height()       const override { return m_height;   }
    GpuFrameFormat GetFormat() const override { return m_format; }

private:
    void UploadTexture(uint32_t width, uint32_t height, GLint internalFormat, GLenum format, GLenum type,
                       const void *pixels) {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                     format, type, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    EGLDisplay  m_display = EGL_NO_DISPLAY;
    EGLSurface  m_surface = EGL_NO_SURFACE;
    EGLContext  m_context = EGL_NO_CONTEXT;
    GLuint      m_texture = 0;
    uint32_t    m_width   = 0;
    uint32_t    m_height  = 0;
    GpuFrameFormat m_format = GpuFrameFormat::ScRgbFloat16;
};

} // namespace

std::shared_ptr<GpuFrame> GpuFrame::Create(const CapturedFrame &frame, EGLDisplay display, EGLSurface dummySurface, EGLContext context) {
    return std::make_shared<GpuFrameImpl>(frame, display, dummySurface, context);
}

std::shared_ptr<GpuFrame> GpuFrame::CreateSdr8FromHdr(const CapturedFrame &frame, float sdrWhiteNits,
                                                       EGLDisplay display, EGLSurface dummySurface,
                                                       EGLContext context) {
    LOG("GpuFrame: CapsLock 模式，捕捉后立即将 HDR/scRGB 转换为 SDR 8bit。");
    std::vector<uint8_t> sdrPixels = ConvertCapturedFrameToSdr8(frame, sdrWhiteNits);
    return std::make_shared<GpuFrameImpl>(frame.metadata.width, frame.metadata.height, sdrPixels, display,
                                          dummySurface, context);
}
