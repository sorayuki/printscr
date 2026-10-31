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
    float hdrPeakNits = 0.0f; // 方法假设的 HDR 峰值（曲线内部前后抵消，仅作约定）
    float sdrPeakNits = 0.0f; // 方法假设的 SDR 峰值
    // 映射锚点：该亮度的内容被映射到 SDR 参考白电平（默认 96%）。k2/k3/k4 由它求解，
    // 是峰值唯一真正影响曲线的地方。
    float anchorNits = 0.0f;
};

// scRGB 线性光里 1.0 对应的绝对亮度（Windows 进阶色彩约定）
inline constexpr float kScRgbWhiteNits = 80.0f;

// BT.2446-1 方法 C 的 HDR→SDR 映射，使用报告示例的默认参数。
//
// 坐标系约定：
//   输入  —— scRGB / BT.709 基色的线性光，1.0 = 80 cd/m²（与 Windows 抓屏一致）；
//   输出  —— BT.709 基色的线性光，1.0 = Params().sdrPeakNits（默认 100 cd/m²）。
// 输出仍是线性光，8bit 输出前还需要做一次 sRGB/BT.1886 逆 EOTF 编码。
//
// 关于“HDR 峰值”：参考实现里 hdr_peak_nits 是先把输入除过去、进曲线后又乘回来，
// 对方法 C 的映射结果没有任何影响；真正决定曲线形状的是 k2/k3/k4，而它们由
// “某个亮度的内容映射到 96% SDR 信号电平”这一约束二分求解，默认该亮度是
// HDR 参考白 203 nits。想让画面实测峰值参与，必须改这个锚点，见 CreateForHdrPeak。
class Bt2446MethodC {
public:
    virtual ~Bt2446MethodC() = default;

    virtual const Bt2446MethodCParams &Params() const = 0;

    // 单像素映射；输入按 scRGB 线性光解释，输出为 BT.709 线性光（1.0 = SDR 峰值）
    virtual void MapToSdrLinear(float scRgbR, float scRgbG, float scRgbB, float &linearR, float &linearG,
                                float &linearB) const = 0;

    // 锚点下限：HDR 参考白。峰值低于它时曲线与默认参数完全一致（不会把纯 SDR 画面压暗）。
    static constexpr float kMinAnchorNits = 203.0f;
    // 锚点上限：防止个别异常像素把整幅画面压平。
    static constexpr float kMaxAnchorNits = 4000.0f;

    // 方法默认参数：锚点 = HDR 参考白 203 nits。
    static std::unique_ptr<Bt2446MethodC> Create();

    // 以画面实测峰值作锚点：该亮度映射到 96% SDR 信号，对数肩部随之展宽，
    // 高光在线性 1.0 之前就被压进 SDR 范围内而不再整片裁成纯白。
    // 线性段（< 69.8 nits）与 k1 不变，因此暗部/中间调行为与默认参数一致。
    static std::unique_ptr<Bt2446MethodC> CreateForHdrPeak(float hdrPeakNits);
};
