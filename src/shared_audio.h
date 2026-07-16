#ifndef SHARED_AUDIO_H_INCLUDED
#define SHARED_AUDIO_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_AUDIO_MAGIC 0x524a5341u /* "RJSA" */
#define SHARED_AUDIO_ABI_VERSION 1u
#define SHARED_AUDIO_NAME_MAX 32

/*
 * Single-writer/single-reader audio ring in POSIX shared memory.
 *
 * Audio is stored non-interleaved: channel c occupies capacity_frames
 * contiguous float samples starting at samples + c * capacity_frames.
 * Positions are monotonically increasing frame counts; the writer publishes
 * write_pos only after the samples are fully copied, so the reader never
 * observes partially written frames.
 */
typedef struct {
  uint32_t magic;
  uint32_t abi_version;
  uint32_t header_bytes;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t capacity_frames;
  uint64_t write_pos;
  uint64_t read_pos;
  uint64_t underruns;
  uint64_t overruns;
  uint64_t dropped_packets;
  uint64_t resets;
} SharedAudioHeader;

typedef struct {
  SharedAudioHeader *header;
  float *samples;
  size_t mapped_bytes;
  int owner;
  char name[SHARED_AUDIO_NAME_MAX];
} SharedAudio;

int shared_audio_create(SharedAudio *audio,
                        const char *name,
                        uint32_t sample_rate,
                        uint16_t channels,
                        uint32_t capacity_frames);
int shared_audio_open(SharedAudio *audio, const char *name);
void shared_audio_close(SharedAudio *audio);
int shared_audio_unlink(const char *name);

uint32_t shared_audio_writable_frames(const SharedAudio *audio);
uint32_t shared_audio_readable_frames(const SharedAudio *audio);

/* Writes all frames or none; on insufficient space increments overruns and
 * returns nonzero. */
int shared_audio_write(SharedAudio *audio, const float *const *channels, uint32_t frames);

/* Copies up to frames; missing frames become silence and increment underruns.
 * Returns the number of real frames copied. Never blocks. */
uint32_t shared_audio_read(SharedAudio *audio, float *const *channels, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
