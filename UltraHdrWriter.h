#pragma once

#include "ScreenCapture.h"

#include <cstdint>
#include <string>
#include <vector>

// printscr.ini 里 [output] 段的编码参数
struct UltraHdrEncodeOptions {
    int   baseQuality = 95;             // SDR 基础图层的 JPEG 质量（0~100）
    int   gainmapQuality = 90;          // 增益图的 JPEG 质量（0~100）
    int   gainmapScaleFactor = 4;       // 增益图相对基础图的下采样倍数
    int   multiChannelGainmap = 1;      // 1 = 三分量增益图
    float gainmapGamma = 1.0f;          // 增益图编码 gamma
    float targetDisplayPeakNits = 0.0f; // 目标显示峰值亮度；0 = 用显示器实测 HDR 峰值
    bool  bestQualityPreset = true;     // true = best_quality，false = realtime
};

// 从 printscr.ini 读取编码参数；文件缺失或键缺失时使用默认值
UltraHdrEncodeOptions LoadUltraHdrEncodeOptions();

// 弹出“保存 Ultra HDR 截图”对话框；返回空字符串表示用户取消
std::wstring ShowUltraHdrSaveDialog();

// 写成 Ultra HDR JPEG（JPEG + gain map）：
// 基础图层直接用 sdrBgra（与剪贴板内容逐像素一致），增益图由 capturedFrame 里同一区域的
// scRGB 数据生成，因此 HDR 显示端还原出来的是原始截取画面。
// sourceLeft/sourceTop 为选区在 capturedFrame 中的左上角坐标，width/height 为选区尺寸。
bool WriteUltraHdrFile(const std::wstring &path, const std::vector<uint8_t> &sdrBgra, uint32_t width,
                       uint32_t height, const CapturedFrame &capturedFrame, int sourceLeft, int sourceTop,
                       float displayPeakNits, const UltraHdrEncodeOptions &options);
