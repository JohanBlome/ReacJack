#if !defined(__APPLE__)
#error "reacjackd currently targets the macOS libpcap capture path"
#endif

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pcap/pcap.h>

#include "reac_decode.h"
#include "shared_audio.h"

#define REACJACKD_DEFAULT_RING "/reacjack-audio"
#define REACJACKD_SAMPLE_RATE 48000
#define REACJACKD_RING_FRAMES REACJACKD_SAMPLE_RATE /* one second of audio */
#define REACJACKD_TONE_HZ 440.0
#define REACJACKD_TONE_AMPLITUDE 0.5f

typedef struct {
  SharedAudio ring;
  int ring_created;
  const char *ring_name;
  const char *interface;
  uint16_t channels; /* 0 = detect from the first REAC packet */
  int tone_mode;
  pcap_t *pcap;

  uint64_t received_packets;
  uint64_t lost_packets;
  uint64_t non_reac_packets;
  uint64_t malformed_packets;
  uint64_t capture_errors;
  uint16_t last_counter;
  int have_last_counter;
} ReacDaemon;

static volatile sig_atomic_t isRunning = 1;

static void request_shutdown(int signum)
{
  (void)signum;
  isRunning = 0;
}

static void note_counter(ReacDaemon *daemon, uint16_t counter)
{
  if (daemon->have_last_counter) {
    uint16_t expected = (uint16_t)(daemon->last_counter + 1);
    if (counter != expected) {
      uint16_t gap = (uint16_t)(counter - expected);
      daemon->lost_packets += gap < 32768 ? gap : 1;
    }
  }

  daemon->last_counter = counter;
  daemon->have_last_counter = 1;
}

static int ensure_ring(ReacDaemon *daemon, uint16_t channels)
{
  if (daemon->ring_created) {
    return 0;
  }

  if (shared_audio_create(&daemon->ring, daemon->ring_name, REACJACKD_SAMPLE_RATE,
                          channels, REACJACKD_RING_FRAMES) != 0) {
    fprintf(stderr, "Failed to create shared audio ring %s\n", daemon->ring_name);
    return -1;
  }

  daemon->ring_created = 1;
  daemon->channels = channels;
  fprintf(stderr, "Shared audio ring %s ready: %u channels, %u Hz\n",
          daemon->ring_name, channels, REACJACKD_SAMPLE_RATE);
  return 0;
}

static void print_status(const ReacDaemon *daemon)
{
  uint32_t fill = daemon->ring_created ? shared_audio_readable_frames(&daemon->ring) : 0;
  uint64_t overruns = daemon->ring_created ? daemon->ring.header->overruns : 0;
  uint64_t underruns = daemon->ring_created ? daemon->ring.header->underruns : 0;
  uint64_t inserted = daemon->ring_created ? daemon->ring.header->inserted_frames : 0;
  uint64_t dropped = daemon->ring_created ? daemon->ring.header->dropped_frames : 0;

  fprintf(stderr,
          "received=%llu lost=%llu malformed=%llu non_reac=%llu capture_errors=%llu "
          "ring_fill=%u overruns=%llu underruns=%llu drift_ins=%llu drift_drop=%llu\n",
          (unsigned long long)daemon->received_packets,
          (unsigned long long)daemon->lost_packets,
          (unsigned long long)daemon->malformed_packets,
          (unsigned long long)daemon->non_reac_packets,
          (unsigned long long)daemon->capture_errors, fill,
          (unsigned long long)overruns, (unsigned long long)underruns,
          (unsigned long long)inserted, (unsigned long long)dropped);
}

static int capture_open(ReacDaemon *daemon)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  errbuf[0] = '\0';

  daemon->pcap = pcap_create(daemon->interface, errbuf);
  if (daemon->pcap == NULL) {
    fprintf(stderr, "pcap_create(%s): %s\n", daemon->interface, errbuf);
    return -1;
  }

  pcap_set_snaplen(daemon->pcap, 2048);
  pcap_set_promisc(daemon->pcap, 1);
  pcap_set_timeout(daemon->pcap, 100);
  pcap_set_immediate_mode(daemon->pcap, 1);

  if (pcap_activate(daemon->pcap) < 0) {
    fprintf(stderr, "pcap_activate(%s): %s\n", daemon->interface,
            pcap_geterr(daemon->pcap));
    return -1;
  }
  if (pcap_datalink(daemon->pcap) != DLT_EN10MB) {
    fprintf(stderr, "pcap interface %s is not exposing Ethernet frames\n",
            daemon->interface);
    return -1;
  }

  struct bpf_program filter;
  if (pcap_compile(daemon->pcap, &filter, "ether proto 0x8819", 1,
                   PCAP_NETMASK_UNKNOWN) == 0) {
    if (pcap_setfilter(daemon->pcap, &filter) != 0) {
      fprintf(stderr, "pcap_setfilter: %s\n", pcap_geterr(daemon->pcap));
    }
    pcap_freecode(&filter);
  } else {
    fprintf(stderr, "pcap_compile: %s\n", pcap_geterr(daemon->pcap));
  }

  return 0;
}

static int run_capture(ReacDaemon *daemon)
{
  float channel_storage[REAC_MAX_CHANNELS][REAC_FRAMES_PER_PACKET];
  float *channel_ptrs[REAC_MAX_CHANNELS];
  time_t last_status = time(NULL);

  for (uint16_t ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
    channel_ptrs[ch] = channel_storage[ch];
  }

  if (capture_open(daemon) != 0) {
    return -1;
  }
  if (daemon->channels != 0 && ensure_ring(daemon, daemon->channels) != 0) {
    return -1;
  }

  while (isRunning) {
    time_t now = time(NULL);
    if (now - last_status >= 2) {
      print_status(daemon);
      last_status = now;
    }

    struct pcap_pkthdr *header = NULL;
    const unsigned char *packet = NULL;
    int rc = pcap_next_ex(daemon->pcap, &header, &packet);
    if (rc == 0) {
      continue; /* capture timeout: keep the status loop alive */
    }
    if (rc == -2) {
      break;
    }
    if (rc != 1) {
      fprintf(stderr, "pcap_next_ex: %s\n", pcap_geterr(daemon->pcap));
      daemon->capture_errors++;
      usleep(1000);
      continue;
    }

    ReacPacketView view;
    ReacStatus parse_status = reac_parse_packet(packet, header->caplen, &view);
    if (parse_status == REAC_STATUS_NOT_REAC) {
      daemon->non_reac_packets++;
      continue;
    }
    if (parse_status != REAC_STATUS_OK) {
      daemon->malformed_packets++;
      continue;
    }

    if (ensure_ring(daemon, view.channels) != 0) {
      return -1;
    }
    if (view.channels != daemon->channels) {
      daemon->malformed_packets++;
      continue;
    }

    if (reac_decode_samples_to_channels(view.payload, view.payload_size, view.channels,
                                        REAC_FRAMES_PER_PACKET,
                                        channel_ptrs) != REAC_STATUS_OK) {
      daemon->malformed_packets++;
      continue;
    }

    if (shared_audio_write(&daemon->ring, (const float *const *)channel_ptrs,
                           REAC_FRAMES_PER_PACKET) != 0) {
      daemon->ring.header->dropped_packets++;
    } else {
      /* Clock observation for readers that slave to the REAC rate. */
      shared_audio_publish_clock(&daemon->ring, shared_audio_host_time(),
                                 daemon->ring.header->write_pos);
    }

    note_counter(daemon, view.counter);
    daemon->received_packets++;
  }

  return 0;
}

static int run_tone(ReacDaemon *daemon)
{
  float channel_storage[REAC_MAX_CHANNELS][REAC_FRAMES_PER_PACKET];
  float *channel_ptrs[REAC_MAX_CHANNELS];
  struct timespec start;
  uint64_t sent_frames = 0;
  double phase = 0.0;
  double phase_step = 2.0 * M_PI * REACJACKD_TONE_HZ / REACJACKD_SAMPLE_RATE;
  time_t last_status = time(NULL);

  for (uint16_t ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
    channel_ptrs[ch] = channel_storage[ch];
  }

  if (ensure_ring(daemon, daemon->channels) != 0) {
    return -1;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);
  while (isRunning) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed_ns = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull +
                          (uint64_t)(now.tv_nsec - start.tv_nsec);
    uint64_t due_frames = elapsed_ns * REACJACKD_SAMPLE_RATE / 1000000000ull;

    while (sent_frames + REAC_FRAMES_PER_PACKET <= due_frames) {
      for (uint32_t i = 0; i < REAC_FRAMES_PER_PACKET; i++) {
        float sample = REACJACKD_TONE_AMPLITUDE * (float)sin(phase);
        phase += phase_step;
        for (uint16_t ch = 0; ch < daemon->channels; ch++) {
          channel_storage[ch][i] = sample;
        }
      }

      if (shared_audio_write(&daemon->ring, (const float *const *)channel_ptrs,
                             REAC_FRAMES_PER_PACKET) != 0) {
        daemon->ring.header->dropped_packets++;
      } else {
        shared_audio_publish_clock(&daemon->ring, shared_audio_host_time(),
                                   daemon->ring.header->write_pos);
      }
      sent_frames += REAC_FRAMES_PER_PACKET;
      daemon->received_packets++;
    }

    time_t status_now = time(NULL);
    if (status_now - last_status >= 2) {
      print_status(daemon);
      last_status = status_now;
    }

    usleep(1000);
  }

  return 0;
}

static void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s -i interface [-c channels] [-n /ring-name]\n"
          "       %s -tone [-c channels] [-n /ring-name]\n"
          "\n"
          "Captures REAC audio and writes it to a shared memory ring for\n"
          "CoreAudio (and other) readers. -tone replaces capture with a\n"
          "synthetic 440 Hz test signal. Without -c the channel count is\n"
          "detected from the first REAC packet.\n",
          argv0, argv0);
}

int main(int argc, char *argv[])
{
  ReacDaemon daemon;
  memset(&daemon, 0, sizeof(daemon));
  daemon.ring_name = REACJACKD_DEFAULT_RING;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      daemon.interface = argv[++i];
    } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      daemon.ring_name = argv[++i];
    } else if (strcmp(argv[i], "-tone") == 0) {
      daemon.tone_mode = 1;
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      long channels = strtol(argv[++i], NULL, 10);
      if (channels <= 0 || channels > REAC_MAX_CHANNELS || (channels % 2) != 0) {
        fprintf(stderr, "Unsupported channel count: %ld\n", channels);
        return 1;
      }
      daemon.channels = (uint16_t)channels;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (!daemon.tone_mode && daemon.interface == NULL) {
    usage(argv[0]);
    return 1;
  }
  if (daemon.tone_mode && daemon.channels == 0) {
    daemon.channels = 2;
  }

  signal(SIGINT, request_shutdown);
  signal(SIGTERM, request_shutdown);

  int result = daemon.tone_mode ? run_tone(&daemon) : run_capture(&daemon);

  print_status(&daemon);
  if (daemon.pcap != NULL) {
    pcap_close(daemon.pcap);
  }
  if (daemon.ring_created) {
    shared_audio_close(&daemon.ring);
    shared_audio_unlink(daemon.ring_name);
  }

  return result == 0 ? 0 : 1;
}
