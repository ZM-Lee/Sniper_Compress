#include "can/can.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace rmcc_sniper::can;

    static_assert(kDefaultCommandId == 0x180, "unexpected CAN command id");

    const std::array<std::array<uint8_t, kCommandPayloadSize>, 4> known_payloads{{
        {{0x01}},
        {{0x02}},
        {{0x03}},
        {{0x04}},
    }};
    for (size_t i = 0; i < known_payloads.size(); ++i) {
        assert(make_command_payload(static_cast<Direction>(i + 1)) == known_payloads[i]);
    }
    const auto payload = make_command_payload(Direction::Left);
    Direction direction{};
    assert(parse_command_payload(payload.data(), payload.size(), &direction));
    assert(direction == Direction::Left);

    auto corrupt = payload;
    corrupt[0] = 0x05;
    assert(!parse_command_payload(corrupt.data(), corrupt.size(), &direction));
    const uint8_t oversized[] = {0x01, 0x00};
    assert(!parse_command_payload(oversized, sizeof(oversized), &direction));

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

    std::printf("CAN command and ROI tests passed\n");
    return 0;
}
