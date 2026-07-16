#include "shared_audio.h"

int shared_audio_create(SharedAudio *audio,
                        const char *name,
                        uint32_t sample_rate,
                        uint16_t channels,
                        uint32_t capacity_frames)
{
  (void)audio;
  (void)name;
  (void)sample_rate;
  (void)channels;
  (void)capacity_frames;
  return -1;
}

int shared_audio_open(SharedAudio *audio, const char *name)
{
  (void)audio;
  (void)name;
  return -1;
}

void shared_audio_close(SharedAudio *audio)
{
  (void)audio;
}

int shared_audio_unlink(const char *name)
{
  (void)name;
  return -1;
}

uint32_t shared_audio_writable_frames(const SharedAudio *audio)
{
  (void)audio;
  return 0;
}

uint32_t shared_audio_readable_frames(const SharedAudio *audio)
{
  (void)audio;
  return 0;
}

int shared_audio_write(SharedAudio *audio, const float *const *channels, uint32_t frames)
{
  (void)audio;
  (void)channels;
  (void)frames;
  return -1;
}

uint32_t shared_audio_read(SharedAudio *audio, float *const *channels, uint32_t frames)
{
  (void)audio;
  (void)channels;
  (void)frames;
  return 0;
}
