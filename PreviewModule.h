#pragma once

#include "ScreenCapture.h"
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

    // Shows the captured frame in a fullscreen window.
    // This call blocks until the user confirms the selection or cancels.
    // Returns the final selection rectangle.
    virtual SelectionRect Show(std::shared_ptr<CapturedFrame> frame) = 0;

    static std::unique_ptr<PreviewWindow> Create();
};
