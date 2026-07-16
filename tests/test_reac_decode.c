#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/reac_decode.h"

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__,   \
              #expr);                                                           \
      exit(1);                                                                  \
    }                                                                           \
  } while (0)

static void put_be16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)(value & 0xff);
}

static void put_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value & 0xff);
  data[1] = (uint8_t)(value >> 8);
}

static void put_reac_sample_pair(uint8_t *out, int32_t sample1, int32_t sample2)
{
  uint32_t value1 = (uint32_t)sample1 & 0x00ffffffu;
  uint32_t value2 = (uint32_t)sample2 & 0x00ffffffu;

  out[0] = (uint8_t)((value1 >> 8) & 0xff);
  out[1] = (uint8_t)((value1 >> 16) & 0xff);
  out[2] = (uint8_t)((value2 >> 16) & 0xff);
  out[3] = (uint8_t)(value1 & 0xff);
  out[4] = (uint8_t)(value2 & 0xff);
  out[5] = (uint8_t)((value2 >> 8) & 0xff);
}

static size_t make_packet(uint8_t *packet,
                          size_t packet_capacity,
                          uint16_t ethertype,
                          uint16_t counter,
                          uint16_t channels)
{
  size_t payload_size = reac_payload_bytes_for(channels, REAC_FRAMES_PER_PACKET);
  size_t packet_size = reac_packet_overhead() + payload_size + 2;
  ASSERT_TRUE(packet_capacity >= packet_size);

  memset(packet, 0, packet_size);
  put_be16(packet + 12, ethertype);
  put_le16(packet + 14, counter);

  uint8_t *payload = packet + reac_packet_overhead();
  for (size_t frame = 0; frame < REAC_FRAMES_PER_PACKET; frame++) {
    for (uint16_t ch = 0; ch < channels; ch += 2) {
      int32_t sample1 = (int32_t)(1000 + frame + ch);
      int32_t sample2 = (int32_t)(-1000 - (int32_t)frame - ch);
      put_reac_sample_pair(payload, sample1, sample2);
      payload += 6;
    }
  }

  packet[packet_size - 2] = REAC_END_MARKER_0;
  packet[packet_size - 1] = REAC_END_MARKER_1;
  return packet_size;
}

static void test_parse_valid_packet(void)
{
  uint8_t packet[2048];
  size_t packet_size = make_packet(packet, sizeof(packet), REAC_ETHERTYPE, 0x1234, 2);

  ReacPacketView view;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size, &view) == REAC_STATUS_OK);
  ASSERT_TRUE(view.counter == 0x1234);
  ASSERT_TRUE(view.channels == 2);
  ASSERT_TRUE(view.payload_size == reac_payload_bytes_for(2, REAC_FRAMES_PER_PACKET));
  ASSERT_TRUE(view.payload == packet + reac_packet_overhead());
}

static void test_rejects_non_reac_packet(void)
{
  uint8_t packet[2048];
  size_t packet_size = make_packet(packet, sizeof(packet), 0x0800, 0x1234, 2);

  ReacPacketView view;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size, &view) == REAC_STATUS_NOT_REAC);
}

static void test_rejects_short_packet(void)
{
  uint8_t packet[8];
  ReacPacketView view;
  memset(packet, 0, sizeof(packet));
  ASSERT_TRUE(reac_parse_packet(packet, sizeof(packet), &view) == REAC_STATUS_SHORT_PACKET);
}

static void test_rejects_bad_end_marker(void)
{
  uint8_t packet[2048];
  size_t packet_size = make_packet(packet, sizeof(packet), REAC_ETHERTYPE, 0x1234, 2);
  packet[packet_size - 1] = 0x00;

  ReacPacketView view;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size, &view) == REAC_STATUS_BAD_END_MARKER);
}

static void test_rejects_bad_payload_size(void)
{
  uint8_t packet[2048];
  size_t packet_size = make_packet(packet, sizeof(packet), REAC_ETHERTYPE, 0x1234, 2);

  ReacPacketView view;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size - 1, &view) == REAC_STATUS_BAD_END_MARKER);

  packet[packet_size - 3] = REAC_END_MARKER_0;
  packet[packet_size - 2] = REAC_END_MARKER_1;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size - 1, &view) ==
              REAC_STATUS_BAD_PAYLOAD_SIZE);
}

static void test_rejects_unsupported_channel_count(void)
{
  uint8_t packet[2048];
  size_t packet_size = make_packet(packet, sizeof(packet), REAC_ETHERTYPE, 0x1234, 1);

  ReacPacketView view;
  ASSERT_TRUE(reac_parse_packet(packet, packet_size, &view) ==
              REAC_STATUS_UNSUPPORTED_CHANNEL_COUNT);
}

static void test_decodes_samples_to_channel_buffers(void)
{
  uint8_t samples[12];
  float ch1[2] = {0};
  float ch2[2] = {0};
  float *outputs[2] = {ch1, ch2};

  put_reac_sample_pair(samples, 0x000001, -1);
  put_reac_sample_pair(samples + 6, 0x7fffff, -0x800000);

  ASSERT_TRUE(reac_decode_samples_to_channels(samples, sizeof(samples), 2, 2, outputs) ==
              REAC_STATUS_OK);
  ASSERT_TRUE(fabsf(ch1[0] - (1.0f / 8388608.0f)) < 0.0000001f);
  ASSERT_TRUE(fabsf(ch2[0] - (-1.0f / 8388608.0f)) < 0.0000001f);
  ASSERT_TRUE(fabsf(ch1[1] - (8388607.0f / 8388608.0f)) < 0.0000001f);
  ASSERT_TRUE(fabsf(ch2[1] - -1.0f) < 0.0000001f);
}

int main(void)
{
  test_parse_valid_packet();
  test_rejects_non_reac_packet();
  test_rejects_short_packet();
  test_rejects_bad_end_marker();
  test_rejects_bad_payload_size();
  test_rejects_unsupported_channel_count();
  test_decodes_samples_to_channel_buffers();

  puts("reac_decode tests passed");
  return 0;
}
