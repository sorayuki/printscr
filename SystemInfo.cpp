#include "SystemInfo.h"
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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

  return info;
}
