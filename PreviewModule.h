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

// 用户在预览界面里对选区做出的确认动作
enum class PreviewAction {
    None,    // 取消：ESC、鼠标右键、关闭窗口
    Copy,    // 回车或在选区内双击：压缩到 SDR 后写入剪贴板
    SaveHdr, // S 键：保存为 Ultra HDR 文件
};

struct PreviewResult {
    SelectionRect selection{};
    PreviewAction action = PreviewAction::None;
};

class PreviewWindow {
public:
    virtual ~PreviewWindow() = default;

    // 在全屏窗口中展示已上传至 GPU 的帧。
    // 此调用阻塞直到用户确认选区或取消。
    // 返回值中的 selection 仅在 action 不为 None 时有效。
    virtual PreviewResult Show(std::shared_ptr<GpuFrame> gpuFrame) = 0;

    static std::unique_ptr<PreviewWindow> Create(EGLDisplay display, EGLSurface dummySurface, EGLContext context);
};
