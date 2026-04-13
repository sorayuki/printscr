#pragma once

#include "ScreenCapture.h"

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <memory>

// GPU 上常驻的帧纹理，持有 EGL display/context/surface 以及上传好的纹理对象。
// 所有需要访问这块纹理的模块（Preview、Output）都从本对象共享或借用 context，
// 从而避免多次 CPU↔GPU 传输。
class GpuFrame {
public:
    virtual ~GpuFrame() = default;

    virtual EGLDisplay GetDisplay() const = 0;
    virtual EGLContext GetContext() const = 0;

    // 1×1 PBuffer surface，供外部模块将本 context 设为 current 时使用
    virtual EGLSurface GetSurface() const = 0;

    virtual GLuint GetTextureId() const = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    // 从 CPU 内存数据创建 GpuFrame：初始化 EGL、上传纹理（仅此一次）
    static std::shared_ptr<GpuFrame> Create(const CapturedFrame &frame);
};
