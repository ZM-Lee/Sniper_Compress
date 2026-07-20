#pragma once

#include <cstddef>
#include <cstdint>

namespace rmcc_sniper::can {

class Crc {
public:
    static uint8_t crc8(const uint8_t* data, size_t length, uint8_t initial = 0xff) {
        uint8_t crc = initial;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x01U) != 0U
                    ? static_cast<uint8_t>((crc >> 1U) ^ 0x8cU)
                    : static_cast<uint8_t>(crc >> 1U);
            }
        }
        return crc;
    }

    static uint16_t crc16(const uint8_t* data, size_t length,
                          uint16_t initial = 0xffff) {
        uint16_t crc = initial;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x0001U) != 0U
                    ? static_cast<uint16_t>((crc >> 1U) ^ 0x8408U)
                    : static_cast<uint16_t>(crc >> 1U);
            }
        }
        return crc;
    }

    static bool verify_crc8(const uint8_t* data, size_t total_length) {
        return data != nullptr && total_length >= 2 &&
               crc8(data, total_length - 1) == data[total_length - 1];
    }

    static bool verify_crc16(const uint8_t* data, size_t total_length) {
        if (data == nullptr || total_length < 3) return false;
        const uint16_t expected = crc16(data, total_length - 2);
        return data[total_length - 2] == static_cast<uint8_t>(expected & 0xffU) &&
               data[total_length - 1] == static_cast<uint8_t>(expected >> 8U);
    }
};

}  // namespace rmcc_sniper::can
