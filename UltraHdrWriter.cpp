#include "UltraHdrWriter.h"

#include "Logger.h"

#include <ultrahdr_api.h>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <windows.h>

#pragma comment(lib, "comdlg32.lib")

namespace {

// scRGB 里 1.0 对应的绝对亮度（Windows 进阶色彩约定）
constexpr float kScRgbWhiteNits = 80.0f;
// libultrahdr 内部把“线性 HDR 意图 1.0”以及增益图元数据里的 capacity 都按 203 nits 表达，
// 仅用于和库的 API 换算；真正的归一化基准是抓屏时系统的 SDR 白点（见 BuildHdrIntentPixels）。
constexpr float kUhdrRefWhiteNits = 203.0f;
constexpr float kHdrIntentMaxNits = 10000.0f;
constexpr char kIniFileName[] = "printscr.ini";
constexpr char kIniSection[] = "output";

std::string WideToUtf8(const std::wstring &text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string Trim(const std::string &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string ToLower(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 极简 ini 解析：支持 [section]、key=value、; 与 # 注释。自行解析是为了正确处理 UTF-8 中文注释
// （GetPrivateProfileString 只认 ANSI / UTF-16LE）。
using IniValues = std::map<std::string, std::string>;

IniValues ParseIni(const std::string &text) {
    IniValues values;
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3; // 跳过 UTF-8 BOM
    }

    std::string section;
    while (pos < text.size()) {
        size_t lineEnd = text.find_first_of("\r\n", pos);
        if (lineEnd == std::string::npos) {
            lineEnd = text.size();
        }
        const std::string line = Trim(text.substr(pos, lineEnd - pos));
        pos = lineEnd + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = ToLower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = ToLower(Trim(line.substr(0, equals)));
        const std::string value = Trim(line.substr(equals + 1));
        if (!section.empty()) {
            values[section + "." + key] = value;
        }
        values.emplace(key, value); // 无段落限定的兜底键
    }
    return values;
}

std::wstring ResolveIniPath() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring path(modulePath, length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring candidate = path.substr(0, slash + 1) + L"printscr.ini";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return candidate;
            }
        }
    }
    // 退回到当前工作目录
    return std::wstring(L"printscr.ini");
}

std::string ReadFileText(const std::wstring &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string FindValue(const IniValues &values, const char *key) {
    const std::string scoped = std::string(kIniSection) + "." + key;
    auto it = values.find(scoped);
    if (it != values.end()) {
        return it->second;
    }
    it = values.find(key);
    if (it != values.end()) {
        return it->second;
    }
    return {};
}

int ReadInt(const IniValues &values, const char *key, int fallback, int minimum, int maximum) {
    const std::string text = FindValue(values, key);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::clamp(std::stoi(text), minimum, maximum);
    } catch (const std::exception &) {
        LOG(std::string("Invalid integer for ini key '") + key + "': " + text + ", using default");
        return fallback;
    }
}

float ReadFloat(const IniValues &values, const char *key, float fallback) {
    const std::string text = FindValue(values, key);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stof(text);
    } catch (const std::exception &) {
        LOG(std::string("Invalid float for ini key '") + key + "': " + text + ", using default");
        return fallback;
    }
}

void CheckUhdr(const uhdr_error_info_t &error, const char *what) {
    if (error.error_code == UHDR_CODEC_OK) {
        return;
    }
    std::string message =
        std::string(what) + " failed (code " + std::to_string(static_cast<int>(error.error_code)) + ")";
    if (error.has_detail) {
        message += ": " + std::string(error.detail, strnlen(error.detail, sizeof(error.detail)));
    }
    throw std::runtime_error(message);
}

struct EncoderDeleter {
    void operator()(uhdr_codec_private_t *encoder) const {
        if (encoder) {
            uhdr_release_encoder(encoder);
        }
    }
};

// 把选区内的 scRGB 半浮点数据转成 libultrahdr 线性 HDR 意图要求的 RGBA 半浮点。
//
// 增益图是“HDR 线性值 / SDR 线性值”的比值，两边必须以同一个白点归一化：
//   - SDR 基础层是普通 8bit 图像，它的 1.0 会被播放端按“本机 SDR 白点”显示；
//   - 所以 HDR 层也要以“1.0 = 本机 SDR 白点”为单位，即 scRGB 值 × (80 / sdrWhiteNits)。
// 直接用库文档里的 203 nits 只在系统 SDR 白点恰好是 203 nits 时才对；白点低于它（例如
// 80 nits）而仍按 203 归一化，会得到一个偏小的 boost，播放端按本机白点还原后整体变暗
// 203/80 倍。sdrWhiteNits <= 0 时退回 203（即库的默认参考白）。
std::vector<uint16_t> BuildHdrIntentPixels(const CapturedFrame &frame, int sourceLeft, int sourceTop, uint32_t width,
                                           uint32_t height, float sdrWhiteNits) {
    if (frame.pixelData.empty()) {
        throw std::runtime_error("Captured frame has no pixel data");
    }
    const int64_t right = static_cast<int64_t>(sourceLeft) + static_cast<int64_t>(width);
    const int64_t bottom = static_cast<int64_t>(sourceTop) + static_cast<int64_t>(height);
    if (sourceLeft < 0 || sourceTop < 0 || right > static_cast<int64_t>(frame.metadata.width) ||
        bottom > static_cast<int64_t>(frame.metadata.height)) {
        throw std::runtime_error("Selection is outside of the captured frame");
    }
    const uint64_t requiredBytes = static_cast<uint64_t>(frame.metadata.rowPitch) * frame.metadata.height;
    if (frame.pixelData.size() < requiredBytes) {
        throw std::runtime_error("Captured frame pixel buffer is smaller than expected");
    }

    const float whiteNits = (sdrWhiteNits > 0.0f) ? sdrWhiteNits : kUhdrRefWhiteNits;
    const float scale = kScRgbWhiteNits / whiteNits;
    // 库对线性意图的像素值有上限 10000 / 203（浮点表示的上限），用它防止溢出
    const float maxValue = kHdrIntentMaxNits / kUhdrRefWhiteNits;
    const uint16_t halfOne = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);

    std::vector<uint16_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const size_t sourceOffset = static_cast<size_t>(sourceTop + y) * frame.metadata.rowPitch;
        const auto *srcRow = reinterpret_cast<const uint16_t *>(frame.pixelData.data() + sourceOffset) +
                             static_cast<size_t>(sourceLeft) * 4;
        uint16_t *dstRow = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (uint32_t x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                uint16_t &dst = dstRow[static_cast<size_t>(x) * 4 + channel];
                const uint16_t sourceHalf = srcRow[static_cast<size_t>(x) * 4 + channel];
                const float value = DirectX::PackedVector::XMConvertHalfToFloat(sourceHalf) * scale;
                // NaN / 负值按 0 处理，超过 10000 nits 的部分按上限裁剪
                dst = DirectX::PackedVector::XMConvertFloatToHalf(std::isfinite(value) && value > 0.0f
                                                                      ? (std::min)(value, maxValue)
                                                                      : 0.0f);
            }
            dstRow[static_cast<size_t>(x) * 4 + 3] = halfOne;
        }
    }
    return pixels;
}

// 剪贴板的 8bit 内容是 BGRA（packUnorm4x8 写入 B,G,R,A），libultrahdr 需要 RGBA
std::vector<uint8_t> BgraToRgba(const std::vector<uint8_t> &bgra) {
    std::vector<uint8_t> rgba(bgra.size());
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        rgba[i + 0] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i + 0];
        rgba[i + 3] = 0xFF;
    }
    return rgba;
}

} // namespace

UltraHdrEncodeOptions LoadUltraHdrEncodeOptions() {
    const std::wstring path = ResolveIniPath();
    const IniValues values = ParseIni(ReadFileText(path));

    UltraHdrEncodeOptions options;
    if (values.empty()) {
        LOG("printscr.ini not found or empty (" + WideToUtf8(path) + "), using default encode options.");
        return options;
    }

    options.baseQuality = ReadInt(values, "quality", options.baseQuality, 0, 100);
    options.gainmapQuality = ReadInt(values, "gainmap_quality", options.gainmapQuality, 0, 100);
    options.gainmapScaleFactor = ReadInt(values, "gainmap_scale_factor", options.gainmapScaleFactor, 1, 128);
    options.multiChannelGainmap = ReadInt(values, "multi_channel_gainmap", options.multiChannelGainmap, 0, 1);
    options.gainmapGamma = ReadFloat(values, "gainmap_gamma", options.gainmapGamma);
    if (!(options.gainmapGamma > 0.0f)) {
        options.gainmapGamma = 1.0f;
    }

    options.targetDisplayPeakNits = ReadFloat(values, "target_display_peak_nits", options.targetDisplayPeakNits);
    if (options.targetDisplayPeakNits > 0.0f) {
        options.targetDisplayPeakNits = std::clamp(options.targetDisplayPeakNits, 20.0f, 10000.0f);
    } else {
        options.targetDisplayPeakNits = 0.0f;
    }

    options.sdrWhiteNits = ReadFloat(values, "sdr_white_nits", options.sdrWhiteNits);
    if (options.sdrWhiteNits > 0.0f) {
        options.sdrWhiteNits = std::clamp(options.sdrWhiteNits, 20.0f, 1000.0f);
    } else {
        options.sdrWhiteNits = 0.0f;
    }

    const std::string preset = ToLower(Trim(FindValue(values, "preset")));
    options.bestQualityPreset = (preset != "realtime");

    LOG("Ultra HDR options from ini: base_quality=" + std::to_string(options.baseQuality) +
        ", gainmap_quality=" + std::to_string(options.gainmapQuality) +
        ", gainmap_scale_factor=" + std::to_string(options.gainmapScaleFactor) +
        ", multi_channel=" + std::to_string(options.multiChannelGainmap) +
        ", gamma=" + std::to_string(options.gainmapGamma) +
        ", target_display_peak_nits=" + std::to_string(options.targetDisplayPeakNits) +
        ", sdr_white_nits=" + std::to_string(options.sdrWhiteNits) +
        ", preset=" + std::string(options.bestQualityPreset ? "best_quality" : "realtime"));
    return options;
}

std::wstring ShowUltraHdrSaveDialog() {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t fileName[MAX_PATH] = {};
    swprintf_s(fileName, L"printscr_%04u%02u%02u_%02u%02u%02u.jpg", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);

    const wchar_t filter[] = L"Ultra HDR JPEG (*.jpg)\0*.jpg\0All Files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr; // 预览窗口此时已经销毁
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"jpg";
    dialog.lpstrTitle = L"保存 Ultra HDR 截图";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) {
        LOG("Ultra HDR save dialog cancelled.");
        return {};
    }
    LOG("Ultra HDR save dialog returned: " + WideToUtf8(fileName));
    return std::wstring(fileName);
}

bool WriteUltraHdrFile(const std::wstring &path, const std::vector<uint8_t> &sdrBgra, uint32_t width, uint32_t height,
                       const CapturedFrame &capturedFrame, int sourceLeft, int sourceTop, float sdrWhiteNits,
                       float displayPeakNits, const UltraHdrEncodeOptions &options) {
    if (path.empty()) {
        return false;
    }
    if (width == 0 || height == 0) {
        LOG("Refusing to write Ultra HDR file: empty selection.");
        return false;
    }
    const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (sdrBgra.size() < expectedBytes) {
        LOG("Refusing to write Ultra HDR file: SDR buffer too small.");
        return false;
    }

    const float whiteNits = (sdrWhiteNits > 0.0f) ? sdrWhiteNits : kUhdrRefWhiteNits;

    try {
        std::vector<uint16_t> hdrPixels =
            BuildHdrIntentPixels(capturedFrame, sourceLeft, sourceTop, width, height, whiteNits);
        std::vector<uint8_t> sdrRgba = BgraToRgba(sdrBgra);

        uhdr_raw_image_t hdrImage{};
        hdrImage.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
        hdrImage.cg = UHDR_CG_BT_709;
        hdrImage.ct = UHDR_CT_LINEAR;
        hdrImage.range = UHDR_CR_FULL_RANGE;
        hdrImage.w = width;
        hdrImage.h = height;
        hdrImage.planes[UHDR_PLANE_PACKED] = hdrPixels.data();
        hdrImage.stride[UHDR_PLANE_PACKED] = width;

        uhdr_raw_image_t sdrImage{};
        sdrImage.fmt = UHDR_IMG_FMT_32bppRGBA8888;
        sdrImage.cg = UHDR_CG_BT_709;
        sdrImage.ct = UHDR_CT_SRGB;
        sdrImage.range = UHDR_CR_FULL_RANGE;
        sdrImage.w = width;
        sdrImage.h = height;
        sdrImage.planes[UHDR_PLANE_PACKED] = sdrRgba.data();
        sdrImage.stride[UHDR_PLANE_PACKED] = width;

        std::unique_ptr<uhdr_codec_private_t, EncoderDeleter> encoder(uhdr_create_encoder());
        if (!encoder) {
            throw std::runtime_error("uhdr_create_encoder failed");
        }

        CheckUhdr(uhdr_enc_set_raw_image(encoder.get(), &hdrImage, UHDR_HDR_IMG), "uhdr_enc_set_raw_image(HDR)");
        CheckUhdr(uhdr_enc_set_raw_image(encoder.get(), &sdrImage, UHDR_SDR_IMG), "uhdr_enc_set_raw_image(SDR)");
        CheckUhdr(uhdr_enc_set_output_format(encoder.get(), UHDR_CODEC_JPG), "uhdr_enc_set_output_format");
        CheckUhdr(uhdr_enc_set_quality(encoder.get(), options.baseQuality, UHDR_BASE_IMG),
                  "uhdr_enc_set_quality(base)");
        CheckUhdr(uhdr_enc_set_quality(encoder.get(), options.gainmapQuality, UHDR_GAIN_MAP_IMG),
                  "uhdr_enc_set_quality(gainmap)");
        CheckUhdr(uhdr_enc_set_gainmap_scale_factor(encoder.get(), options.gainmapScaleFactor),
                  "uhdr_enc_set_gainmap_scale_factor");
        CheckUhdr(uhdr_enc_set_gainmap_gamma(encoder.get(), options.gainmapGamma), "uhdr_enc_set_gainmap_gamma");
        // 下限兜底：基础层 JPEG 在纯黑区域会有振铃，导致个别像素出现 hdr/sdr 比值 0，
        // 写进元数据就是 min_content_boost = 0，某些解码端算 log2(0) 会得到 NaN。
        CheckUhdr(uhdr_enc_set_min_max_content_boost(encoder.get(), 1.0f / 32.0f, 1000.0f),
                  "uhdr_enc_set_min_max_content_boost");
        CheckUhdr(uhdr_enc_set_using_multi_channel_gainmap(encoder.get(), options.multiChannelGainmap),
                  "uhdr_enc_set_using_multi_channel_gainmap");
        CheckUhdr(uhdr_enc_set_preset(encoder.get(),
                                      options.bestQualityPreset ? UHDR_USAGE_BEST_QUALITY : UHDR_USAGE_REALTIME),
                  "uhdr_enc_set_preset");
        // 库把 capacity（增益图的满量程倍数）按 203 nits 的倍数换算，这里把“以本机 SDR 白点为
        // 1.0 的显示器峰值”折算成库要的 nits 参数，保证 hdr_capacity_max 仍是真实的峰值倍数。
        if (displayPeakNits > 0.0f) {
            const float capacityUnits = displayPeakNits / whiteNits;
            const float apiNits = std::clamp(capacityUnits * kUhdrRefWhiteNits, kUhdrRefWhiteNits, kHdrIntentMaxNits);
            CheckUhdr(uhdr_enc_set_target_display_peak_brightness(encoder.get(), apiNits),
                      "uhdr_enc_set_target_display_peak_brightness");
        }

        CheckUhdr(uhdr_encode(encoder.get()), "uhdr_encode");

        const uhdr_compressed_image_t *stream = uhdr_get_encoded_stream(encoder.get());
        if (!stream || !stream->data || stream->data_sz == 0) {
            throw std::runtime_error("uhdr_get_encoded_stream returned no data");
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            LOG("Failed to open output file: " + WideToUtf8(path));
            return false;
        }
        file.write(static_cast<const char *>(stream->data), static_cast<std::streamsize>(stream->data_sz));
        if (!file) {
            LOG("Failed to write output file: " + WideToUtf8(path));
            return false;
        }

        LOG("Ultra HDR written: " + WideToUtf8(path) + ", " + std::to_string(width) + "x" + std::to_string(height) +
            ", " + std::to_string(stream->data_sz) + " bytes, SDR white=" + std::to_string(whiteNits) +
            " nits (HDR layer scaled by " + std::to_string(kScRgbWhiteNits / whiteNits) + "), display peak=" +
            std::to_string(displayPeakNits) + " nits, HDR source=(" + std::to_string(sourceLeft) + "," +
            std::to_string(sourceTop) + ")");
        return true;
    } catch (const std::exception &ex) {
        LOG(std::string("Ultra HDR encoding failed: ") + ex.what());
        return false;
    }
}
