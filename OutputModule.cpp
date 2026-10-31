#include "OutputModule.h"
#include "Bt2446MethodC.h"
#include "GpuFrame.h"
#include "Logger.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {

constexpr float kMinLw = 0.000001f;
constexpr float kDefaultLw = 1.0f;
constexpr GLuint kLocalSizeX = 16;
constexpr GLuint kLocalSizeY = 16;

constexpr const char *kDetectionShaderSource = R"(#version 310 es
precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0) uniform highp sampler2D u_source;

layout(std430, binding = 0) buffer DetectionBuffer {
    uint peakBits; // 非负 float 的位模式按 uint 比较时保持大小顺序
} u_detection;

uniform ivec2 u_selectionOrigin;
uniform ivec2 u_outputSize;

const uint kGroupSize = 256u; // local_size_x * local_size_y
shared float s_peak[kGroupSize];

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    uint lid = gl_LocalInvocationIndex;

    // 越界线程不能提前 return，所有线程都必须到达 barrier
    float peak = 0.0;
    if (gid.x < u_outputSize.x && gid.y < u_outputSize.y) {
        vec3 color = texelFetch(u_source, u_selectionOrigin + gid, 0).rgb;
        peak = max(max(color.r, color.g), color.b);
        if (!(peak > 0.0)) {
            peak = 0.0; // 过滤负值与 NaN
        }
        peak = min(peak, 65504.0);
    }
    s_peak[lid] = peak;
    barrier();

    // workgroup 内树形归约，每组只做一次全局原子操作
    for (uint stride = kGroupSize / 2u; stride > 0u; stride >>= 1u) {
        if (lid < stride) {
            s_peak[lid] = max(s_peak[lid], s_peak[lid + stride]);
        }
        barrier();
    }

    if (lid == 0u && s_peak[0] > 0.0) {
        atomicMax(u_detection.peakBits, floatBitsToUint(s_peak[0]));
    }
}
)";

constexpr const char *kProcessingShaderSource = R"(#version 310 es
precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0) uniform highp sampler2D u_source;

layout(std430, binding = 0) buffer OutputBuffer {
    uint pixels[];
} u_output;

uniform ivec2 u_selectionOrigin;
uniform ivec2 u_outputSize;
// 系统 SDR 白点，单位 scRGB（1.0 = 80 cd/m²）；用于判断内容是否真的含有 HDR 高光
uniform float u_lw;
uniform bool u_passthroughSdr;
uniform bool u_toneMap;

// BT.2446-1 方法 C 参数（CPU 侧按方法默认参数求解后传入）
uniform float u_alpha;
uniform float u_k1;
uniform float u_k2;
uniform float u_k3;
uniform float u_k4;
uniform float u_kneeHdrNits;
uniform float u_hdrPeakNits;
uniform float u_sdrPeakNits;

const float kScRgbWhiteNits = 80.0;
const float kSrgbLinearThreshold = 0.0031308;
const float kSrgbLowSlope = 12.92;
const float kSrgbHighScale = 1.055;
const float kSrgbHighOffset = 0.055;

vec3 Rec709ToRec2020(vec3 color) {
    return vec3(
        0.6274030 * color.r + 0.3292830 * color.g + 0.0433136 * color.b,
        0.0690970 * color.r + 0.9195400 * color.g + 0.0113612 * color.b,
        0.0163916 * color.r + 0.0880132 * color.g + 0.8955950 * color.b
    );
}

vec3 Rec2020ToRec709(vec3 color) {
    return vec3(
        1.6604910 * color.r - 0.5876411 * color.g - 0.0728499 * color.b,
        -0.1245505 * color.r + 1.1328999 * color.g - 0.0083494 * color.b,
        -0.0181508 * color.r - 0.1005789 * color.g + 1.1187297 * color.b
    );
}

vec3 Rec2020ToXyz(vec3 color) {
    return vec3(
        0.6370 * color.r + 0.1446 * color.g + 0.1689 * color.b,
        0.2627 * color.r + 0.6780 * color.g + 0.0593 * color.b,
        0.0000 * color.r + 0.0281 * color.g + 1.0610 * color.b
    );
}

vec3 XyzToRec2020(vec3 color) {
    return vec3(
        1.7167 * color.r - 0.3557 * color.g - 0.2534 * color.b,
        -0.6667 * color.r + 1.6165 * color.g + 0.0158 * color.b,
        0.0176 * color.r - 0.0428 * color.g + 0.9421 * color.b
    );
}

// §6.1.2：映射前串扰降饱和，映射后用逆矩阵还原
vec3 Crosstalk(vec3 color, float alpha) {
    float diagonal = 1.0 - 2.0 * alpha;
    return vec3(
        diagonal * color.r + alpha * color.g + alpha * color.b,
        alpha * color.r + diagonal * color.g + alpha * color.b,
        alpha * color.r + alpha * color.g + diagonal * color.b
    );
}

vec3 InverseCrosstalk(vec3 color, float alpha) {
    float mean = (color.r + color.g + color.b) / 3.0;
    float scale = 1.0 / max(1.0 - 3.0 * alpha, 0.01);
    return (color - vec3(mean)) * scale + vec3(mean);
}

// 输入：BT.2020 线性光，1.0 = HDR 峰值；输出：BT.2020 线性光，1.0 = SDR 峰值
vec3 MapMethodC(vec3 normalized2020) {
    vec3 crosstalked = Crosstalk(normalized2020 * u_hdrPeakNits, u_alpha);
    vec3 xyz = Rec2020ToXyz(crosstalked);
    float sum = xyz.x + xyz.y + xyz.z;
    if (xyz.y <= 0.0 || sum <= 0.00001) {
        // 退化的无亮度像素：退化为线性增益，避免后续除以零
        return normalized2020 * (u_k1 * u_hdrPeakNits / u_sdrPeakNits);
    }

    vec2 xy = xyz.xy / sum;

    float mappedY;
    if (xyz.y < u_kneeHdrNits) {
        mappedY = u_k1 * xyz.y;
    } else {
        mappedY = u_k2 * log(max(xyz.y / u_kneeHdrNits - u_k3, 0.0001)) + u_k4;
    }

    float inverseY = 1.0 / max(xy.y, 0.00001);
    vec3 xyzSdr = vec3(xy.x * inverseY * mappedY, mappedY, (1.0 - xy.x - xy.y) * inverseY * mappedY);
    vec3 rec2020Sdr = XyzToRec2020(xyzSdr);

    // 逆串扰还原饱和度后，归一化到 SDR 峰值（1.0 = u_sdrPeakNits）
    return InverseCrosstalk(rec2020Sdr, u_alpha) / u_sdrPeakNits;
}

float LinearToSrgb(float linearValue) {
    linearValue = clamp(linearValue, 0.0, 1.0);
    if (linearValue <= kSrgbLinearThreshold) {
        return linearValue * kSrgbLowSlope;
    }
    return kSrgbHighScale * pow(linearValue, 1.0 / 2.4) - kSrgbHighOffset;
}

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    if (gid.x >= u_outputSize.x || gid.y >= u_outputSize.y) {
        return;
    }

    int pixelIndex = gid.y * u_outputSize.x + gid.x;
    vec3 color = max(texelFetch(u_source, u_selectionOrigin + gid, 0).rgb, vec3(0.0));
    vec3 outputColor;

    if (u_passthroughSdr) {
        outputColor = clamp(color, vec3(0.0), vec3(1.0));
    } else if (!u_toneMap) {
        // 选区里没有超过 SDR 白点的内容：按纯 SDR 缩放，保持原样输出
        vec3 normalized = clamp(color / max(u_lw, 0.000001), vec3(0.0), vec3(1.0));
        outputColor = vec3(
            LinearToSrgb(normalized.r),
            LinearToSrgb(normalized.g),
            LinearToSrgb(normalized.b)
        );
    } else {
        // scRGB 线性光 → 绝对亮度（1.0 = 80 cd/m²）→ BT.2020 基色的线性光
        vec3 nits = color * kScRgbWhiteNits;
        vec3 normalized2020 = Rec709ToRec2020(nits) / u_hdrPeakNits;
        // 方法 C 输出 0~1 的线性 SDR（1.0 = SDR 峰值 = 100 cd/m²）。
        // 高光不做任何按内容峰值的重映射：对数肩部之上（约 331 cd/m²）的线性值会超过 1.0，
        // 与参考实现写入 SDR 目标时的行为一致，直接在 SDR 边界裁剪。
        vec3 sdrLinear = clamp(Rec2020ToRec709(MapMethodC(normalized2020)), vec3(0.0), vec3(1.0));
        // 参考实现的输出是线性光，本项目写的是 8bit 编码值，故此处补一次 sRGB OETF
        outputColor = vec3(
            LinearToSrgb(sdrLinear.r),
            LinearToSrgb(sdrLinear.g),
            LinearToSrgb(sdrLinear.b)
        );
    }

    u_output.pixels[pixelIndex] = packUnorm4x8(vec4(
        outputColor.b,
        outputColor.g,
        outputColor.r,
        1.0
    ));
}
)";

SelectionRect ClampSelectionToFrame(const SelectionRect &selection, uint32_t frameWidth, uint32_t frameHeight) {
    SelectionRect clamped = selection;
    clamped.x1 = std::clamp(clamped.x1, 0, static_cast<int>(frameWidth));
    clamped.x2 = std::clamp(clamped.x2, 0, static_cast<int>(frameWidth));
    clamped.y1 = std::clamp(clamped.y1, 0, static_cast<int>(frameHeight));
    clamped.y2 = std::clamp(clamped.y2, 0, static_cast<int>(frameHeight));
    return clamped;
}

void WriteBitmapToClipboard(const std::vector<uint8_t> &bgraPixels, int width, int height) {
    const SIZE_T headerSize = sizeof(BITMAPV5HEADER);
    const SIZE_T pixelBytes = static_cast<SIZE_T>(width) * static_cast<SIZE_T>(height) * 4;
    HGLOBAL dibMemory = GlobalAlloc(GMEM_MOVEABLE, headerSize + pixelBytes);
    if (!dibMemory) {
        throw std::runtime_error("GlobalAlloc failed for clipboard bitmap");
    }

    void *memory = GlobalLock(dibMemory);
    if (!memory) {
        GlobalFree(dibMemory);
        throw std::runtime_error("GlobalLock failed for clipboard bitmap");
    }

    auto *header = static_cast<BITMAPV5HEADER *>(memory);
    std::memset(header, 0, headerSize);
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = width;
    header->bV5Height = -height;
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_sRGB;

    auto *dstPixels = reinterpret_cast<uint8_t *>(header + 1);
    std::memcpy(dstPixels, bgraPixels.data(), pixelBytes);
    GlobalUnlock(dibMemory);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(dibMemory);
        throw std::runtime_error("OpenClipboard failed");
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(dibMemory);
        throw std::runtime_error("EmptyClipboard failed");
    }

    if (!SetClipboardData(CF_DIBV5, dibMemory)) {
        CloseClipboard();
        GlobalFree(dibMemory);
        throw std::runtime_error("SetClipboardData failed");
    }

    CloseClipboard();
}

std::string DescribeEglError(EGLint error) {
    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    default:
        return "Unknown EGL error";
    }
}

std::string GetShaderInfoLog(GLuint shader) {
    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength <= 1) {
        return {};
    }

    std::string infoLog(static_cast<size_t>(logLength), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, infoLog.data());
    return infoLog;
}

std::string GetProgramInfoLog(GLuint program) {
    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength <= 1) {
        return {};
    }

    std::string infoLog(static_cast<size_t>(logLength), '\0');
    glGetProgramInfoLog(program, logLength, nullptr, infoLog.data());
    return infoLog;
}

float ComputeLw(float sdrWhiteNits) {
    if (sdrWhiteNits <= 0.0f) {
        return kDefaultLw;
    }
    return (std::max)(sdrWhiteNits / kScRgbWhiteNits, kMinLw);
}

float ResolveSdrWhiteNits(float sdrWhiteNits) {
    if (sdrWhiteNits < (kMinLw * kScRgbWhiteNits)) {
        return kScRgbWhiteNits;
    }
    return sdrWhiteNits;
}

GLuint CompileComputeProgram(const char *shaderSource) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &shaderSource, nullptr);
    glCompileShader(shader);

    GLint compileStatus = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus != GL_TRUE) {
        const std::string infoLog = GetShaderInfoLog(shader);
        glDeleteShader(shader);
        throw std::runtime_error("Failed to compile compute shader: " + infoLog);
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);

    GLint linkStatus = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus != GL_TRUE) {
        const std::string infoLog = GetProgramInfoLog(program);
        glDeleteProgram(program);
        throw std::runtime_error("Failed to link compute shader program: " + infoLog);
    }

    return program;
}

class OutputModuleImpl final : public OutputModule {
public:
    OutputModuleImpl(EGLDisplay display, EGLSurface dummySurface, EGLContext context)
        : m_display(display), m_surface(dummySurface), m_context(context) {
        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            throw std::runtime_error("OutputModuleImpl: eglMakeCurrent failed during init");
        }
        m_detectProgram  = CompileComputeProgram(kDetectionShaderSource);
        m_processProgram = CompileComputeProgram(kProcessingShaderSource);
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        m_methodC = Bt2446MethodC::Create();
        const Bt2446MethodCParams &params = m_methodC->Params();
        LOG("BT.2446-1 method C (default): alpha=" + std::to_string(params.alpha) +
            ", k1=" + std::to_string(params.k1) + ", k2=" + std::to_string(params.k2) +
            ", k3=" + std::to_string(params.k3) + ", k4=" + std::to_string(params.k4) +
            ", knee=" + std::to_string(params.kneeHdrNits) + " nits, HDR peak=" +
            std::to_string(params.hdrPeakNits) + " nits, SDR peak=" + std::to_string(params.sdrPeakNits) + " nits");
    }

    ~OutputModuleImpl() {
        if (eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            if (m_detectProgram  != 0) glDeleteProgram(m_detectProgram);
            if (m_processProgram != 0) glDeleteProgram(m_processProgram);
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        } else {
            LOG("OutputModuleImpl: eglMakeCurrent failed during destroy, shader program might leak");
        }
    }

    void CopySelectionToClipboard(const GpuFrame &gpuFrame, const SelectionRect &selection,
                                  const DisplayHdrInfo &hdrInfo) override {
        const SelectionRect clampedSelection = ClampSelectionToFrame(selection, gpuFrame.Width(), gpuFrame.Height());
        if (!clampedSelection.IsValid()) {
            throw std::runtime_error("Selection is empty after clamping");
        }

        const int   outputWidth   = clampedSelection.Width();
        const int   outputHeight  = clampedSelection.Height();
        const float sdrWhiteNits  = ResolveSdrWhiteNits(hdrInfo.sdrWhiteLevel);

        LOG("Copying selection to clipboard via compute shader. Rect=(" + std::to_string(clampedSelection.Left()) +
            "," + std::to_string(clampedSelection.Top()) + ")-(" + std::to_string(clampedSelection.Right()) + "," +
            std::to_string(clampedSelection.Bottom()) + "), SDR white=" + std::to_string(sdrWhiteNits));

        std::vector<uint8_t> bgraPixels = ConvertSelection(gpuFrame, clampedSelection, sdrWhiteNits);
        WriteBitmapToClipboard(bgraPixels, outputWidth, outputHeight);
        LOG("Selection copied to clipboard as 8-bit bitmap from SSBO output.");
    }

private:
    std::vector<uint8_t> ConvertSelection(const GpuFrame &gpuFrame, const SelectionRect &selection, float sdrWhiteNits) {
        struct ScopedBuffer {
            GLuint id = 0;
            ~ScopedBuffer() { if (id != 0) glDeleteBuffers(1, &id); }
        };

        const GLsizei outputWidth  = static_cast<GLsizei>(selection.Width());
        const GLsizei outputHeight = static_cast<GLsizei>(selection.Height());
        const size_t  outputPixels = static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight);
        const float   lw           = ComputeLw(sdrWhiteNits);
        const GLuint  dispatchX    = (static_cast<GLuint>(outputWidth)  + kLocalSizeX - 1) / kLocalSizeX;
        const GLuint  dispatchY    = (static_cast<GLuint>(outputHeight) + kLocalSizeY - 1) / kLocalSizeY;

        std::vector<uint8_t> bgraPixels(outputPixels * 4);

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            throw std::runtime_error("ConvertSelection: eglMakeCurrent failed");
        }

        const GLuint sourceTexture = gpuFrame.GetTextureId();

        ScopedBuffer detectionBuffer;
        ScopedBuffer outputBuffer;

        const uint32_t detectionInit = 0;
        glGenBuffers(1, &detectionBuffer.id);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, detectionBuffer.id);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(detectionInit), &detectionInit, GL_DYNAMIC_COPY);

        glUseProgram(m_detectProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glUniform1i(glGetUniformLocation(m_detectProgram, "u_source"), 0);
        glUniform2i(glGetUniformLocation(m_detectProgram, "u_selectionOrigin"), selection.Left(), selection.Top());
        glUniform2i(glGetUniformLocation(m_detectProgram, "u_outputSize"), outputWidth, outputHeight);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, detectionBuffer.id);
        glDispatchCompute(dispatchX, dispatchY, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, detectionBuffer.id);
        auto *mappedDetection = static_cast<const uint32_t *>(
            glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(detectionInit), GL_MAP_READ_BIT));
        if (!mappedDetection) {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            throw std::runtime_error("Failed to map detection SSBO");
        }
        const bool usePassThroughSdr = (gpuFrame.GetFormat() == GpuFrameFormat::Sdr8);
        float selectionPeak = 0.0f;
        std::memcpy(&selectionPeak, mappedDetection, sizeof(selectionPeak));
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        const bool useToneMap = !usePassThroughSdr && selectionPeak > lw * 1.01f; // 容差

        glGenBuffers(1, &outputBuffer.id);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, outputBuffer.id);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(outputPixels * sizeof(uint32_t)), nullptr,
                     GL_DYNAMIC_COPY);

        const Bt2446MethodCParams &params = m_methodC->Params();

        glUseProgram(m_processProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glUniform1i(glGetUniformLocation(m_processProgram, "u_source"), 0);
        glUniform2i(glGetUniformLocation(m_processProgram, "u_selectionOrigin"), selection.Left(), selection.Top());
        glUniform2i(glGetUniformLocation(m_processProgram, "u_outputSize"), outputWidth, outputHeight);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_lw"), lw);
        glUniform1i(glGetUniformLocation(m_processProgram, "u_passthroughSdr"), usePassThroughSdr ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_processProgram, "u_toneMap"), useToneMap ? 1 : 0);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_alpha"), params.alpha);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_k1"), params.k1);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_k2"), params.k2);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_k3"), params.k3);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_k4"), params.k4);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_kneeHdrNits"), params.kneeHdrNits);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_hdrPeakNits"), params.hdrPeakNits);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_sdrPeakNits"), params.sdrPeakNits);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outputBuffer.id);
        glDispatchCompute(dispatchX, dispatchY, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, outputBuffer.id);
        auto *mappedPixels = static_cast<const uint32_t *>(
            glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                             static_cast<GLsizeiptr>(outputPixels * sizeof(uint32_t)), GL_MAP_READ_BIT));
        if (!mappedPixels) {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            throw std::runtime_error("Failed to map output SSBO");
        }
        std::memcpy(bgraPixels.data(), mappedPixels, outputPixels * sizeof(uint32_t));
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (usePassThroughSdr) {
            LOG("Compute shader output path selected: SDR 8bit passthrough");
        } else if (useToneMap) {
            LOG("Compute shader output path selected: BT.2446-1 method C (default parameters). "
                "Selection peak=" + std::to_string(selectionPeak * kScRgbWhiteNits) +
                " nits, HDR reference white 203 nits maps to 96% SDR signal, output encoded as sRGB.");
        } else {
            LOG("Compute shader output path selected: linear-sRGB");
        }
        return bgraPixels;
    }

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;
    GLuint m_detectProgram  = 0;
    GLuint m_processProgram = 0;
    std::unique_ptr<Bt2446MethodC> m_methodC;
};

} // namespace

std::unique_ptr<OutputModule> OutputModule::Create(EGLDisplay display, EGLSurface dummySurface, EGLContext context) { return std::make_unique<OutputModuleImpl>(display, dummySurface, context); }

