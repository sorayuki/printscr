#pragma once

#include "PreviewModule.h"
#include "ScreenCapture.h"
#include "SystemInfo.h"
#include <memory>

class OutputModule {
public:
    virtual ~OutputModule() = default;

    virtual void CopySelectionToClipboard(const CapturedFrame &frame, const SelectionRect &selection,
                                          const DisplayHdrInfo &hdrInfo) = 0;

    static std::unique_ptr<OutputModule> Create();
};
