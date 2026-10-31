#pragma once

#include <memory>

// ITU-R BT.2446-1 §6.1 方法 C（可调肤色映射）的参数与求解结果。
// 所有亮度单位均为 cd/m²（nits），曲线在“线性显示光”域上工作。
struct Bt2446MethodCParams {
    float alpha = 0.0f;       // 串扰矩阵强度
    float k1 = 0.0f;          // 线性段增益
    float k2 = 0.0f;          // 对数段系数
    float k3 = 0.0f;          // 对数段偏移
    float k4 = 0.0f;          // 对数段偏置
    float kneeHdrNits = 0.0f; // HDR 侧拐点亮度
    float hdrPeakNits = 0.0f; // 方法假设的 HDR 峰值
    float sdrPeakNits = 0.0f; // 方法假设的 SDR 峰值
};

// scRGB 线性光里 1.0 对应的绝对亮度（Windows 进阶色彩约定）
inline constexpr float kScRgbWhiteNits = 80.0f;

// BT.2446-1 方法 C 的 HDR→SDR 映射，使用报告示例的默认参数。
//
// 坐标系约定：
//   输入  —— scRGB / BT.709 基色的线性光，1.0 = 80 cd/m²（与 Windows 抓屏一致）；
//   输出  —— BT.709 基色的线性光，1.0 = Params().sdrPeakNits（默认 100 cd/m²）。
// 输出仍是线性光，8bit 输出前还需要做一次 sRGB/BT.1886 逆 EOTF 编码。
class Bt2446MethodC {
public:
    virtual ~Bt2446MethodC() = default;

    virtual const Bt2446MethodCParams &Params() const = 0;

    // 单像素映射；输入按 scRGB 线性光解释，输出为 BT.709 线性光（1.0 = SDR 峰值）
    virtual void MapToSdrLinear(float scRgbR, float scRgbG, float scRgbB, float &linearR, float &linearG,
                                float &linearB) const = 0;

    static std::unique_ptr<Bt2446MethodC> Create();
};
