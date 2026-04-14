#include "GpuFrame.h"
#include "Logger.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        // ScreenCapture 已按紧凑行距保存数据，可直接传入
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                     static_cast<GLsizei>(frame.metadata.width),
                     static_cast<GLsizei>(frame.metadata.height),
                     0, GL_RGBA, GL_HALF_FLOAT, frame.pixelData.data());

        glBindTexture(GL_TEXTURE_2D, 0);

        // 释放 current，由各使用模块自行绑定
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        m_width  = frame.metadata.width;
        m_height = frame.metadata.height;

        LOG("GpuFrame: 纹理上传完成。");
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

private:
    EGLDisplay  m_display = EGL_NO_DISPLAY;
    EGLSurface  m_surface = EGL_NO_SURFACE;
    EGLContext  m_context = EGL_NO_CONTEXT;
    GLuint      m_texture = 0;
    uint32_t    m_width   = 0;
    uint32_t    m_height  = 0;
};

} // namespace

std::shared_ptr<GpuFrame> GpuFrame::Create(const CapturedFrame &frame, EGLDisplay display, EGLSurface dummySurface, EGLContext context) {
    return std::make_shared<GpuFrameImpl>(frame, display, dummySurface, context);
}
