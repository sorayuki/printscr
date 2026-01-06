#include "SystemInfo.h"
#include <dxgi1_6.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Define CCD structures if missing in the SDK
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL_CUSTOM =
    (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;

struct DISPLAYCONFIG_SDR_WHITE_LEVEL_CUSTOM {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    ULONG SDRWhiteLevel;
};

DisplayHdrInfo SystemInfo::GetPrimaryDisplayHdrInfo() {
    DisplayHdrInfo info = {200.0f, 1000.0f, 0.001f, 1000.0f, 600.0f};

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return info;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) {
        return info;
    }

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        return info;
    }

    ComPtr<IDXGIOutput6> output6;
    if (SUCCEEDED(output.As(&output6))) {
        DXGI_OUTPUT_DESC1 desc;
        if (SUCCEEDED(output6->GetDesc1(&desc))) {
            // In Windows, SDR white level is often 80 nits by default in scRGB 1.0.
            // But the "SDR White Level" setting in Windows HDR settings determines
            // how SDR content is scaled.
            // This value is not directly in GetDesc1, but peak luminance etc are.

            info.maxLuminance = desc.MaxLuminance;
            info.minLuminance = desc.MinLuminance;
            info.peakBrightness = desc.MaxLuminance;
            info.maxFullFrameLuminance = desc.MaxFullFrameLuminance;

            // Note: SDR White Level is trickier to get.
            // It's usually found in the registry or via some more modern APIs.
            // For now, we'll assume a reasonable default or look for it if possible.
            // Actually, we can assume 1.0 scRGB = 80 nits.
            // Many users set SDR white to 200 nits.
        }
    }

    // Use CCD API to get the SDR white level
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) ==
            ERROR_SUCCESS) {
            for (const auto &path : paths) {
                // Find potential primary display (usually at 0,0)
                if (path.sourceInfo.modeInfoIdx < modes.size()) {
                    const auto &sourceMode = modes[path.sourceInfo.modeInfoIdx].sourceMode;
                    if (sourceMode.position.x == 0 && sourceMode.position.y == 0) {
                        DISPLAYCONFIG_SDR_WHITE_LEVEL_CUSTOM sdrWhite = {};
                        sdrWhite.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL_CUSTOM;
                        sdrWhite.header.size = sizeof(sdrWhite);
                        sdrWhite.header.adapterId = path.targetInfo.adapterId;
                        sdrWhite.header.id = path.targetInfo.id;

                        if (DisplayConfigGetDeviceInfo(&sdrWhite.header) == ERROR_SUCCESS) {
                            if (sdrWhite.SDRWhiteLevel > 0) {
                                // SDRWhiteLevel is a multiplier of 80 nits * 1000
                                // e.g., 1000 => 80 nits, 2500 => 200 nits
                                info.sdrWhiteLevel = (sdrWhite.SDRWhiteLevel / 1000.0f) * 80.0f;
                            }
                        }
                        break; // Stop after finding primary
                    }
                }
            }
        }
    }

    return info;
}
