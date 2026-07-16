#include "reac_decode.h"

#include <string.h>

#include "reac.h"

enum {
  ETHERNET_HEADER_BYTES = 14,
  REAC_PACKET_HEADER_BYTES = 36,
  REAC_END_MARKER_BYTES = 2
};

static uint16_t read_be16(const uint8_t *data)
{
  return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint16_t read_le16(const uint8_t *data)
{
  return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int32_t sign_extend_24(uint32_t value)
{
  if (value & 0x800000u) {
    value |= 0xff000000u;
  }

  return (int32_t)value;
}

static float sample24_to_float(int32_t value)
{
  return (float)value / (float)MAX_INT24;
}

size_t reac_packet_overhead(void)
{
  return ETHERNET_HEADER_BYTES + REAC_PACKET_HEADER_BYTES;
}

size_t reac_payload_bytes_for(uint16_t channels, size_t frames)
{
  return (size_t)channels * frames * REAC_BYTES_PER_SAMPLE;
}

ReacStatus reac_parse_packet(const uint8_t *frame, size_t frame_size, ReacPacketView *packet)
{
  if (frame == NULL || packet == NULL) {
    return REAC_STATUS_BAD_ARGUMENT;
  }

  memset(packet, 0, sizeof(*packet));

  size_t overhead = reac_packet_overhead();
  if (frame_size < overhead + REAC_END_MARKER_BYTES) {
    return REAC_STATUS_SHORT_PACKET;
  }

  if (read_be16(frame + 12) != REAC_ETHERTYPE) {
    return REAC_STATUS_NOT_REAC;
  }

  if (frame[frame_size - 2] != REAC_END_MARKER_0 ||
      frame[frame_size - 1] != REAC_END_MARKER_1) {
    return REAC_STATUS_BAD_END_MARKER;
  }

  size_t payload_size = frame_size - overhead - REAC_END_MARKER_BYTES;
  size_t packet_quantum = reac_payload_bytes_for(1, REAC_FRAMES_PER_PACKET);
  if (payload_size == 0 || payload_size % packet_quantum != 0) {
    return REAC_STATUS_BAD_PAYLOAD_SIZE;
  }

  size_t channels = payload_size / packet_quantum;
  if (channels == 0 || channels > REAC_MAX_CHANNELS || (channels % 2) != 0) {
    return REAC_STATUS_UNSUPPORTED_CHANNEL_COUNT;
  }

  packet->counter = read_le16(frame + ETHERNET_HEADER_BYTES);
  packet->channels = (uint16_t)channels;
  packet->payload_size = payload_size;
  packet->payload = frame + overhead;
  return REAC_STATUS_OK;
}

ReacStatus reac_decode_samples_to_channels(const uint8_t *samples,
                                           size_t sample_bytes,
                                           uint16_t channels,
                                           size_t frames,
                                           float *const *outputs)
{
  if (samples == NULL || outputs == NULL) {
    return REAC_STATUS_BAD_ARGUMENT;
  }
  if (channels == 0 || channels > REAC_MAX_CHANNELS || (channels % 2) != 0) {
    return REAC_STATUS_UNSUPPORTED_CHANNEL_COUNT;
  }
  if (sample_bytes != reac_payload_bytes_for(channels, frames)) {
    return REAC_STATUS_BAD_PAYLOAD_SIZE;
  }

  for (uint16_t ch = 0; ch < channels; ch++) {
    if (outputs[ch] == NULL) {
      return REAC_STATUS_BAD_ARGUMENT;
    }
  }

  const uint8_t *in = samples;
  for (size_t frame = 0; frame < frames; frame++) {
    for (uint16_t ch = 0; ch < channels; ch += 2) {
      uint32_t sample1 = ((uint32_t)in[3]) | ((uint32_t)in[0] << 8) |
                         ((uint32_t)in[1] << 16);
      uint32_t sample2 = ((uint32_t)in[4]) | ((uint32_t)in[5] << 8) |
                         ((uint32_t)in[2] << 16);

      outputs[ch][frame] = sample24_to_float(sign_extend_24(sample1));
      outputs[ch + 1][frame] = sample24_to_float(sign_extend_24(sample2));

      in += 6;
    }
  }

  return REAC_STATUS_OK;
}
