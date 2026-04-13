#pragma once

#include "GpuFrame.h"
#include <memory>
#include <windows.h>

struct SelectionRect {
    int x1, y1, x2, y2;

    int Left() const { return (x1 < x2) ? x1 : x2; }
    int Top() const { return (y1 < y2) ? y1 : y2; }
    int Right() const { return (x1 < x2) ? x2 : x1; }
    int Bottom() const { return (y1 < y2) ? y2 : y1; }
    int Width() const { return Right() - Left(); }
    int Height() const { return Bottom() - Top(); }
    bool IsValid() const { return Width() > 0 && Height() > 0; }
};

class PreviewWindow {
public:
    virtual ~PreviewWindow() = default;

    // 在全屏窗口中展示已上传至 GPU 的帧。
    // 此调用阻塞直到用户确认选区或取消。
    // 返回最终选区矩形。
    virtual SelectionRect Show(std::shared_ptr<GpuFrame> gpuFrame) = 0;

    static std::unique_ptr<PreviewWindow> Create();
};
