#include "ScreenCapture.h"
#include "Logger.h"

#include <atomic>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <mutex>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <wrl/client.h>

// Needed for MonitorFromPoint
#include <windows.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

class ScreenCapturerImpl : public ScreenCapturer {
public:
    ScreenCapturerImpl() { InitializeDevice(); }
    ~ScreenCapturerImpl() { StopCapture(); }

    void StartCapture() override;
    void StopCapture() override;
    std::shared_ptr<CapturedFrame> GetLatestFrame() override;
    bool IsCapturing() const override { return is_capturing; }

private:
    void InitializeDevice();
    GraphicsCaptureItem CreateCaptureItemForPrimaryMonitor();
    void OnFrameArrived(Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &args);

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_winrt{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool frame_pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_revoker;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture;

    std::shared_ptr<CapturedFrame> latest_frame;
    std::mutex frame_mutex;
    std::atomic<bool> is_capturing{false};
    winrt::Windows::Graphics::SizeInt32 last_size{0, 0};
};

std::unique_ptr<ScreenCapturer> ScreenCapturer::Create() {
    return std::make_unique<ScreenCapturerImpl>();
}

void ScreenCapturerImpl::InitializeDevice() {
    // Create D3D11 Device
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    LOG("Initializing D3D11 device...");
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels,
                                   ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, d3d_device.GetAddressOf(), nullptr,
                                   d3d_context.GetAddressOf());

    if (FAILED(hr)) {
        LOG("Failed to create D3D11 device. HR=" + std::to_string(hr));
        throw std::runtime_error("Failed to create D3D11 device");
    }
    LOG("D3D11 device created.");

    // Create WinRT Wrapper for D3D11 Device
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = d3d_device.As(&dxgi_device);
    if (FAILED(hr))
        throw std::runtime_error("Failed to get DXGI Interface");

    winrt::com_ptr<::IInspectable> device_inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), device_inspectable.put());
    if (FAILED(hr))
        throw std::runtime_error("Failed to create WinRT D3D11 device");

    device_winrt = device_inspectable.as<IDirect3DDevice>();
}

GraphicsCaptureItem ScreenCapturerImpl::CreateCaptureItemForPrimaryMonitor() {
    // Get Primary Monitor
    POINT pt = {0, 0};
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);

    // Get Interop Factory
    auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem>();
    auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item = {nullptr};

    // Create Item
    HRESULT hr = interop_factory->CreateForMonitor(
        monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item));

    if (FAILED(hr))
        throw std::runtime_error("Failed to create Capture Item for Monitor");

    return item;
}

void ScreenCapturerImpl::StartCapture() {
    if (is_capturing)
        return;

    try {
        item = CreateCaptureItemForPrimaryMonitor();
        last_size = item.Size();
        LOG("Capture item created. Size=" + std::to_string(last_size.Width) + "x" + std::to_string(last_size.Height));

        // Create Frame Pool
        // Use scRGB format (R16G16B16A16Float - FP16) which is 64 bits per pixel
        LOG("Creating FramePool...");
        frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(device_winrt, DirectXPixelFormat::R16G16B16A16Float,
                                                                    2, last_size);

        LOG("Creating CaptureSession...");
        session = frame_pool.CreateCaptureSession(item);

        frame_arrived_revoker =
            frame_pool.FrameArrived(winrt::auto_revoke, {this, &ScreenCapturerImpl::OnFrameArrived});

        session.IsCursorCaptureEnabled(false);
        session.StartCapture();
        LOG("Capture session started.");
        is_capturing = true;
    } catch (winrt::hresult_error const &ex) {
        LOG("StartCapture failed (WinRT): " + winrt::to_string(ex.message()));
        throw;
    } catch (const std::exception &ex) {
        LOG("StartCapture failed (std): " + std::string(ex.what()));
        throw;
    }
}

void ScreenCapturerImpl::StopCapture() {
    if (!is_capturing)
        return;

    is_capturing = false;
    frame_arrived_revoker.revoke();

    if (session) {
        session.Close();
        session = nullptr;
    }

    if (frame_pool) {
        frame_pool.Close();
        frame_pool = nullptr;
    }

    item = nullptr;
    // Release resources under lock to ensure no on-going usage in OnFrameArrived
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        staging_texture.Reset();
    }
}

void ScreenCapturerImpl::OnFrameArrived(Direct3D11CaptureFramePool const &sender,
                                        winrt::Windows::Foundation::IInspectable const &) {
    auto frame = sender.TryGetNextFrame();
    if (!frame) {
        // LOG("OnFrameArrived: Frame is null");
        return;
    }

    auto contentSize = frame.ContentSize();
    // Check for resize
    if ((contentSize.Width != last_size.Width) || (contentSize.Height != last_size.Height)) {
        last_size = contentSize;
        frame_pool.Recreate(device_winrt, DirectXPixelFormat::R16G16B16A16Float, 2, last_size);
        staging_texture.Reset(); // Invalidate staging texture
        return;                  // Skip this frame to let recreation happen safely
    }

    // Get the texture from the frame
    auto surface = frame.Surface();
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = access->GetInterface(IID_PPV_ARGS(&texture));

    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        // Use a local ComPtr to hold reference during operation
        Microsoft::WRL::ComPtr<ID3D11Texture2D> local_staging_texture;

        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            if (!is_capturing)
                return;

            // Create or reuse staging texture
            if (!staging_texture) {
                D3D11_TEXTURE2D_DESC stagingDesc = desc;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.BindFlags = 0;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                stagingDesc.MiscFlags = 0;

                hr = d3d_device->CreateTexture2D(&stagingDesc, nullptr, staging_texture.GetAddressOf());
                if (FAILED(hr))
                    return;
            }
            local_staging_texture = staging_texture;
        }

        if (SUCCEEDED(hr) && local_staging_texture) {
            // Prepare CPU buffer
            auto newFrame = std::make_shared<CapturedFrame>();
            newFrame->metadata.width = desc.Width;
            newFrame->metadata.height = desc.Height;
            // 8 bytes per pixel (R16G16B16A16_FLOAT)
            const uint32_t bytesPerPixel = 8;

            // Copy to staging
            d3d_context->CopyResource(local_staging_texture.Get(), texture.Get());

            // Map staging to read
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = d3d_context->Map(local_staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                newFrame->metadata.rowPitch = desc.Width * bytesPerPixel;
                newFrame->pixelData.resize(newFrame->metadata.rowPitch * desc.Height);

                uint8_t *src = static_cast<uint8_t *>(mapped.pData);
                uint8_t *dst = newFrame->pixelData.data();

                // Copy row by row to remove padding if present
                for (UINT row = 0; row < desc.Height; ++row) {
                    memcpy(dst, src, newFrame->metadata.rowPitch);
                    dst += newFrame->metadata.rowPitch;
                    src += mapped.RowPitch;
                }

                d3d_context->Unmap(local_staging_texture.Get(), 0);

                std::lock_guard<std::mutex> lock(frame_mutex);
                latest_frame = newFrame;
            }
        }
    }
}

std::shared_ptr<CapturedFrame> ScreenCapturerImpl::GetLatestFrame() {
    std::lock_guard<std::mutex> lock(frame_mutex);
    return latest_frame;
}
