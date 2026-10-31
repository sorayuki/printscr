#include "GpuFrame.h"
#include "Bt2446MethodC.h"
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

constexpr float kMinWhiteLinear = 0.000001f;

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

float LinearToSrgb(float linearValue) {
    linearValue = Clamp01(linearValue);
    if (linearValue <= 0.0031308f) {
        return linearValue * 12.92f;
    }
    return 1.055f * std::pow(linearValue, 1.0f / 2.4f) - 0.055f;
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

    const float sdrWhiteScRgb = (std::max)(sdrWhiteNits / kScRgbWhiteNits, kMinWhiteLinear);

    const size_t pixelCount = static_cast<size_t>(frame.metadata.width) * frame.metadata.height;
    std::vector<uint8_t> pixels(pixelCount * 4);
    const auto *source = reinterpret_cast<const uint16_t *>(frame.pixelData.data());

    // 统计画面峰值；与输出模块一致，没有超过 SDR 白点的内容时按纯 SDR 直接缩放
    float framePeak = 0.0f;
    for (size_t i = 0; i < pixelCount * 4; ++i) {
        if ((i & 3) == 3) {
            continue;
        }
        const float value = HalfToFloat(source[i]);
        if (value > framePeak && std::isfinite(value)) {
            framePeak = value;
        }
    }
    const bool needsToneMap = framePeak > sdrWhiteScRgb * 1.01f;
    LOG("GpuFrame: 画面峰值 " + std::to_string(framePeak * kScRgbWhiteNits) + " nits, 使用" +
        (needsToneMap ? " BT.2446-1 方法 C 色调映射" : "线性 sRGB 直通"));

    // 画面峰值同时作为方法 C 的映射锚点，高光随峰值展宽而不再被裁成纯白
    const std::unique_ptr<Bt2446MethodC> methodC =
        needsToneMap ? Bt2446MethodC::CreateForHdrPeak(framePeak * kScRgbWhiteNits) : nullptr;
    if (methodC) {
        const Bt2446MethodCParams &params = methodC->Params();
        LOG("GpuFrame: 方法 C 锚点 " + std::to_string(params.anchorNits) + " nits, k2=" +
            std::to_string(params.k2) + ", k3=" + std::to_string(params.k3) + ", k4=" +
            std::to_string(params.k4));
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        const float r = (std::max)(HalfToFloat(source[i * 4 + 0]), 0.0f);
        const float g = (std::max)(HalfToFloat(source[i * 4 + 1]), 0.0f);
        const float b = (std::max)(HalfToFloat(source[i * 4 + 2]), 0.0f);

        float srgbR = 0.0f;
        float srgbG = 0.0f;
        float srgbB = 0.0f;
        if (!needsToneMap) {
            srgbR = LinearToSrgb(r / sdrWhiteScRgb);
            srgbG = LinearToSrgb(g / sdrWhiteScRgb);
            srgbB = LinearToSrgb(b / sdrWhiteScRgb);
        } else {
            // 方法 C 输出线性 SDR 光（1.0 = SDR 峰值），需再做 sRGB 编码才是 8bit 像素值
            float linearR = 0.0f;
            float linearG = 0.0f;
            float linearB = 0.0f;
            methodC->MapToSdrLinear(r, g, b, linearR, linearG, linearB);
            srgbR = LinearToSrgb(linearR);
            srgbG = LinearToSrgb(linearG);
            srgbB = LinearToSrgb(linearB);
        }

        pixels[i * 4 + 0] = FloatToByte(srgbR);
        pixels[i * 4 + 1] = FloatToByte(srgbG);
        pixels[i * 4 + 2] = FloatToByte(srgbB);
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
