#include "can/can.h"

#include "can/crc.h"

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
    payload[2] = Crc::crc8(payload.data(), 2);
    const uint16_t crc16 = Crc::crc16(payload.data(), payload.size() - 2);
    payload[6] = static_cast<uint8_t>(crc16 & 0xffU);
    payload[7] = static_cast<uint8_t>(crc16 >> 8U);
    return payload;
}

bool parse_command_payload(const uint8_t* data, size_t length, Direction* direction) {
    if (data == nullptr || direction == nullptr || length != kCommandPayloadSize) {
        return false;
    }
    if (!Crc::verify_crc8(data, 3) || !Crc::verify_crc16(data, length)) {
        return false;
    }
    if (data[1] != 0 || data[3] != 0 || data[4] != 0 || data[5] != 0) {
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

int open_can_socket(const std::string& interface_name, uint32_t command_id) {
    const unsigned int interface_index = if_nametoindex(interface_name.c_str());
    if (interface_index == 0) return -1;

    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) return -1;

    can_filter filter{};
    filter.can_id = command_id;
    filter.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
        close(fd);
        return -1;
    }

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = static_cast<int>(interface_index);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

void CanReceiver::run() {
    int retry_count = 0;
    while (!stop_requested_.load()) {
        const int fd = open_can_socket(interface_name_, command_id_);
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
            if ((frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0 ||
                (frame.can_id & CAN_SFF_MASK) != command_id_) {
                continue;
            }

            Direction direction{};
            if (!parse_command_payload(frame.data, frame.can_dlc, &direction)) {
                std::fprintf(stderr, "CAN cmd_id=0x%03x rejected: invalid payload/CRC\n",
                             command_id_);
                continue;
            }
            if (handler_) handler_(direction);
        }
        close(fd);
    }
}

}  // namespace rmcc_sniper::can
