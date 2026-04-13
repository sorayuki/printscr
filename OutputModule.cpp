#include "OutputModule.h"
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

constexpr float kBt1886Gamma = 2.4f;
constexpr float kReferencePeakNits = 1000.0f;
constexpr float kSdrReferenceWhiteNits = 80.0f;
constexpr float kMinLw = 0.000001f;
constexpr GLuint kLocalSizeX = 16;
constexpr GLuint kLocalSizeY = 16;

constexpr const char *kDetectionShaderSource = R"(#version 310 es
precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0) uniform highp sampler2D u_source;

layout(std430, binding = 0) buffer DetectionBuffer {
    uint foundHighlight;
} u_detection;

uniform ivec2 u_selectionOrigin;
uniform ivec2 u_outputSize;
uniform float u_lw;

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    if (gid.x >= u_outputSize.x || gid.y >= u_outputSize.y) {
        return;
    }

    vec3 color = texelFetch(u_source, u_selectionOrigin + gid, 0).rgb;
    if (any(greaterThan(color, vec3(u_lw)))) {
        atomicOr(u_detection.foundHighlight, 1u);
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
uniform float u_sdrWhiteNits;
uniform float u_lw;
uniform bool u_useHlgPath;

const float kBt1886Gamma = 2.4;
const float kHlgA = 0.17883277;
const float kHlgB = 1.0 - 4.0 * kHlgA;
const float kHlgC = 0.55991073;
const float kReferencePeakNits = 1000.0;
const float kSrgbLinearThreshold = 0.0031308;
const float kSrgbLowSlope = 12.92;
const float kSrgbHighScale = 1.055;
const float kSrgbHighOffset = 0.055;

vec3 SrgbLinearToBt2020Linear(vec3 color) {
    return vec3(
        0.6274040 * color.r + 0.3292820 * color.g + 0.0433136 * color.b,
        0.0690970 * color.r + 0.9195400 * color.g + 0.0113612 * color.b,
        0.0163916 * color.r + 0.0880132 * color.g + 0.8955950 * color.b
    );
}

vec3 Bt2020LinearToBt709Linear(vec3 color) {
    return vec3(
        1.6604910 * color.r - 0.5876411 * color.g - 0.0728499 * color.b,
        -0.1245505 * color.r + 1.1328999 * color.g - 0.0083494 * color.b,
        -0.0181508 * color.r - 0.1005789 * color.g + 1.1187297 * color.b
    );
}

float Clamp01(float value) {
    return clamp(value, 0.0, 1.0);
}

float HlgOetf(float linearValue) {
    linearValue = Clamp01(linearValue);
    if (linearValue <= (1.0 / 12.0)) {
        return sqrt(3.0 * linearValue);
    }
    return kHlgA * log(12.0 * linearValue - kHlgB) + kHlgC;
}

float Bt1886Eotf(float signalValue) {
    return pow(Clamp01(signalValue), kBt1886Gamma);
}

float Bt1886Oetf(float linearValue) {
    return pow(Clamp01(linearValue), 1.0 / kBt1886Gamma);
}

float LinearToSrgb(float linearValue) {
    linearValue = Clamp01(linearValue);
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

    if (u_useHlgPath) {
        vec3 bt2020Linear = SrgbLinearToBt2020Linear(color);
        vec3 hlg = vec3(
            HlgOetf(max(bt2020Linear.r, 0.0) * u_sdrWhiteNits / kReferencePeakNits),
            HlgOetf(max(bt2020Linear.g, 0.0) * u_sdrWhiteNits / kReferencePeakNits),
            HlgOetf(max(bt2020Linear.b, 0.0) * u_sdrWhiteNits / kReferencePeakNits)
        );
        vec3 interpretedLinear = vec3(
            Bt1886Eotf(hlg.r),
            Bt1886Eotf(hlg.g),
            Bt1886Eotf(hlg.b)
        );
        vec3 bt709Linear = Bt2020LinearToBt709Linear(interpretedLinear);
        outputColor = vec3(
            Bt1886Oetf(bt709Linear.r),
            Bt1886Oetf(bt709Linear.g),
            Bt1886Oetf(bt709Linear.b)
        );
    } else {
        vec3 normalized = clamp(color / u_lw, vec3(0.0), vec3(1.0));
        outputColor = vec3(
            LinearToSrgb(normalized.r),
            LinearToSrgb(normalized.g),
            LinearToSrgb(normalized.b)
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

SelectionRect ClampSelectionToFrame(const SelectionRect &selection, const FrameMetadata &metadata) {
    SelectionRect clamped = selection;
    clamped.x1 = std::clamp(clamped.x1, 0, static_cast<int>(metadata.width));
    clamped.x2 = std::clamp(clamped.x2, 0, static_cast<int>(metadata.width));
    clamped.y1 = std::clamp(clamped.y1, 0, static_cast<int>(metadata.height));
    clamped.y2 = std::clamp(clamped.y2, 0, static_cast<int>(metadata.height));
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
        return 1.0f;
    }
    return std::max(sdrWhiteNits / kSdrReferenceWhiteNits, kMinLw);
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

std::vector<uint8_t> MakeTextureUploadData(const CapturedFrame &frame) {
    const size_t tightRowPitch = static_cast<size_t>(frame.metadata.width) * sizeof(uint16_t) * 4;
    const size_t totalBytes = static_cast<size_t>(frame.metadata.rowPitch) * static_cast<size_t>(frame.metadata.height);
    if (frame.pixelData.size() < totalBytes) {
        throw std::runtime_error("Captured frame buffer is smaller than expected");
    }

    if (frame.metadata.rowPitch == tightRowPitch) {
        return frame.pixelData;
    }

    std::vector<uint8_t> packed(static_cast<size_t>(frame.metadata.height) * tightRowPitch);
    for (uint32_t row = 0; row < frame.metadata.height; ++row) {
        const uint8_t *src = frame.pixelData.data() + static_cast<size_t>(row) * frame.metadata.rowPitch;
        uint8_t *dst = packed.data() + static_cast<size_t>(row) * tightRowPitch;
        std::memcpy(dst, src, tightRowPitch);
    }
    return packed;
}

class GpuOutputProcessor {
public:
    GpuOutputProcessor() { Initialize(); }

    ~GpuOutputProcessor() {
        if (m_detectProgram != 0) {
            glDeleteProgram(m_detectProgram);
        }
        if (m_processProgram != 0) {
            glDeleteProgram(m_processProgram);
        }

        if (m_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_surface != EGL_NO_SURFACE) {
                eglDestroySurface(m_display, m_surface);
            }
            if (m_context != EGL_NO_CONTEXT) {
                eglDestroyContext(m_display, m_context);
            }
            eglTerminate(m_display);
        }
    }

    std::vector<uint8_t> ConvertSelection(const CapturedFrame &frame, const SelectionRect &selection, float sdrWhiteNits) {
        const GLsizei frameWidth = static_cast<GLsizei>(frame.metadata.width);
        const GLsizei frameHeight = static_cast<GLsizei>(frame.metadata.height);
        const GLsizei outputWidth = static_cast<GLsizei>(selection.Width());
        const GLsizei outputHeight = static_cast<GLsizei>(selection.Height());
        const size_t outputPixels = static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight);
        const float lw = ComputeLw(sdrWhiteNits);
        std::vector<uint8_t> bgraPixels(outputPixels * 4);

        const std::vector<uint8_t> uploadPixels = MakeTextureUploadData(frame);

        GLuint sourceTexture = 0;
        GLuint detectionBuffer = 0;
        GLuint outputBuffer = 0;

        glGenTextures(1, &sourceTexture);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, frameWidth, frameHeight, 0, GL_RGBA, GL_HALF_FLOAT,
                     uploadPixels.data());

        uint32_t detectionFlag = 0;
        glGenBuffers(1, &detectionBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, detectionBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(detectionFlag), &detectionFlag, GL_DYNAMIC_COPY);

        glUseProgram(m_detectProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glUniform1i(glGetUniformLocation(m_detectProgram, "u_source"), 0);
        glUniform2i(glGetUniformLocation(m_detectProgram, "u_selectionOrigin"), selection.Left(), selection.Top());
        glUniform2i(glGetUniformLocation(m_detectProgram, "u_outputSize"), outputWidth, outputHeight);
        glUniform1f(glGetUniformLocation(m_detectProgram, "u_lw"), lw);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, detectionBuffer);
        glDispatchCompute((outputWidth + static_cast<GLsizei>(kLocalSizeX) - 1) / static_cast<GLsizei>(kLocalSizeX),
                          (outputHeight + static_cast<GLsizei>(kLocalSizeY) - 1) / static_cast<GLsizei>(kLocalSizeY), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, detectionBuffer);
        auto *mappedDetection = static_cast<const uint32_t *>(
            glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t), GL_MAP_READ_BIT));
        if (!mappedDetection) {
            glDeleteBuffers(1, &detectionBuffer);
            glDeleteTextures(1, &sourceTexture);
            throw std::runtime_error("Failed to map detection SSBO");
        }
        const bool useHlgPath = (*mappedDetection != 0u);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

        glGenBuffers(1, &outputBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, outputBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(outputPixels * sizeof(uint32_t)), nullptr,
                     GL_DYNAMIC_COPY);

        glUseProgram(m_processProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glUniform1i(glGetUniformLocation(m_processProgram, "u_source"), 0);
        glUniform2i(glGetUniformLocation(m_processProgram, "u_selectionOrigin"), selection.Left(), selection.Top());
        glUniform2i(glGetUniformLocation(m_processProgram, "u_outputSize"), outputWidth, outputHeight);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_sdrWhiteNits"), sdrWhiteNits);
        glUniform1f(glGetUniformLocation(m_processProgram, "u_lw"), lw);
        glUniform1i(glGetUniformLocation(m_processProgram, "u_useHlgPath"), useHlgPath ? 1 : 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outputBuffer);
        glDispatchCompute((outputWidth + static_cast<GLsizei>(kLocalSizeX) - 1) / static_cast<GLsizei>(kLocalSizeX),
                          (outputHeight + static_cast<GLsizei>(kLocalSizeY) - 1) / static_cast<GLsizei>(kLocalSizeY), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, outputBuffer);
        auto *mappedPixels = static_cast<const uint32_t *>(
            glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                             static_cast<GLsizeiptr>(outputPixels * sizeof(uint32_t)), GL_MAP_READ_BIT));
        if (!mappedPixels) {
            glDeleteBuffers(1, &outputBuffer);
            glDeleteBuffers(1, &detectionBuffer);
            glDeleteTextures(1, &sourceTexture);
            throw std::runtime_error("Failed to map output SSBO");
        }
        std::memcpy(bgraPixels.data(), mappedPixels, outputPixels * sizeof(uint32_t));
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteBuffers(1, &outputBuffer);
        glDeleteBuffers(1, &detectionBuffer);
        glDeleteTextures(1, &sourceTexture);

        LOG(useHlgPath ? "Compute shader output path selected: HLG"
                       : "Compute shader output path selected: linear-sRGB");
        return bgraPixels;
    }

private:
    void Initialize() {
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY) {
            throw std::runtime_error("eglGetDisplay failed");
        }

        if (!eglInitialize(m_display, nullptr, nullptr)) {
            throw std::runtime_error("eglInitialize failed: " + DescribeEglError(eglGetError()));
        }

        const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
            EGL_NONE};
        EGLint configCount = 0;
        if (!eglChooseConfig(m_display, configAttribs, &m_config, 1, &configCount) || configCount == 0) {
            throw std::runtime_error("eglChooseConfig failed: " + DescribeEglError(eglGetError()));
        }

        const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1,
                                         EGL_NONE};
        m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
        if (m_context == EGL_NO_CONTEXT) {
            throw std::runtime_error("eglCreateContext failed: " + DescribeEglError(eglGetError()));
        }

        const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m_surface = eglCreatePbufferSurface(m_display, m_config, surfaceAttribs);
        if (m_surface == EGL_NO_SURFACE) {
            throw std::runtime_error("eglCreatePbufferSurface failed: " + DescribeEglError(eglGetError()));
        }

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            throw std::runtime_error("eglMakeCurrent failed: " + DescribeEglError(eglGetError()));
        }

        m_detectProgram = CompileComputeProgram(kDetectionShaderSource);
        m_processProgram = CompileComputeProgram(kProcessingShaderSource);
    }

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;
    GLuint m_detectProgram = 0;
    GLuint m_processProgram = 0;
};

class OutputModuleImpl final : public OutputModule {
public:
    void CopySelectionToClipboard(const CapturedFrame &frame, const SelectionRect &selection,
                                  const DisplayHdrInfo &hdrInfo) override {
        const SelectionRect clampedSelection = ClampSelectionToFrame(selection, frame.metadata);
        if (!clampedSelection.IsValid()) {
            throw std::runtime_error("Selection is empty after clamping");
        }

        const int outputWidth = clampedSelection.Width();
        const int outputHeight = clampedSelection.Height();
        const float sdrWhiteNits = hdrInfo.sdrWhiteLevel > 0.0f ? hdrInfo.sdrWhiteLevel : kSdrReferenceWhiteNits;
        const float lw = ComputeLw(sdrWhiteNits);

        LOG("Copying selection to clipboard via compute shader. Rect=(" + std::to_string(clampedSelection.Left()) +
            "," + std::to_string(clampedSelection.Top()) + ")-(" + std::to_string(clampedSelection.Right()) + "," +
            std::to_string(clampedSelection.Bottom()) + "), SDR white=" + std::to_string(sdrWhiteNits) +
            ", Lw=" + std::to_string(lw));

        std::vector<uint8_t> bgraPixels = m_gpuProcessor.ConvertSelection(frame, clampedSelection, sdrWhiteNits);
        WriteBitmapToClipboard(bgraPixels, outputWidth, outputHeight);
        LOG("Selection copied to clipboard as 8-bit bitmap from SSBO output.");
    }

private:
    GpuOutputProcessor m_gpuProcessor;
};

} // namespace

std::unique_ptr<OutputModule> OutputModule::Create() { return std::make_unique<OutputModuleImpl>(); }
