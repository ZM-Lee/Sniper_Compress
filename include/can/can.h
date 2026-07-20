#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rmcc_sniper::can {

constexpr uint32_t kDefaultCommandId = 0x170;
constexpr int kRoiMovePixels = 10;
constexpr size_t kCommandPayloadSize = 8;

enum class Direction : uint8_t {
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
};

const char* direction_name(Direction direction);

std::array<uint8_t, kCommandPayloadSize> make_command_payload(
    Direction direction);

bool parse_command_payload(const uint8_t* data, size_t length, Direction* direction);

struct RoiPosition {
    int x = 0;
    int y = 0;
};

class RoiController {
public:
    RoiController(int frame_width, int frame_height,
                  int roi_width, int roi_height,
                  int step_pixels = kRoiMovePixels);

    bool move(Direction direction);
    RoiPosition position() const;
    int roi_width() const { return roi_width_; }
    int roi_height() const { return roi_height_; }

private:
    int frame_width_ = 0;
    int frame_height_ = 0;
    int roi_width_ = 0;
    int roi_height_ = 0;
    int step_pixels_ = kRoiMovePixels;
    mutable std::mutex mutex_;
    RoiPosition position_;
};

class CanReceiver {
public:
    using DirectionHandler = std::function<void(Direction)>;

    CanReceiver(std::string interface_name, uint32_t command_id,
                DirectionHandler handler);
    ~CanReceiver();

    CanReceiver(const CanReceiver&) = delete;
    CanReceiver& operator=(const CanReceiver&) = delete;

    void start();
    void stop();

private:
    void run();

    std::string interface_name_;
    uint32_t command_id_ = kDefaultCommandId;
    DirectionHandler handler_;
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
};

}  // namespace rmcc_sniper::can
