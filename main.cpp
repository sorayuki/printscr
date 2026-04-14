#include "GpuFrame.h"
#include "Logger.h"
#include "OutputModule.h"
#include "PreviewModule.h"
#include "ScreenCapture.h"
#include "SystemInfo.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winrt/base.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#pragma data_seg(".shared")
#pragma comment(linker, "/SECTION:.shared,RWS")
static struct {
    wchar_t caller_mutex_name[80];
    wchar_t caller_event_name[80];
} g_shared_context = {0};
#pragma data_seg()

class PrintScrApp {
public:
    PrintScrApp() {
        LOG("Application started.");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        LOG("High DPI awareness set.");

        winrt::init_apartment();
        LOG("WinRT apartment initialized.");

        // Init EGL Root
        m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_eglDisplay == EGL_NO_DISPLAY) {
            throw std::runtime_error("eglGetDisplay failed");
        }
        if (!eglInitialize(m_eglDisplay, nullptr, nullptr)) {
            throw std::runtime_error("eglInitialize failed");
        }

        const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_NONE
        };
        EGLint configCount = 0;
        EGLConfig config;
        if (!eglChooseConfig(m_eglDisplay, configAttribs, &config, 1, &configCount) || configCount == 0) {
            throw std::runtime_error("eglChooseConfig failed");
        }

        const EGLint contextAttribs[] = {
            EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            EGL_CONTEXT_MINOR_VERSION_KHR, 1,
            EGL_NONE
        };
        m_rootContext = eglCreateContext(m_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
        if (m_rootContext == EGL_NO_CONTEXT) {
            throw std::runtime_error("eglCreateContext failed");
        }

        const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m_dummySurface = eglCreatePbufferSurface(m_eglDisplay, config, surfaceAttribs);
        if (m_dummySurface == EGL_NO_SURFACE) {
            throw std::runtime_error("eglCreatePbufferSurface failed");
        }

        LOG("Creating ScreenCapturer...");
        m_capturer = ScreenCapturer::Create();
        
        LOG("Creating PreviewWindow...");
        m_previewWindow = PreviewWindow::Create(m_eglDisplay, m_dummySurface, m_rootContext);
        
        LOG("Creating OutputModule...");
        m_outputModule = OutputModule::Create(m_eglDisplay, m_dummySurface, m_rootContext);
    }

    ~PrintScrApp() {
        m_previewWindow.reset();
        m_outputModule.reset();
        m_capturer.reset();
        if (m_eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_dummySurface != EGL_NO_SURFACE) eglDestroySurface(m_eglDisplay, m_dummySurface);
            if (m_rootContext != EGL_NO_CONTEXT) eglDestroyContext(m_eglDisplay, m_rootContext);
            eglTerminate(m_eglDisplay);
        }
    }

    int RunCaptureTarget() {
        try {
            LOG("Starting capture...");
            m_capturer->StartCapture();
            std::cout << "Capture started. Waiting for first frame..." << std::endl;

            std::shared_ptr<CapturedFrame> frame = nullptr;
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                frame = m_capturer->GetLatestFrame();
                if (frame) {
                    std::cout << "Frame captured! " << frame->metadata.width << "x" << frame->metadata.height << std::endl;
                    break;
                }
            }

            if (!frame) {
                std::cerr << "Timeout waiting for frame." << std::endl;
                m_capturer->StopCapture();
                return 1;
            }

            m_capturer->StopCapture();
            std::cout << "Capture stopped. Opening preview..." << std::endl;

            auto gpuFrame = GpuFrame::Create(*frame, m_eglDisplay, m_dummySurface, m_rootContext);
            std::cout << "GPU frame created." << std::endl;

            SelectionRect selection = m_previewWindow->Show(gpuFrame);

            if (selection.IsValid()) {
                std::cout << "Selection confirmed: (" << selection.Left() << ", " << selection.Top() << ") to ("
                          << selection.Right() << ", " << selection.Bottom() << ")" << std::endl;
                std::cout << "Size: " << selection.Width() << "x" << selection.Height() << std::endl;

                const DisplayHdrInfo hdrInfo = SystemInfo::GetPrimaryDisplayHdrInfo();
                m_outputModule->CopySelectionToClipboard(*gpuFrame, selection, hdrInfo);
                std::cout << "Selection copied to clipboard." << std::endl;
            } else {
                std::cout << "Selection cancelled." << std::endl;
            }

        } catch (const winrt::hresult_error &ex) {
            std::cerr << "WinRT Error: " << winrt::to_string(ex.message()) << std::endl;
            return 1;
        } catch (const std::exception &ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
            return 1;
        }

        return 0;
    }

private:
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLSurface m_dummySurface = EGL_NO_SURFACE;
    EGLContext m_rootContext = EGL_NO_CONTEXT;

    std::unique_ptr<ScreenCapturer> m_capturer;
    std::unique_ptr<PreviewWindow> m_previewWindow;
    std::unique_ptr<OutputModule> m_outputModule;
};


#include <ole2.h>
#include <atlbase.h>
int wmain(int argc, wchar_t *argv[]) {
    // 守护进程模式
    if (argc > 1 && wcscmp(argv[1], L"--daemon") == 0) {
        if (g_shared_context.caller_event_name[0] != 0 || g_shared_context.caller_mutex_name[0] != 0) {
            std::cerr << "Already running as daemon." << std::endl;
            return 1;
        }
        if (CoInitializeEx(nullptr, COINIT_MULTITHREADED) != S_OK) {
            std::cerr << "Failed to initialize COM." << std::endl;
            return 1;
        }

        // 生成guid
        GUID guid;
        CoCreateGuid(&guid);
        StringFromGUID2(guid, g_shared_context.caller_mutex_name, 80);
        CoCreateGuid(&guid);
        StringFromGUID2(guid, g_shared_context.caller_event_name, 80);
        
        CHandle hMutex { CreateMutexW(nullptr, FALSE, g_shared_context.caller_mutex_name) };
        if (hMutex == nullptr) {
            std::cerr << "Failed to create mutex." << std::endl;
            return 1;
        }
        CHandle hEvent { CreateEventW(nullptr, TRUE, FALSE, g_shared_context.caller_event_name) };
        if (hEvent == nullptr) {
            std::cerr << "Failed to create event." << std::endl;
            return 1;
        }

        PrintScrApp app;
        for(;;) {
            if (WaitForSingleObject(hEvent, INFINITE) == WAIT_OBJECT_0) {
                auto wait_mutex_result = WaitForSingleObject(hMutex, INFINITE);
                if (wait_mutex_result == WAIT_OBJECT_0 || wait_mutex_result == WAIT_ABANDONED_0) {
                    app.RunCaptureTarget();
                    ResetEvent(hEvent);
                    ReleaseMutex(hMutex);
                } else {
                    std::cerr << "Failed to wait for mutex." << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Failed to wait for event." << std::endl;
                return 1;
            }
        }
        return 0;
    }

    // 调用守护进程执行
    if (g_shared_context.caller_event_name[0] != 0 && g_shared_context.caller_mutex_name[0] != 0) {
        CHandle hEvent { OpenEventW(EVENT_ALL_ACCESS, FALSE, g_shared_context.caller_event_name) };
        if (hEvent == nullptr) {
            std::cerr << "Failed to open event." << std::endl;
            return 1;
        }
        CHandle hMutex { OpenMutexW(MUTEX_ALL_ACCESS, FALSE, g_shared_context.caller_mutex_name) };
        if (hMutex == nullptr) {
            std::cerr << "Failed to open mutex." << std::endl;
            return 1;
        }
        WaitForSingleObject(hMutex, INFINITE);
        SetEvent(hEvent);
        ReleaseMutex(hMutex);
        return 0;
    }

    // 没有正在运行的守护进程，冷启动执行
    PrintScrApp app;
    return app.RunCaptureTarget();
}
