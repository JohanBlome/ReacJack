#include "shared_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SHARED_AUDIO_SAMPLES_ALIGN = 64 };

static size_t samples_offset(void)
{
  return (sizeof(SharedAudioHeader) + (SHARED_AUDIO_SAMPLES_ALIGN - 1)) &
         ~((size_t)SHARED_AUDIO_SAMPLES_ALIGN - 1);
}

static size_t mapping_bytes(uint32_t channels, uint32_t capacity_frames)
{
  return samples_offset() + (size_t)channels * capacity_frames * sizeof(float);
}

static uint64_t load_pos(const uint64_t *pos)
{
  return __atomic_load_n(pos, __ATOMIC_ACQUIRE);
}

static void store_pos(uint64_t *pos, uint64_t value)
{
  __atomic_store_n(pos, value, __ATOMIC_RELEASE);
}

static int map_shared(SharedAudio *audio, int fd, size_t bytes)
{
  void *mapped = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    return -1;
  }

  audio->header = (SharedAudioHeader *)mapped;
  audio->samples = (float *)((uint8_t *)mapped + samples_offset());
  audio->mapped_bytes = bytes;
  return 0;
}

static int init_handle(SharedAudio *audio, const char *name)
{
  if (audio == NULL || name == NULL || name[0] != '/' ||
      strlen(name) >= sizeof(audio->name)) {
    return -1;
  }

  memset(audio, 0, sizeof(*audio));
  snprintf(audio->name, sizeof(audio->name), "%s", name);
  return 0;
}

int shared_audio_create(SharedAudio *audio,
                        const char *name,
                        uint32_t sample_rate,
                        uint16_t channels,
                        uint32_t capacity_frames)
{
  if (init_handle(audio, name) != 0 || sample_rate == 0 || channels == 0 ||
      capacity_frames == 0) {
    return -1;
  }

  shm_unlink(name); /* replace a stale ring from a dead writer */
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return -1;
  }

  size_t bytes = mapping_bytes(channels, capacity_frames);
  if (ftruncate(fd, (off_t)bytes) != 0 || map_shared(audio, fd, bytes) != 0) {
    close(fd);
    shm_unlink(name);
    return -1;
  }
  close(fd);

  audio->header->magic = SHARED_AUDIO_MAGIC;
  audio->header->abi_version = SHARED_AUDIO_ABI_VERSION;
  audio->header->header_bytes = (uint32_t)sizeof(SharedAudioHeader);
  audio->header->sample_rate = sample_rate;
  audio->header->channels = channels;
  audio->header->capacity_frames = capacity_frames;
  audio->owner = 1;
  return 0;
}

int shared_audio_open(SharedAudio *audio, const char *name)
{
  if (init_handle(audio, name) != 0) {
    return -1;
  }

  int fd = shm_open(name, O_RDWR, 0);
  if (fd < 0) {
    return -1;
  }

  struct stat status;
  if (fstat(fd, &status) != 0 || (size_t)status.st_size < sizeof(SharedAudioHeader)) {
    close(fd);
    return -1;
  }

  if (map_shared(audio, fd, (size_t)status.st_size) != 0) {
    close(fd);
    return -1;
  }
  close(fd);

  SharedAudioHeader *header = audio->header;
  if (header->magic != SHARED_AUDIO_MAGIC ||
      header->abi_version != SHARED_AUDIO_ABI_VERSION ||
      header->header_bytes != (uint32_t)sizeof(SharedAudioHeader) ||
      header->channels == 0 || header->capacity_frames == 0 ||
      (size_t)status.st_size < mapping_bytes(header->channels, header->capacity_frames)) {
    shared_audio_close(audio);
    return -1;
  }

  return 0;
}

void shared_audio_close(SharedAudio *audio)
{
  if (audio == NULL || audio->header == NULL) {
    return;
  }

  munmap(audio->header, audio->mapped_bytes);
  audio->header = NULL;
  audio->samples = NULL;
  audio->mapped_bytes = 0;
}

int shared_audio_unlink(const char *name)
{
  return shm_unlink(name);
}

uint32_t shared_audio_writable_frames(const SharedAudio *audio)
{
  const SharedAudioHeader *header = audio->header;
  uint64_t used = load_pos(&header->write_pos) - load_pos(&header->read_pos);
  return header->capacity_frames - (uint32_t)used;
}

uint32_t shared_audio_readable_frames(const SharedAudio *audio)
{
  const SharedAudioHeader *header = audio->header;
  return (uint32_t)(load_pos(&header->write_pos) - load_pos(&header->read_pos));
}

int shared_audio_write(SharedAudio *audio, const float *const *channels, uint32_t frames)
{
  SharedAudioHeader *header = audio->header;

  if (frames > shared_audio_writable_frames(audio)) {
    header->overruns++;
    return -1;
  }

  uint64_t write_pos = load_pos(&header->write_pos);
  uint32_t capacity = header->capacity_frames;
  uint32_t index = (uint32_t)(write_pos % capacity);
  uint32_t first = frames < capacity - index ? frames : capacity - index;

  for (uint32_t ch = 0; ch < header->channels; ch++) {
    float *base = audio->samples + (size_t)ch * capacity;
    memcpy(base + index, channels[ch], first * sizeof(float));
    if (first < frames) {
      memcpy(base, channels[ch] + first, (frames - first) * sizeof(float));
    }
  }

  store_pos(&header->write_pos, write_pos + frames);
  return 0;
}

uint32_t shared_audio_read_interleaved(SharedAudio *audio,
                                       float *out,
                                       uint16_t out_channels,
                                       uint32_t frames)
{
  SharedAudioHeader *header = audio->header;
  uint64_t read_pos = load_pos(&header->read_pos);
  uint64_t available = load_pos(&header->write_pos) - read_pos;
  uint32_t to_read = available < frames ? (uint32_t)available : frames;
  uint32_t capacity = header->capacity_frames;
  uint32_t copy_channels =
      header->channels < out_channels ? (uint32_t)header->channels : out_channels;

  memset(out, 0, (size_t)frames * out_channels * sizeof(float));
  for (uint32_t ch = 0; ch < copy_channels; ch++) {
    const float *base = audio->samples + (size_t)ch * capacity;
    for (uint32_t frame = 0; frame < to_read; frame++) {
      out[(size_t)frame * out_channels + ch] =
          base[(uint32_t)((read_pos + frame) % capacity)];
    }
  }

  if (to_read < frames) {
    header->underruns++;
  }

  store_pos(&header->read_pos, read_pos + to_read);
  return to_read;
}

uint32_t shared_audio_read(SharedAudio *audio, float *const *channels, uint32_t frames)
{
  SharedAudioHeader *header = audio->header;
  uint64_t read_pos = load_pos(&header->read_pos);
  uint64_t available = load_pos(&header->write_pos) - read_pos;
  uint32_t to_read = available < frames ? (uint32_t)available : frames;
  uint32_t capacity = header->capacity_frames;
  uint32_t index = (uint32_t)(read_pos % capacity);
  uint32_t first = to_read < capacity - index ? to_read : capacity - index;

  for (uint32_t ch = 0; ch < header->channels; ch++) {
    const float *base = audio->samples + (size_t)ch * capacity;
    memcpy(channels[ch], base + index, first * sizeof(float));
    if (first < to_read) {
      memcpy(channels[ch] + first, base, (to_read - first) * sizeof(float));
    }
    if (to_read < frames) {
      memset(channels[ch] + to_read, 0, (frames - to_read) * sizeof(float));
    }
  }

  if (to_read < frames) {
    header->underruns++;
  }

  store_pos(&header->read_pos, read_pos + to_read);
  return to_read;
}
