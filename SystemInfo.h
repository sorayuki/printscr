#pragma once

struct DisplayHdrInfo {
  float sdrWhiteLevel;         // in nits, e.g. 200.0f
  float peakBrightness;        // in nits, e.g. 1000.0f
  float minLuminance;          // in nits
  float maxLuminance;          // in nits
  float maxFullFrameLuminance; // in nits
};

class SystemInfo {
public:
  // Gets HDR info for the primary display
  static DisplayHdrInfo GetPrimaryDisplayHdrInfo();
};
