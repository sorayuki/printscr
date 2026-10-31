#include "Bt2446MethodC.h"

#include <algorithm>
#include <cmath>

// 参考工程 D:\dev\obs-dev\obs-bt2446 中方法 C 的默认参数（BT.2446-1 §6.1 示例）：
//   c_alpha = 0.1, c_k1 = 0.83802, c_knee_sdr_nits = 58.5,
//   c_hdr_reference_white_nits = 203, c_sdr_reference_white_percent = 96,
//   hdr_peak_nits = 1000, sdr_peak_nits = 100, c_white_shift = 0（关闭 §6.1.8 高光去色）。
namespace {

constexpr float kDefaultAlpha = 0.1f;
constexpr float kDefaultK1 = 0.83802f;
constexpr float kDefaultKneeSdrNits = 58.5f;
constexpr float kDefaultHdrReferenceWhiteNits = 203.0f;
constexpr float kDefaultSdrReferenceWhitePercent = 96.0f;
constexpr float kDefaultHdrPeakNits = 1000.0f;
constexpr float kDefaultSdrPeakNits = 100.0f;

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Float3 MaxZero(const Float3 &value) {
    return {std::max(value.x, 0.0f), std::max(value.y, 0.0f), std::max(value.z, 0.0f)};
}

Float3 Scale(const Float3 &value, float factor) {
    return {value.x * factor, value.y * factor, value.z * factor};
}

// BT.2446-1 §6.1.2：映射前用串扰矩阵降低饱和度，映射后再用逆矩阵还原
Float3 Crosstalk(const Float3 &color, float alpha) {
    const float diagonal = 1.0f - 2.0f * alpha;
    return {
        diagonal * color.x + alpha * color.y + alpha * color.z,
        alpha * color.x + diagonal * color.y + alpha * color.z,
        alpha * color.x + alpha * color.y + diagonal * color.z
    };
}

Float3 InverseCrosstalk(const Float3 &color, float alpha) {
    const float mean = (color.x + color.y + color.z) / 3.0f;
    const float scale = 1.0f / std::max(1.0f - 3.0f * alpha, 0.01f);
    return {
        (color.x - mean) * scale + mean,
        (color.y - mean) * scale + mean,
        (color.z - mean) * scale + mean
    };
}

Float3 Rec709ToRec2020(const Float3 &color) {
    return {
        0.6274030f * color.x + 0.3292830f * color.y + 0.0433136f * color.z,
        0.0690970f * color.x + 0.9195400f * color.y + 0.0113612f * color.z,
        0.0163916f * color.x + 0.0880132f * color.y + 0.8955950f * color.z
    };
}

Float3 Rec2020ToRec709(const Float3 &color) {
    return {
        1.6604910f * color.x - 0.5876411f * color.y - 0.0728499f * color.z,
        -0.1245505f * color.x + 1.1328999f * color.y - 0.0083494f * color.z,
        -0.0181508f * color.x - 0.1005789f * color.y + 1.1187297f * color.z
    };
}

Float3 Rec2020ToXyz(const Float3 &color) {
    return {
        0.6370f * color.x + 0.1446f * color.y + 0.1689f * color.z,
        0.2627f * color.x + 0.6780f * color.y + 0.0593f * color.z,
        0.0000f * color.x + 0.0281f * color.y + 1.0610f * color.z
    };
}

Float3 XyzToRec2020(const Float3 &color) {
    return {
        1.7167f * color.x - 0.3557f * color.y - 0.2534f * color.z,
        -0.6667f * color.x + 1.6165f * color.y + 0.0158f * color.z,
        0.0176f * color.x - 0.0428f * color.y + 0.9421f * color.z
    };
}

// 对数段与线性段在拐点处连续且斜率一致：
//   k4 = k1 * knee - k2 * ln(1 - k3)
//   k2 = k1 * (1 - k3) * knee
// 剩下的 k3 通过“HDR 参考白映射到指定的 SDR 参考白信号电平”这一约束二分求解。
float SolveK3(float k1, float kneeHdrNits, float hdrReferenceWhiteNits, float sdrReferenceWhitePercent) {
    const float referenceHdr = std::max(hdrReferenceWhiteNits, kneeHdrNits + 0.0001f);
    const float referenceSdr = kDefaultSdrPeakNits * std::pow(sdrReferenceWhitePercent / 100.0f, 2.4f);
    const float target = std::max(referenceSdr - k1 * kneeHdrNits, 0.0001f);

    float low = 0.0001f;
    float high = 0.9999f;
    for (int i = 0; i < 48; ++i) {
        const float mid = 0.5f * (low + high);
        const float k2 = k1 * (1.0f - mid) * kneeHdrNits;
        const float k4 = k1 * kneeHdrNits - k2 * std::log(1.0f - mid);
        const float mapped =
            k2 * std::log(std::max(referenceHdr / kneeHdrNits - mid, 0.0001f)) + k4;
        if (mapped - k1 * kneeHdrNits > target) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return 0.5f * (low + high);
}

class Bt2446MethodCImpl final : public Bt2446MethodC {
public:
    // anchorNits：被映射到 SDR 参考白电平的那个 HDR 亮度。默认参数取值即 HDR 参考白 203 nits；
    // 传入画面实测峰值后，同一条对数肩部会被拉宽，峰值落在 96% 而不是被裁成纯白。
    explicit Bt2446MethodCImpl(float anchorNits) {
        m_params.alpha = kDefaultAlpha;
        m_params.k1 = kDefaultK1;
        m_params.hdrPeakNits = kDefaultHdrPeakNits;
        m_params.sdrPeakNits = kDefaultSdrPeakNits;
        m_params.kneeHdrNits = std::max(kDefaultKneeSdrNits / m_params.k1, 0.0001f);
        m_params.anchorNits = std::clamp(anchorNits, Bt2446MethodC::kMinAnchorNits, Bt2446MethodC::kMaxAnchorNits);
        m_params.k3 =
            SolveK3(m_params.k1, m_params.kneeHdrNits, m_params.anchorNits, kDefaultSdrReferenceWhitePercent);
        m_params.k2 = m_params.k1 * (1.0f - m_params.k3) * m_params.kneeHdrNits;
        m_params.k4 = m_params.k1 * m_params.kneeHdrNits - m_params.k2 * std::log(1.0f - m_params.k3);
    }

    const Bt2446MethodCParams &Params() const override { return m_params; }

    void MapToSdrLinear(float scRgbR, float scRgbG, float scRgbB, float &linearR, float &linearG,
                        float &linearB) const override {
        // scRGB 线性光 → 绝对亮度（cd/m²）→ BT.2020 基色
        const Float3 nits = MaxZero({scRgbR * kScRgbWhiteNits, scRgbG * kScRgbWhiteNits,
                                     scRgbB * kScRgbWhiteNits});
        const Float3 normalized = Scale(Rec709ToRec2020(nits), 1.0f / m_params.hdrPeakNits);
        const Float3 mapped = MapMethodC(normalized);
        const Float3 rec709 = Rec2020ToRec709(mapped);
        linearR = rec709.x;
        linearG = rec709.y;
        linearB = rec709.z;
    }

private:
    // 输入：BT.2020 线性光，1.0 = HDR 峰值；输出：BT.2020 线性光，1.0 = SDR 峰值
    Float3 MapMethodC(const Float3 &normalized2020) const {
        const Float3 crosstalked = Crosstalk(Scale(normalized2020, m_params.hdrPeakNits), m_params.alpha);
        const Float3 xyz = Rec2020ToXyz(crosstalked);

        const float sum = xyz.x + xyz.y + xyz.z;
        if (xyz.y <= 0.0f || sum <= 0.00001f) {
            // 退化的无亮度像素：退化为线性增益，避免后续除以零
            return Scale(normalized2020, m_params.k1 * m_params.hdrPeakNits / m_params.sdrPeakNits);
        }

        const float chromaX = xyz.x / sum;
        const float chromaY = xyz.y / sum;

        float mappedY = 0.0f;
        if (xyz.y < m_params.kneeHdrNits) {
            mappedY = m_params.k1 * xyz.y;
        } else {
            mappedY = m_params.k2 * std::log(std::max(xyz.y / m_params.kneeHdrNits - m_params.k3, 0.0001f)) +
                      m_params.k4;
        }

        const float inverseY = 1.0f / std::max(chromaY, 0.00001f);
        const Float3 xyzSdr = {chromaX * inverseY * mappedY, mappedY, (1.0f - chromaX - chromaY) * inverseY * mappedY};
        const Float3 rec2020Sdr = XyzToRec2020(xyzSdr);

        // 逆串扰还原饱和度后，归一化到 SDR 峰值（1.0 = sdrPeakNits）
        return Scale(InverseCrosstalk(rec2020Sdr, m_params.alpha), 1.0f / m_params.sdrPeakNits);
    }

    Bt2446MethodCParams m_params;
};

} // namespace

std::unique_ptr<Bt2446MethodC> Bt2446MethodC::Create() {
    return std::make_unique<Bt2446MethodCImpl>(kDefaultHdrReferenceWhiteNits);
}

std::unique_ptr<Bt2446MethodC> Bt2446MethodC::CreateForHdrPeak(float hdrPeakNits) {
    return std::make_unique<Bt2446MethodCImpl>(hdrPeakNits);
}
