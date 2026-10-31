#pragma once

#include "GpuFrame.h"
#include "PreviewModule.h"
#include "SystemInfo.h"
#include <memory>

class OutputModule {
public:
    virtual ~OutputModule() = default;

    virtual void CopySelectionToClipboard(const GpuFrame &gpuFrame, const SelectionRect &selection,
                                          const DisplayHdrInfo &hdrInfo) = 0;

    // 保存为 Ultra HDR 文件（JPEG + gain map）：先弹保存对话框，用户确认后
    // 以剪贴板同一份 SDR 像素为基础层、capturedFrame 中同一区域的原始 scRGB 数据生成增益图。
    virtual void SaveSelectionAsUltraHdr(const GpuFrame &gpuFrame, const CapturedFrame &capturedFrame,
                                         const SelectionRect &selection, const DisplayHdrInfo &hdrInfo) = 0;

    static std::unique_ptr<OutputModule> Create(EGLDisplay display, EGLSurface dummySurface, EGLContext context);
};
