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

    static std::unique_ptr<OutputModule> Create(EGLDisplay display, EGLSurface dummySurface, EGLContext context);
};
