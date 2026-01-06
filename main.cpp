#include "Logger.h"
#include "PreviewModule.h"
#include "ScreenCapture.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winrt/base.h>

int main() {
    // Logger::Init("printscr.log");
    LOG("Application started.");

    // Declare High DPI support
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LOG("High DPI awareness set.");

    winrt::init_apartment();
    LOG("WinRT apartment initialized.");

    try {
        LOG("Creating ScreenCapturer...");
        auto capturer = ScreenCapturer::Create();
        LOG("Starting capture...");
        capturer->StartCapture();
        std::cout << "Capture started. Waiting for first frame..." << std::endl;

        std::shared_ptr<CapturedFrame> frame = nullptr;
        // Wait for at least one frame
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            frame = capturer->GetLatestFrame();
            if (frame) {
                std::cout << "Frame captured! " << frame->metadata.width << "x" << frame->metadata.height << std::endl;
                break;
            }
        }

        if (!frame) {
            std::cerr << "Timeout waiting for frame." << std::endl;
            capturer->StopCapture();
            return 1;
        }

        // Stop capture before showing preview to avoid resource contention or just
        // to be clean
        capturer->StopCapture();
        std::cout << "Capture stopped. Opening preview..." << std::endl;

        auto preview = PreviewWindow::Create();
        SelectionRect selection = preview->Show(frame);

        if (selection.IsValid()) {
            std::cout << "Selection confirmed: (" << selection.Left() << ", " << selection.Top() << ") to ("
                      << selection.Right() << ", " << selection.Bottom() << ")" << std::endl;
            std::cout << "Size: " << selection.Width() << "x" << selection.Height() << std::endl;
        } else {
            std::cout << "Selection cancelled." << std::endl;
        }

    } catch (const winrt::hresult_error &ex) {
        std::cerr << "WinRT Error: " << winrt::to_string(ex.message()) << std::endl;
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }

    return 0;
}
