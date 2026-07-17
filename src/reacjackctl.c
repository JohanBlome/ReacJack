#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared_audio.h"

#define REACJACKCTL_DEFAULT_RING "/reacjack-audio"

static void print_ring(const SharedAudio *ring)
{
  const SharedAudioHeader *header = ring->header;

  printf("ring: %s\n", ring->name);
  printf("abi_version: %u\n", header->abi_version);
  printf("sample_rate: %u\n", header->sample_rate);
  printf("channels: %u\n", header->channels);
  printf("capacity_frames: %u\n", header->capacity_frames);
  printf("fill_frames: %u\n", shared_audio_readable_frames(ring));
  printf("write_pos: %llu\n", (unsigned long long)header->write_pos);
  printf("read_pos: %llu\n", (unsigned long long)header->read_pos);
  printf("underruns: %llu\n", (unsigned long long)header->underruns);
  printf("overruns: %llu\n", (unsigned long long)header->overruns);
  printf("dropped_packets: %llu\n", (unsigned long long)header->dropped_packets);
  printf("resets: %llu\n", (unsigned long long)header->resets);
  printf("drift_inserted_frames: %llu\n", (unsigned long long)header->inserted_frames);
  printf("drift_dropped_frames: %llu\n", (unsigned long long)header->dropped_frames);

  uint64_t clock_host_time = 0;
  uint64_t clock_sample_pos = 0;
  if (shared_audio_get_clock(ring, &clock_host_time, &clock_sample_pos) == 0) {
    printf("clock_host_time: %llu\n", (unsigned long long)clock_host_time);
    printf("clock_sample_pos: %llu\n", (unsigned long long)clock_sample_pos);
  } else {
    printf("clock: not published\n");
  }
}

static void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s [-n /ring-name] [-w]\n"
                  "  -n  shared audio ring name (default %s)\n"
                  "  -w  watch: refresh every second until interrupted\n",
          argv0, REACJACKCTL_DEFAULT_RING);
}

int main(int argc, char *argv[])
{
  const char *ring_name = REACJACKCTL_DEFAULT_RING;
  int watch = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      ring_name = argv[++i];
    } else if (strcmp(argv[i], "-w") == 0) {
      watch = 1;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  SharedAudio ring;
  if (shared_audio_open(&ring, ring_name) != 0) {
    fprintf(stderr, "Failed to open shared audio ring %s. Is reacjackd running?\n",
            ring_name);
    return 1;
  }

  print_ring(&ring);
  while (watch) {
    sleep(1);
    printf("\n");
    print_ring(&ring);
  }

  shared_audio_close(&ring);
  return 0;
}
