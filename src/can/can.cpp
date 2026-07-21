#include "can/can.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rmcc_sniper::can {

const char* direction_name(Direction direction) {
    switch (direction) {
        case Direction::Up: return "up";
        case Direction::Down: return "down";
        case Direction::Left: return "left";
        case Direction::Right: return "right";
    }
    return "unknown";
}

std::array<uint8_t, kCommandPayloadSize> make_command_payload(
    Direction direction) {
    std::array<uint8_t, kCommandPayloadSize> payload{};
    payload[0] = static_cast<uint8_t>(direction);
    return payload;
}

bool parse_command_payload(const uint8_t* data, size_t length, Direction* direction) {
    if (data == nullptr || direction == nullptr || length != kCommandPayloadSize) {
        return false;
    }
    if (!std::all_of(data + 1, data + kCommandPayloadSize,
                     [](uint8_t value) { return value == 0; })) {
        return false;
    }
    switch (static_cast<Direction>(data[0])) {
        case Direction::Up:
        case Direction::Down:
        case Direction::Left:
        case Direction::Right:
            *direction = static_cast<Direction>(data[0]);
            return true;
    }
    return false;
}

RoiController::RoiController(int frame_width, int frame_height,
                             int roi_width, int roi_height, int step_pixels)
    : frame_width_(std::max(1, frame_width)),
      frame_height_(std::max(1, frame_height)),
      roi_width_(std::clamp(roi_width, 1, frame_width_)),
      roi_height_(std::clamp(roi_height, 1, frame_height_)),
      step_pixels_(std::max(1, step_pixels)) {
    position_.x = (frame_width_ - roi_width_) / 2;
    position_.y = (frame_height_ - roi_height_) / 2;
}

bool RoiController::move(Direction direction) {
    std::lock_guard<std::mutex> lock(mutex_);
    const RoiPosition previous = position_;
    switch (direction) {
        case Direction::Up: position_.y -= step_pixels_; break;
        case Direction::Down: position_.y += step_pixels_; break;
        case Direction::Left: position_.x -= step_pixels_; break;
        case Direction::Right: position_.x += step_pixels_; break;
    }
    position_.x = std::clamp(position_.x, 0, frame_width_ - roi_width_);
    position_.y = std::clamp(position_.y, 0, frame_height_ - roi_height_);
    return position_.x != previous.x || position_.y != previous.y;
}

RoiPosition RoiController::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return position_;
}

CanReceiver::CanReceiver(std::string interface_name, uint32_t command_id,
                         DirectionHandler handler)
    : interface_name_(std::move(interface_name)),
      command_id_(command_id),
      handler_(std::move(handler)) {}

CanReceiver::~CanReceiver() {
    stop();
}

void CanReceiver::start() {
    if (thread_.joinable()) return;
    stop_requested_.store(false);
    thread_ = std::thread(&CanReceiver::run, this);
}

void CanReceiver::stop() {
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
}

namespace {

int open_can_socket(const std::string& interface_name) {
    const unsigned int interface_index = if_nametoindex(interface_name.c_str());
    if (interface_index == 0) return -1;

    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return -1;

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = static_cast<int>(interface_index);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void log_rejected_frame(const char* reason, const can_frame& frame,
                        uint32_t expected_id) {
    char data_hex[CAN_MAX_DLEN * 2 + 1]{};
    const uint8_t data_length = std::min<uint8_t>(frame.can_dlc, CAN_MAX_DLEN);
    for (uint8_t i = 0; i < data_length; ++i) {
        std::snprintf(data_hex + i * 2, sizeof(data_hex) - i * 2,
                      "%02x", frame.data[i]);
    }
    const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
    const uint32_t received_id = frame.can_id &
        (extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    // std::fprintf(stderr,
    //              "CAN rejected: reason=%s id=0x%x expected=0x%03x "
    //              "type=%s dlc=%u data=%s\n",
    //              reason, received_id, expected_id,
    //              extended ? "EFF" : "SFF", frame.can_dlc,
    //              data_length > 0 ? data_hex : "(empty)");
}

}  // namespace

void CanReceiver::run() {
    int retry_count = 0;
    while (!stop_requested_.load()) {
        const int fd = open_can_socket(interface_name_);
        if (fd < 0) {
            if (retry_count == 0 || retry_count % 5 == 0) {
                std::fprintf(stderr, "CAN %s open failed: %s; retrying\n",
                             interface_name_.c_str(), std::strerror(errno));
            }
            ++retry_count;
            for (int i = 0; i < 4 && !stop_requested_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            continue;
        }

        retry_count = 0;
        std::fprintf(stderr, "CAN receiver ready: interface=%s cmd_id=0x%03x\n",
                     interface_name_.c_str(), command_id_);
        while (!stop_requested_.load()) {
            pollfd event{fd, POLLIN, 0};
            const int poll_result = poll(&event, 1, 250);
            if (poll_result < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (poll_result == 0) continue;
            if ((event.revents & POLLIN) == 0) break;

            can_frame frame{};
            const ssize_t bytes = recv(fd, &frame, sizeof(frame), 0);
            if (bytes != static_cast<ssize_t>(sizeof(frame))) break;
            if ((frame.can_id & CAN_ERR_FLAG) != 0) {
                log_rejected_frame("error-frame", frame, command_id_);
                continue;
            }
            if ((frame.can_id & CAN_RTR_FLAG) != 0) {
                log_rejected_frame("remote-frame", frame, command_id_);
                continue;
            }
            if ((frame.can_id & CAN_EFF_FLAG) != 0) {
                log_rejected_frame("extended-frame", frame, command_id_);
                continue;
            }
            if ((frame.can_id & CAN_SFF_MASK) != command_id_) {
                log_rejected_frame("unexpected-cmd-id", frame, command_id_);
                continue;
            }

            Direction direction{};
            if (!parse_command_payload(frame.data, frame.can_dlc, &direction)) {
                const char* reason = "invalid-dlc";
                if (frame.can_dlc == kCommandPayloadSize) {
                    if (frame.data[0] < static_cast<uint8_t>(Direction::Up) ||
                        frame.data[0] > static_cast<uint8_t>(Direction::Right)) {
                        reason = "invalid-direction";
                    } else {
                        reason = "invalid-padding";
                    }
                }
                log_rejected_frame(reason, frame, command_id_);
                continue;
            }
            if (handler_) handler_(direction);
        }
        close(fd);
    }
}

}  // namespace rmcc_sniper::can
