#ifndef REAC_DECODE_H_INCLUDED
#define REAC_DECODE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REAC_ETHERTYPE 0x8819
#define REAC_MAX_CHANNELS 40
#define REAC_FRAMES_PER_PACKET 12
#define REAC_BYTES_PER_SAMPLE 3
#define REAC_END_MARKER_0 0xc2
#define REAC_END_MARKER_1 0xea

typedef enum {
  REAC_STATUS_OK = 0,
  REAC_STATUS_BAD_ARGUMENT,
  REAC_STATUS_NOT_REAC,
  REAC_STATUS_SHORT_PACKET,
  REAC_STATUS_BAD_END_MARKER,
  REAC_STATUS_BAD_PAYLOAD_SIZE,
  REAC_STATUS_UNSUPPORTED_CHANNEL_COUNT
} ReacStatus;

typedef struct {
  uint16_t counter;
  uint16_t channels;
  size_t payload_size;
  const uint8_t *payload;
} ReacPacketView;

size_t reac_packet_overhead(void);
size_t reac_payload_bytes_for(uint16_t channels, size_t frames);
ReacStatus reac_parse_packet(const uint8_t *frame, size_t frame_size, ReacPacketView *packet);
ReacStatus reac_decode_samples_to_channels(const uint8_t *samples,
                                           size_t sample_bytes,
                                           uint16_t channels,
                                           size_t frames,
                                           float *const *outputs);

#ifdef __cplusplus
}
#endif

#endif
