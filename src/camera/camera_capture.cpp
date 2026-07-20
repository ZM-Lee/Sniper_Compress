#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#if __has_include("daheng_camera/driver/GxIAPI.h")
#include "daheng_camera/driver/DxImageProc.h"
#include "daheng_camera/driver/GxIAPI.h"
#else
#include "DxImageProc.h"
#include "GxIAPI.h"
#endif
#include "compress/shm_ring.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

struct CameraConfig {
    double exposure_us = 5000.0;
    double gain = 20.0;
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    bool balance_white_auto = true;
    bool trigger_mode = false;
    bool gamma_enable = true;
    int gamma_mode = GX_GAMMA_SELECTOR_SRGB;
    double gamma = 0.5;
};

struct CameraOptions {
    const char* shm_name = "/rm_camera_frames";
    const char* camera_config_path = "config/daheng_camera/feature.yaml";
    int slots = 4;
    unsigned int device_index = 0;
    int max_frames = 0;
    int roi_size = 1080;
    bool auto_square_roi = false;
    double fps = 24.0;
    double exposure_us = 20000.0;
    bool allow_adjust_roi = false;
    bool fps_override = false;
    bool exposure_override = false;
};

struct SessionResult {
    bool reconnect = false;
    int frames = 0;
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --shm-name NAME       Shared memory ring name (default: /rm_camera_frames)\n"
        "  --camera-config PATH  Daheng YAML config (default: config/daheng_camera/feature.yaml)\n"
        "  --slots N             Ring slots (default: 4)\n"
        "  --device-index N      Daheng device index, zero-based (default: 0)\n"
        "  --frames N            Stop after N frames, 0 means run forever (default: 0)\n"
        "  --roi-size N          Fixed center ROI size (default: 1080)\n"
        "  --roi-mode MODE       fixed|max-square; max-square uses min(max width, max height)\n"
        "  --auto-square-roi     Alias for --roi-mode max-square\n"
        "  --fps FPS             Acquisition frame rate override\n"
        "  --exposure-us US      Exposure time override in microseconds\n"
        "  --allow-adjust-roi    Align unsupported ROI size down to camera increment\n"
        "  --help                Show this help\n",
        prog);
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

std::string strip_inline_comment(std::string value) {
    bool in_quote = false;
    char quote = '\0';
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if ((c == '\'' || c == '"') && (i == 0 || value[i - 1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = c;
            } else if (quote == c) {
                in_quote = false;
            }
        } else if (c == '#' && !in_quote) {
            value.resize(i);
            break;
        }
    }
    return trim(value);
}

std::map<std::string, std::string> load_simple_yaml(const char* path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    if (!in) {
        if (errno != ENOENT) {
            std::fprintf(stderr, "Warning: cannot open camera config %s\n", path);
        }
        return values;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(strip_inline_comment(line));
        if (line.empty()) continue;
        const size_t pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty() && !value.empty()) values[key] = value;
    }
    return values;
}

template <typename T>
bool parse_value(const std::map<std::string, std::string>& values, const char* key, T* out) {
    auto it = values.find(key);
    if (it == values.end()) return false;
    std::istringstream ss(it->second);
    T parsed{};
    ss >> parsed;
    if (!ss.fail()) {
        *out = parsed;
        return true;
    }
    std::fprintf(stderr, "Warning: invalid camera config value %s: %s\n",
                 key, it->second.c_str());
    return false;
}

CameraConfig load_camera_config(const CameraOptions& opt) {
    CameraConfig cfg;
    const auto values = load_simple_yaml(opt.camera_config_path);
    parse_value(values, "ExposureTime", &cfg.exposure_us);
    parse_value(values, "Gain", &cfg.gain);
    parse_value(values, "Width", &cfg.width);
    parse_value(values, "Height", &cfg.height);
    parse_value(values, "AcquisitionFrameRate", &cfg.fps);
    parse_value(values, "BalanceWhiteAuto", &cfg.balance_white_auto);
    parse_value(values, "TriggerMode", &cfg.trigger_mode);
    parse_value(values, "GammaEnable", &cfg.gamma_enable);
    parse_value(values, "GammaMode", &cfg.gamma_mode);
    parse_value(values, "Gamma", &cfg.gamma);

    if (opt.fps_override) cfg.fps = opt.fps;
    if (opt.exposure_override) cfg.exposure_us = opt.exposure_us;
    if (!values.empty()) {
        std::printf("Loaded Daheng camera config: %s\n", opt.camera_config_path);
    } else {
        std::printf("Using built-in Daheng camera defaults; config not found/read: %s\n",
                    opt.camera_config_path);
    }
    return cfg;
}

bool check(GX_STATUS status, const char* op) {
    if (status == GX_STATUS_SUCCESS) return true;
    std::fprintf(stderr, "%s failed: 0x%x\n", op, static_cast<int>(status));
    return false;
}

void warn_if_fail(GX_STATUS status, const char* op) {
    if (status != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "Warning: %s failed: 0x%x\n", op, static_cast<int>(status));
    }
}

int64_t align_down(int64_t value, int64_t min_value, int64_t inc) {
    if (inc <= 0) inc = 1;
    if (value < min_value) return min_value;
    return min_value + ((value - min_value) / inc) * inc;
}

int64_t clamp_and_align(int64_t value, const GX_INT_RANGE& range) {
    value = std::max<int64_t>(range.nMin, std::min<int64_t>(value, range.nMax));
    return align_down(value, range.nMin, range.nInc);
}

int64_t align_square_down(int64_t value,
                          const GX_INT_RANGE& width_rng,
                          const GX_INT_RANGE& height_rng) {
    int64_t side = std::min<int64_t>(value, std::min(width_rng.nMax, height_rng.nMax));
    side = std::min(align_down(side, width_rng.nMin, width_rng.nInc),
                    align_down(side, height_rng.nMin, height_rng.nInc));
    while (side >= width_rng.nMin && side >= height_rng.nMin) {
        const int64_t w = align_down(side, width_rng.nMin, width_rng.nInc);
        const int64_t h = align_down(side, height_rng.nMin, height_rng.nInc);
        if (w == side && h == side) return side;
        side = std::min(w, h) - 1;
    }
    return 0;
}

bool get_int_range(GX_DEV_HANDLE handle, GX_FEATURE_ID_CMD key, const char* name,
                   GX_INT_RANGE* out) {
    std::memset(out, 0, sizeof(*out));
    return check(GXGetIntRange(handle, key, out), name);
}

bool get_int(GX_DEV_HANDLE handle, GX_FEATURE_ID_CMD key, const char* name, int64_t* out) {
    return check(GXGetInt(handle, key, out), name);
}

bool set_int(GX_DEV_HANDLE handle, GX_FEATURE_ID_CMD key, const char* name, int64_t value) {
    return check(GXSetInt(handle, key, value), name);
}

bool configure_center_roi(GX_DEV_HANDLE handle, int requested_size, bool auto_square,
                          bool allow_adjust, const CameraConfig& cfg,
                          uint32_t* out_w, uint32_t* out_h) {
    if (!set_int(handle, GX_INT_OFFSET_X, "OffsetX", 0) ||
        !set_int(handle, GX_INT_OFFSET_Y, "OffsetY", 0)) {
        return false;
    }

    GX_INT_RANGE width_rng{}, height_rng{}, ox_rng{}, oy_rng{};
    if (!get_int_range(handle, GX_INT_WIDTH, "Width range", &width_rng) ||
        !get_int_range(handle, GX_INT_HEIGHT, "Height range", &height_rng) ||
        !get_int_range(handle, GX_INT_OFFSET_X, "OffsetX range", &ox_rng) ||
        !get_int_range(handle, GX_INT_OFFSET_Y, "OffsetY range", &oy_rng)) {
        return false;
    }

    int64_t full_w = 0;
    int64_t full_h = 0;
    if (GXGetInt(handle, GX_INT_WIDTH_MAX, &full_w) != GX_STATUS_SUCCESS || full_w <= 0) {
        full_w = width_rng.nMax;
    }
    if (GXGetInt(handle, GX_INT_HEIGHT_MAX, &full_h) != GX_STATUS_SUCCESS || full_h <= 0) {
        full_h = height_rng.nMax;
    }

    int64_t roi_w = cfg.width;
    int64_t roi_h = cfg.height;
    if (requested_size > 0) {
        roi_w = requested_size;
        roi_h = requested_size;
    }
    if (auto_square) {
        const int64_t requested_auto = std::min(full_w, full_h);
        const int64_t side = align_square_down(requested_auto, width_rng, height_rng);
        if (side <= 0) {
            std::fprintf(stderr,
                "Cannot derive max-square ROI from Daheng ranges "
                "(Width min=%ld max=%ld inc=%ld, Height min=%ld max=%ld inc=%ld)\n",
                static_cast<long>(width_rng.nMin), static_cast<long>(width_rng.nMax),
                static_cast<long>(width_rng.nInc),
                static_cast<long>(height_rng.nMin), static_cast<long>(height_rng.nMax),
                static_cast<long>(height_rng.nInc));
            return false;
        }
        roi_w = side;
        roi_h = side;
        std::printf("Auto max-square ROI: using side %ld\n", static_cast<long>(side));
    }

    const bool exact_w = roi_w >= width_rng.nMin && roi_w <= width_rng.nMax &&
                         align_down(roi_w, width_rng.nMin, width_rng.nInc) == roi_w;
    const bool exact_h = roi_h >= height_rng.nMin && roi_h <= height_rng.nMax &&
                         align_down(roi_h, height_rng.nMin, height_rng.nInc) == roi_h;
    if (!exact_w || !exact_h) {
        if (!allow_adjust) {
            std::fprintf(stderr,
                "Requested ROI %ldx%ld is not supported by Daheng increments "
                "(Width min=%ld max=%ld inc=%ld, Height min=%ld max=%ld inc=%ld). "
                "Use --allow-adjust-roi to align down.\n",
                static_cast<long>(roi_w), static_cast<long>(roi_h),
                static_cast<long>(width_rng.nMin), static_cast<long>(width_rng.nMax),
                static_cast<long>(width_rng.nInc),
                static_cast<long>(height_rng.nMin), static_cast<long>(height_rng.nMax),
                static_cast<long>(height_rng.nInc));
            return false;
        }
        roi_w = align_down(std::min(roi_w, width_rng.nMax), width_rng.nMin, width_rng.nInc);
        roi_h = align_down(std::min(roi_h, height_rng.nMax), height_rng.nMin, height_rng.nInc);
        std::fprintf(stderr, "Adjusted ROI to %ldx%ld for Daheng increments\n",
                     static_cast<long>(roi_w), static_cast<long>(roi_h));
    }

    if (!set_int(handle, GX_INT_WIDTH, "Width", roi_w) ||
        !set_int(handle, GX_INT_HEIGHT, "Height", roi_h)) {
        return false;
    }

    if (!get_int_range(handle, GX_INT_OFFSET_X, "OffsetX range", &ox_rng) ||
        !get_int_range(handle, GX_INT_OFFSET_Y, "OffsetY range", &oy_rng)) {
        return false;
    }
    const int64_t ox = clamp_and_align((full_w - roi_w) / 2, ox_rng);
    const int64_t oy = clamp_and_align((full_h - roi_h) / 2, oy_rng);
    if (!set_int(handle, GX_INT_OFFSET_X, "OffsetX", ox) ||
        !set_int(handle, GX_INT_OFFSET_Y, "OffsetY", oy)) {
        return false;
    }

    int64_t actual_w = 0, actual_h = 0, actual_ox = 0, actual_oy = 0;
    if (!get_int(handle, GX_INT_WIDTH, "Width", &actual_w) ||
        !get_int(handle, GX_INT_HEIGHT, "Height", &actual_h) ||
        !get_int(handle, GX_INT_OFFSET_X, "OffsetX", &actual_ox) ||
        !get_int(handle, GX_INT_OFFSET_Y, "OffsetY", &actual_oy)) {
        return false;
    }
    std::printf("ROI: %ldx%ld at offset (%ld,%ld), full max %ldx%ld\n",
                static_cast<long>(actual_w), static_cast<long>(actual_h),
                static_cast<long>(actual_ox), static_cast<long>(actual_oy),
                static_cast<long>(full_w), static_cast<long>(full_h));
    *out_w = static_cast<uint32_t>(actual_w);
    *out_h = static_cast<uint32_t>(actual_h);
    return true;
}

DX_PIXEL_COLOR_FILTER to_dx_bayer(int64_t pixel_format) {
    switch (static_cast<GX_PIXEL_FORMAT_ENTRY>(pixel_format)) {
        case GX_PIXEL_FORMAT_BAYER_RG8:
            return BAYERRG;
        case GX_PIXEL_FORMAT_BAYER_GR8:
            return BAYERGR;
        case GX_PIXEL_FORMAT_BAYER_GB8:
            return BAYERGB;
        case GX_PIXEL_FORMAT_BAYER_BG8:
            return BAYERBG;
        default:
            return NONE;
    }
}

bool select_bayer_format(GX_DEV_HANDLE handle, int64_t* out_pixel_format) {
    const GX_PIXEL_FORMAT_ENTRY formats[] = {
        GX_PIXEL_FORMAT_BAYER_RG8,
        GX_PIXEL_FORMAT_BAYER_GR8,
        GX_PIXEL_FORMAT_BAYER_GB8,
        GX_PIXEL_FORMAT_BAYER_BG8,
    };
    for (GX_PIXEL_FORMAT_ENTRY fmt : formats) {
        if (GXSetEnum(handle, GX_ENUM_PIXEL_FORMAT, fmt) == GX_STATUS_SUCCESS) {
            *out_pixel_format = fmt;
            std::printf("Daheng pixel format: 0x%lx\n", static_cast<long>(fmt));
            return true;
        }
    }
    std::fprintf(stderr, "Failed to select supported Daheng Bayer8 pixel format\n");
    return false;
}

void apply_camera_features(GX_DEV_HANDLE handle, const CameraConfig& cfg) {
    warn_if_fail(GXSetEnum(handle, GX_ENUM_EXPOSURE_AUTO, GX_EXPOSURE_AUTO_OFF),
                 "ExposureAuto Off");
    if (cfg.exposure_us > 0.0) {
        warn_if_fail(GXSetFloat(handle, GX_FLOAT_EXPOSURE_TIME, cfg.exposure_us),
                     "ExposureTime");
    }

    warn_if_fail(GXSetEnum(handle, GX_ENUM_GAIN_AUTO, GX_GAIN_AUTO_OFF), "GainAuto Off");
    if (cfg.gain >= 0.0) {
        warn_if_fail(GXSetFloat(handle, GX_FLOAT_GAIN, cfg.gain), "Gain");
    }

    warn_if_fail(GXSetEnum(handle, GX_ENUM_BALANCE_WHITE_AUTO,
                           cfg.balance_white_auto ? GX_BALANCE_WHITE_AUTO_CONTINUOUS
                                                  : GX_BALANCE_WHITE_AUTO_OFF),
                 "BalanceWhiteAuto");
    warn_if_fail(GXSetEnum(handle, GX_ENUM_TRIGGER_MODE,
                           cfg.trigger_mode ? GX_TRIGGER_MODE_ON : GX_TRIGGER_MODE_OFF),
                 "TriggerMode");
    warn_if_fail(GXSetBool(handle, GX_BOOL_GAMMA_ENABLE, cfg.gamma_enable), "GammaEnable");
    if (cfg.gamma_enable) {
        warn_if_fail(GXSetEnum(handle, GX_ENUM_GAMMA_MODE, cfg.gamma_mode), "GammaMode");
        if (cfg.gamma_mode == GX_GAMMA_SELECTOR_USER && cfg.gamma > 0.0) {
            warn_if_fail(GXSetFloat(handle, GX_FLOAT_GAMMA, cfg.gamma), "Gamma");
        }
    }

    warn_if_fail(GXSetEnum(handle, GX_ENUM_ACQUISITION_MODE, GX_ACQ_MODE_CONTINUOUS),
                 "AcquisitionMode Continuous");
    if (cfg.fps > 0.0) {
        warn_if_fail(GXSetEnum(handle, GX_ENUM_ACQUISITION_FRAME_RATE_MODE,
                               GX_ACQUISITION_FRAME_RATE_MODE_ON),
                     "AcquisitionFrameRateMode On");
        warn_if_fail(GXSetFloat(handle, GX_FLOAT_ACQUISITION_FRAME_RATE, cfg.fps),
                     "AcquisitionFrameRate");
    }
}

SessionResult run_camera_session(const CameraOptions& opt, int frames_remaining) {
    SessionResult result{};
    GX_DEV_HANDLE handle = nullptr;
    bool stream_started = false;
    compress::ShmRing ring;

    auto cleanup = [&]() {
        if (handle) {
            if (stream_started) {
                warn_if_fail(GXStreamOff(handle), "GXStreamOff");
                stream_started = false;
            }
            warn_if_fail(GXCloseDevice(handle), "GXCloseDevice");
            handle = nullptr;
        }
        ring.close();
    };

    const CameraConfig cfg = load_camera_config(opt);

    uint32_t device_count = 0;
    if (!check(GXUpdateDeviceList(&device_count, 1000), "GXUpdateDeviceList")) {
        result.reconnect = true;
        return result;
    }
    if (device_count == 0U || opt.device_index >= device_count) {
        std::fprintf(stderr, "No Daheng camera at index %u (found %u devices)\n",
                     opt.device_index, device_count);
        result.reconnect = true;
        return result;
    }

    std::printf("Daheng devices found: %u; opening index %u\n", device_count, opt.device_index);
    if (!check(GXOpenDeviceByIndex(opt.device_index + 1U, &handle), "GXOpenDeviceByIndex")) {
        result.reconnect = true;
        cleanup();
        return result;
    }

    uint32_t width = 0, height = 0;
    if (!configure_center_roi(handle, opt.roi_size, opt.auto_square_roi,
                              opt.allow_adjust_roi, cfg, &width, &height)) {
        result.reconnect = true;
        cleanup();
        return result;
    }

    int64_t pixel_format = 0;
    if (!select_bayer_format(handle, &pixel_format)) {
        result.reconnect = true;
        cleanup();
        return result;
    }
    const DX_PIXEL_COLOR_FILTER bayer_type = to_dx_bayer(pixel_format);
    if (bayer_type == NONE) {
        std::fprintf(stderr, "Unsupported Daheng Bayer format: 0x%lx\n",
                     static_cast<long>(pixel_format));
        result.reconnect = true;
        cleanup();
        return result;
    }
    apply_camera_features(handle, cfg);

    int64_t payload_size = 0;
    if (!get_int(handle, GX_INT_PAYLOAD_SIZE, "PayloadSize", &payload_size) ||
        payload_size <= 0) {
        result.reconnect = true;
        cleanup();
        return result;
    }

    const uint32_t stride = width * 3;
    std::string error;
    if (!ring.create(opt.shm_name, static_cast<uint32_t>(opt.slots), width, height,
                     stride, compress::SHM_PIXFMT_BGR8, &error)) {
        std::fprintf(stderr, "Failed to create shm ring %s: %s\n",
                     opt.shm_name, error.c_str());
        result.reconnect = true;
        cleanup();
        return result;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(payload_size));
    std::vector<uint8_t> bgr(static_cast<size_t>(stride) * height);

    if (!check(GXStreamOn(handle), "GXStreamOn")) {
        result.reconnect = true;
        cleanup();
        return result;
    }
    stream_started = true;

    std::printf("Daheng camera capture started: %ux%u BGR8 %.2f fps exposure %.1fus gain %.1f -> shm %s\n",
                width, height, cfg.fps, cfg.exposure_us, cfg.gain, opt.shm_name);

    uint64_t first_ns = compress::monotonic_time_ns();
    uint64_t last_report_ns = first_ns;
    int consecutive_errors = 0;
    while (!g_stop && (frames_remaining <= 0 || result.frames < frames_remaining)) {
        GX_FRAME_DATA frame{};
        frame.pImgBuf = raw.data();
        const GX_STATUS status = GXGetImage(handle, &frame, 1000);
        if (status != GX_STATUS_SUCCESS || frame.nStatus != GX_FRAME_STATUS_SUCCESS) {
            std::fprintf(stderr, "GXGetImage timeout/error: 0x%x frame_status=0x%x\n",
                         static_cast<int>(status), static_cast<int>(frame.nStatus));
            consecutive_errors++;
            if (consecutive_errors >= 3) {
                std::fprintf(stderr, "Camera grab failed %d times; reconnecting camera\n",
                             consecutive_errors);
                result.reconnect = true;
                break;
            }
            continue;
        }
        consecutive_errors = 0;

        if (frame.nWidth != static_cast<int32_t>(width) ||
            frame.nHeight != static_cast<int32_t>(height)) {
            std::fprintf(stderr, "Unexpected frame size %dx%d, expected %ux%u\n",
                         frame.nWidth, frame.nHeight, width, height);
            result.reconnect = true;
            break;
        }

        const VxInt32 cvt_ret = DxRaw8toRGB24Ex(
            frame.pImgBuf, bgr.data(), width, height, RAW2RGB_NEIGHBOUR,
            bayer_type, false, DX_ORDER_BGR);
        if (cvt_ret != DX_OK) {
            std::fprintf(stderr, "DxRaw8toRGB24Ex failed: %d\n", cvt_ret);
            consecutive_errors++;
            if (consecutive_errors >= 3) {
                std::fprintf(stderr, "Pixel conversion failed %d times; reconnecting camera\n",
                             consecutive_errors);
                result.reconnect = true;
                break;
            }
            continue;
        }

        ring.write_latest(bgr.data(), static_cast<uint32_t>(bgr.size()),
                          compress::monotonic_time_ns());
        result.frames++;

        uint64_t now = compress::monotonic_time_ns();
        if (now - last_report_ns >= 1000000000ULL) {
            double elapsed = static_cast<double>(now - first_ns) / 1.0e9;
            std::printf("  captured=%d actual_fps=%.2f camera_frame=%lu\n",
                        result.frames, result.frames / std::max(0.001, elapsed),
                        static_cast<unsigned long>(frame.nFrameID));
            last_report_ns = now;
        }
    }

    cleanup();
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    CameraOptions opt;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shm-name") == 0 && i + 1 < argc) {
            opt.shm_name = argv[++i];
        } else if (std::strcmp(argv[i], "--camera-config") == 0 && i + 1 < argc) {
            opt.camera_config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--slots") == 0 && i + 1 < argc) {
            opt.slots = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--device-index") == 0 && i + 1 < argc) {
            opt.device_index = static_cast<unsigned int>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            opt.max_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--roi-size") == 0 && i + 1 < argc) {
            opt.roi_size = std::atoi(argv[++i]);
            opt.auto_square_roi = false;
        } else if (std::strcmp(argv[i], "--roi-mode") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            if (std::strcmp(mode, "fixed") == 0) {
                opt.auto_square_roi = false;
            } else if (std::strcmp(mode, "max-square") == 0) {
                opt.auto_square_roi = true;
                opt.allow_adjust_roi = true;
            } else {
                std::fprintf(stderr, "Unknown roi mode: %s\n", mode);
                usage(argv[0]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--auto-square-roi") == 0) {
            opt.auto_square_roi = true;
            opt.allow_adjust_roi = true;
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            opt.fps = std::atof(argv[++i]);
            opt.fps_override = true;
        } else if (std::strcmp(argv[i], "--exposure-us") == 0 && i + 1 < argc) {
            opt.exposure_us = std::atof(argv[++i]);
            opt.exposure_override = true;
        } else if (std::strcmp(argv[i], "--allow-adjust-roi") == 0) {
            opt.allow_adjust_roi = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!check(GXInitLib(), "GXInitLib")) return 1;

    int reconnect_attempt = 0;
    int total_frames = 0;

    while (!g_stop && (opt.max_frames <= 0 || total_frames < opt.max_frames)) {
        const int remaining = opt.max_frames > 0 ? (opt.max_frames - total_frames) : 0;
        SessionResult sr = run_camera_session(opt, remaining);
        total_frames += sr.frames;
        if (sr.frames > 0) reconnect_attempt = 0;
        if (sr.reconnect && !g_stop && (opt.max_frames <= 0 || total_frames < opt.max_frames)) {
            reconnect_attempt++;
            int delay_s = std::min(5, reconnect_attempt);
            std::fprintf(stderr, "Reconnecting camera in %ds (attempt %d)\n",
                         delay_s, reconnect_attempt);
            for (int i = 0; i < delay_s && !g_stop; ++i) {
                sleep(1);
            }
            continue;
        }
        break;
    }

    GXCloseLib();
    return 0;
}
