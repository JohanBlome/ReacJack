#ifndef SHARED_AUDIO_H_INCLUDED
#define SHARED_AUDIO_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_AUDIO_MAGIC 0x524a5341u /* "RJSA" */
#define SHARED_AUDIO_ABI_VERSION 3u
#define SHARED_AUDIO_NAME_MAX 32

/* Largest drift correction (frames inserted or dropped) per regulated read. */
#define SHARED_AUDIO_MAX_CORRECTION 8u

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
  uint64_t inserted_frames; /* drift corrections: duplicated frames */
  uint64_t dropped_frames;  /* drift corrections: skipped frames */
  /* Writer clock observations (seqlock: clock_seq is even when stable).
   * Readers derive the writer's true sample rate from these to slave their
   * own clock instead of correcting audio. */
  uint64_t clock_seq;
  uint64_t clock_host_time;
  uint64_t clock_sample_pos;
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

/* Like shared_audio_read, but interleaves into out (out_channels samples per
 * frame). Ring channels beyond out_channels are skipped; output channels
 * beyond the ring's become silence. */
uint32_t shared_audio_read_interleaved(SharedAudio *audio,
                                       float *out,
                                       uint16_t out_channels,
                                       uint32_t frames);

/* Moves read_pos so at most target_fill frames are buffered, counting a
 * reset. Returns the resulting fill. Used to align latency at IO start. */
uint32_t shared_audio_seek_to_fill(SharedAudio *audio, uint32_t target_fill);

/* Like shared_audio_read_interleaved, but keeps the ring fill near
 * target_fill to absorb clock drift between writer and reader: when fill
 * exceeds target_fill + tolerance, up to SHARED_AUDIO_MAX_CORRECTION frames
 * are dropped first (counted in dropped_frames); when fill is below
 * target_fill - tolerance, up to SHARED_AUDIO_MAX_CORRECTION output frames
 * are synthesized by duplicating the last real frame (counted in
 * inserted_frames). Returns the number of real frames consumed. */
uint32_t shared_audio_read_interleaved_regulated(SharedAudio *audio,
                                                 float *out,
                                                 uint16_t out_channels,
                                                 uint32_t frames,
                                                 uint32_t target_fill,
                                                 uint32_t tolerance);

/* Monotonic host clock in platform ticks: mach_absolute_time on macOS,
 * CLOCK_MONOTONIC nanoseconds elsewhere. Writer and readers share a host,
 * so the unit only needs to agree per platform. */
uint64_t shared_audio_host_time(void);

/* Writer: publish "sample_pos was written at host_time". Wait-free. */
void shared_audio_publish_clock(SharedAudio *audio,
                                uint64_t host_time,
                                uint64_t sample_pos);

/* Reader: fetch the latest observation. Returns nonzero if the writer has
 * never published (or the seqlock stayed contended). Never blocks. */
int shared_audio_get_clock(const SharedAudio *audio,
                           uint64_t *host_time,
                           uint64_t *sample_pos);

#ifdef __cplusplus
}
#endif

#endif
