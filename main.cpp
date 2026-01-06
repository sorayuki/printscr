#include "ScreenCapture.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <winrt/base.h>

int main() {
  winrt::init_apartment();

  std::cout << "Printscr started." << std::endl;

  try {
    auto capturer = ScreenCapturer::Create();
    capturer->StartCapture();
    std::cout << "Capture started. Waiting for frames..." << std::endl;

    // Wait a bit
    for (int i = 0; i < 50; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto frame = capturer->GetLatestFrame();
      if (frame) {
        std::cout << "Frame captured! " << frame->metadata.width << "x"
                  << frame->metadata.height << " (" << frame->pixelData.size()
                  << " bytes)" << std::endl;

        // Keep capturing for a bit to ensure stability
        if (i > 10)
          break;
      }
    }

    capturer->StopCapture();
    std::cout << "Capture stopped." << std::endl;
  } catch (const winrt::hresult_error &ex) {
    std::cerr << "WinRT Error: " << winrt::to_string(ex.message()) << std::endl;
  } catch (const std::exception &ex) {
    std::cerr << "Error: " << ex.what() << std::endl;
  }

  return 0;
}
