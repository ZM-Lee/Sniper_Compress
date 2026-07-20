#include "can/can.h"
#include "can/crc.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace rmcc_sniper::can;

    const std::array<std::array<uint8_t, kCommandPayloadSize>, 4> known_payloads{{
        {{0x01, 0x00, 0x45, 0x00, 0x00, 0x00, 0xbb, 0x74}},
        {{0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 0x87, 0xc3}},
        {{0x03, 0x00, 0xd4, 0x00, 0x00, 0x00, 0x99, 0x8e}},
        {{0x04, 0x00, 0xba, 0x00, 0x00, 0x00, 0xee, 0xa5}},
    }};
    for (size_t i = 0; i < known_payloads.size(); ++i) {
        assert(make_command_payload(static_cast<Direction>(i + 1)) == known_payloads[i]);
    }
    const auto payload = make_command_payload(Direction::Left);
    Direction direction{};
    assert(parse_command_payload(payload.data(), payload.size(), &direction));
    assert(direction == Direction::Left);
    assert(Crc::verify_crc8(payload.data(), 3));
    assert(Crc::verify_crc16(payload.data(), payload.size()));

    auto corrupt = payload;
    corrupt[1] ^= 0x01;
    assert(!parse_command_payload(corrupt.data(), corrupt.size(), &direction));

    RoiController roi(1200, 1200, 192, 192);
    assert(roi.position().x == 504 && roi.position().y == 504);
    roi.move(Direction::Up);
    roi.move(Direction::Left);
    assert(roi.position().x == 494 && roi.position().y == 494);
    for (int i = 0; i < 200; ++i) {
        roi.move(Direction::Up);
        roi.move(Direction::Left);
    }
    assert(roi.position().x == 0 && roi.position().y == 0);
    for (int i = 0; i < 200; ++i) {
        roi.move(Direction::Down);
        roi.move(Direction::Right);
    }
    assert(roi.position().x == 1008 && roi.position().y == 1008);

    std::printf("CAN CRC and ROI tests passed\n");
    return 0;
}
