#include <gtest/gtest.h>

#include <cstring>

#include "driver_protocol.h"

TEST(DriverProtocolTest, MultiByteFieldsUseLittleEndianWireOrder) {
    veda_risk_event_t event;
    std::memset(&event, 0, sizeof(event));
    veda_write_i64_le(&event.timestamp_ms, INT64_C(0x0102030405060708));
    veda_write_u16_le(&event.dist_mm, UINT16_C(0x1234));

    const auto* timestamp = reinterpret_cast<const uint8_t*>(&event.timestamp_ms);
    EXPECT_EQ(timestamp[0], 0x08);
    EXPECT_EQ(timestamp[7], 0x01);
    const auto* distance = reinterpret_cast<const uint8_t*>(&event.dist_mm);
    EXPECT_EQ(distance[0], 0x34);
    EXPECT_EQ(distance[1], 0x12);
    EXPECT_EQ(veda_read_i64_le(&event.timestamp_ms), INT64_C(0x0102030405060708));
    EXPECT_EQ(veda_read_u16_le(&event.dist_mm), UINT16_C(0x1234));
}

TEST(DriverProtocolTest, WireSizesRemainStable) {
    EXPECT_EQ(sizeof(veda_risk_event_t), 24u);
    EXPECT_EQ(sizeof(veda_uplink_packet_t), 16u);
    EXPECT_EQ(sizeof(veda_downlink_frame_t), 27u);
    EXPECT_EQ(sizeof(veda_uplink_frame_t), 19u);
}

TEST(DriverProtocolTest, RejectsInvalidWireFieldsAfterChecksum) {
    veda_uplink_packet_t packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.reason = VEDA_UPLINK_REASON_HEARTBEAT;
    EXPECT_TRUE(veda_uplink_payload_is_valid(&packet));

    packet.reason = 0xFF;
    EXPECT_FALSE(veda_uplink_payload_is_valid(&packet));
    packet.reason = VEDA_UPLINK_REASON_ACK;

    packet.led_red = 2;
    EXPECT_FALSE(veda_uplink_payload_is_valid(&packet));
    packet.led_red = 0;

    packet.reserved0[0] = 1;
    EXPECT_FALSE(veda_uplink_payload_is_valid(&packet));
    EXPECT_FALSE(veda_uplink_payload_is_valid(nullptr));
}
