#if !defined(__linux__)
#error "reacjack-pw is a Linux PipeWire client"
#endif

#include <errno.h>
#include <ifaddrs.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include "reac_decode.h"

#define REACJACK_PW_NAME "ReacJack REAC"
#define REACJACK_PW_SAMPLE_RATE 48000
#define REACJACK_PW_RING_FRAMES (REACJACK_PW_SAMPLE_RATE * 2)
#define REACJACK_PW_PACKET_BYTES 2048

typedef struct {
  float *samples;
  size_t capacity_frames;
  uint16_t channels;
  size_t read_pos;
  size_t write_pos;
} AudioRing;

typedef struct {
  AudioRing ring;
  struct pw_main_loop *loop;
  struct pw_stream *stream;
  struct spa_hook stream_listener;
  pthread_t capture_thread;
  int capture_thread_started;
  int socket_fd;
  char interface[IF_NAMESIZE];
  uint16_t channels;
  volatile sig_atomic_t running;
  uint64_t received_packets;
  uint64_t non_reac_packets;
  uint64_t malformed_packets;
  uint64_t lost_packets;
  uint64_t capture_errors;
  uint64_t ring_overruns;
  uint64_t pipewire_underruns;
  uint16_t last_counter;
  int have_last_counter;
} ReacPw;

static size_t atomic_load_size(const size_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void atomic_store_size(size_t *value, size_t next)
{
  __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static int audio_ring_init(AudioRing *ring, uint16_t channels, size_t capacity_frames)
{
  memset(ring, 0, sizeof(*ring));
  ring->samples = (float *)calloc(capacity_frames * channels, sizeof(float));
  if (ring->samples == NULL) {
    return -1;
  }

  ring->capacity_frames = capacity_frames;
  ring->channels = channels;
  return 0;
}

static void audio_ring_destroy(AudioRing *ring)
{
  free(ring->samples);
  memset(ring, 0, sizeof(*ring));
}

static int audio_ring_write(AudioRing *ring, const float *interleaved, size_t frames)
{
  size_t read_pos = atomic_load_size(&ring->read_pos);
  size_t write_pos = ring->write_pos;
  size_t used = write_pos - read_pos;
  size_t free_frames = ring->capacity_frames - used;

  if (frames > free_frames) {
    return -1;
  }

  size_t written = 0;
  while (written < frames) {
    size_t write_index = (write_pos + written) % ring->capacity_frames;
    size_t contiguous = ring->capacity_frames - write_index;
    size_t todo = frames - written;
    if (todo > contiguous) {
      todo = contiguous;
    }

    memcpy(ring->samples + (write_index * ring->channels),
           interleaved + (written * ring->channels),
           todo * ring->channels * sizeof(float));
    written += todo;
  }

  atomic_store_size(&ring->write_pos, write_pos + frames);
  return 0;
}

static size_t audio_ring_read(AudioRing *ring, float *out, size_t frames)
{
  size_t read_pos = ring->read_pos;
  size_t write_pos = atomic_load_size(&ring->write_pos);
  size_t available = write_pos - read_pos;
  size_t to_read = available < frames ? available : frames;
  size_t read_frames = 0;

  while (read_frames < to_read) {
    size_t read_index = (read_pos + read_frames) % ring->capacity_frames;
    size_t contiguous = ring->capacity_frames - read_index;
    size_t todo = to_read - read_frames;
    if (todo > contiguous) {
      todo = contiguous;
    }

    memcpy(out + (read_frames * ring->channels),
           ring->samples + (read_index * ring->channels),
           todo * ring->channels * sizeof(float));
    read_frames += todo;
  }

  if (to_read < frames) {
    memset(out + (to_read * ring->channels), 0,
           (frames - to_read) * ring->channels * sizeof(float));
  }

  atomic_store_size(&ring->read_pos, read_pos + to_read);
  return to_read;
}

static int choose_interface(char *name, size_t name_size)
{
  struct ifaddrs *ifaddr;

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return -1;
  }

  int shown = 0;
  for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET) {
      continue;
    }

    shown++;
    printf("%d - %-8s\n", shown, ifa->ifa_name);
  }

  if (shown == 0) {
    fprintf(stderr, "No packet capture interfaces found\n");
    freeifaddrs(ifaddr);
    return -1;
  }

  printf("Enter the interface number: ");
  int selected = -1;
  if (scanf("%d", &selected) != 1 || selected < 1 || selected > shown) {
    fprintf(stderr, "No valid interface selected\n");
    freeifaddrs(ifaddr);
    return -1;
  }

  int current = 0;
  for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET) {
      continue;
    }

    current++;
    if (current == selected) {
      snprintf(name, name_size, "%s", ifa->ifa_name);
      freeifaddrs(ifaddr);
      return 0;
    }
  }

  freeifaddrs(ifaddr);
  return -1;
}

static int open_capture_socket(const char *interface)
{
  int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) {
    perror("socket PF_PACKET. Root privileges or cap_net_raw are required");
    return -1;
  }

  struct sockaddr_ll address;
  memset(&address, 0, sizeof(address));
  address.sll_family = PF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = (int)if_nametoindex(interface);
  if (address.sll_ifindex == 0) {
    fprintf(stderr, "Unknown interface: %s\n", interface);
    close(fd);
    return -1;
  }

  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  return fd;
}

static void note_counter(ReacPw *app, uint16_t counter)
{
  if (app->have_last_counter) {
    uint16_t expected = (uint16_t)(app->last_counter + 1);
    if (counter != expected) {
      uint16_t gap = (uint16_t)(counter - expected);
      app->lost_packets += gap < 32768 ? gap : 1;
    }
  }

  app->last_counter = counter;
  app->have_last_counter = 1;
}

static void interleave_packet(float *interleaved,
                              float *const *channels,
                              uint16_t channel_count,
                              size_t frames)
{
  for (size_t frame = 0; frame < frames; frame++) {
    for (uint16_t ch = 0; ch < channel_count; ch++) {
      interleaved[(frame * channel_count) + ch] = channels[ch][frame];
    }
  }
}

static void *capture_thread_main(void *argument)
{
  ReacPw *app = (ReacPw *)argument;
  uint8_t packet_bytes[REACJACK_PW_PACKET_BYTES];
  float channel_storage[REAC_MAX_CHANNELS][REAC_FRAMES_PER_PACKET];
  float *channel_ptrs[REAC_MAX_CHANNELS];
  float interleaved[REAC_MAX_CHANNELS * REAC_FRAMES_PER_PACKET];

  for (uint16_t ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
    channel_ptrs[ch] = channel_storage[ch];
  }

  while (app->running) {
    struct pollfd pollfd;
    pollfd.fd = app->socket_fd;
    pollfd.events = POLLIN;
    pollfd.revents = 0;

    int poll_result = poll(&pollfd, 1, 250);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      app->capture_errors++;
      usleep(1000);
      continue;
    }
    if (poll_result == 0) {
      continue;
    }

    ssize_t bytes_read = recv(app->socket_fd, packet_bytes, sizeof(packet_bytes), 0);
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("recv");
      app->capture_errors++;
      usleep(1000);
      continue;
    }

    ReacPacketView packet;
    ReacStatus parse_status =
        reac_parse_packet(packet_bytes, (size_t)bytes_read, &packet);
    if (parse_status == REAC_STATUS_NOT_REAC) {
      app->non_reac_packets++;
      continue;
    }
    if (parse_status != REAC_STATUS_OK) {
      app->malformed_packets++;
      continue;
    }

    if (packet.channels != app->channels) {
      app->malformed_packets++;
      continue;
    }

    ReacStatus decode_status =
        reac_decode_samples_to_channels(packet.payload, packet.payload_size, packet.channels,
                                        REAC_FRAMES_PER_PACKET, channel_ptrs);
    if (decode_status != REAC_STATUS_OK) {
      app->malformed_packets++;
      continue;
    }

    interleave_packet(interleaved, channel_ptrs, app->channels, REAC_FRAMES_PER_PACKET);
    if (audio_ring_write(&app->ring, interleaved, REAC_FRAMES_PER_PACKET) != 0) {
      app->ring_overruns++;
      continue;
    }

    note_counter(app, packet.counter);
    app->received_packets++;
  }

  return NULL;
}

static void on_process(void *userdata)
{
  ReacPw *app = (ReacPw *)userdata;
  struct pw_buffer *buffer = pw_stream_dequeue_buffer(app->stream);
  if (buffer == NULL) {
    return;
  }

  struct spa_buffer *spa_buffer = buffer->buffer;
  if (spa_buffer->n_datas == 0 || spa_buffer->datas[0].data == NULL ||
      spa_buffer->datas[0].chunk == NULL) {
    pw_stream_queue_buffer(app->stream, buffer);
    return;
  }

  size_t stride = (size_t)app->channels * sizeof(float);
  size_t frames = spa_buffer->datas[0].maxsize / stride;
  if (buffer->requested > 0 && buffer->requested < frames) {
    frames = buffer->requested;
  }

  float *out = (float *)spa_buffer->datas[0].data;
  size_t read_frames = audio_ring_read(&app->ring, out, frames);
  if (read_frames < frames) {
    app->pipewire_underruns++;
  }

  spa_buffer->datas[0].chunk->offset = 0;
  spa_buffer->datas[0].chunk->stride = (int32_t)stride;
  spa_buffer->datas[0].chunk->size = (uint32_t)(frames * stride);
  pw_stream_queue_buffer(app->stream, buffer);
}

static void on_state_changed(void *userdata,
                             enum pw_stream_state old_state,
                             enum pw_stream_state state,
                             const char *error_message)
{
  (void)userdata;
  (void)old_state;

  fprintf(stderr, "PipeWire stream state: %s", pw_stream_state_as_string(state));
  if (error_message != NULL) {
    fprintf(stderr, " (%s)", error_message);
  }
  fprintf(stderr, "\n");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
  (void)signal_number;
  ReacPw *app = (ReacPw *)userdata;
  app->running = 0;
  pw_main_loop_quit(app->loop);
}

static int start_pipewire_stream(ReacPw *app)
{
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  struct spa_audio_info_raw info;
  memset(&info, 0, sizeof(info));
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = REACJACK_PW_SAMPLE_RATE;
  info.channels = app->channels;

  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

  app->stream = pw_stream_new_simple(
      pw_main_loop_get_loop(app->loop), REACJACK_PW_NAME,
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, "reacjack.reac",
                        PW_KEY_NODE_DESCRIPTION, REACJACK_PW_NAME, NULL),
      &stream_events, app);
  if (app->stream == NULL) {
    fprintf(stderr, "Failed to create PipeWire stream\n");
    return -1;
  }

  if (pw_stream_connect(app->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                            PW_STREAM_FLAG_RT_PROCESS,
                        params, 1) != 0) {
    fprintf(stderr, "Failed to connect PipeWire stream\n");
    return -1;
  }

  return 0;
}

static void print_summary(const ReacPw *app)
{
  fprintf(stderr,
          "\nPackets: received=%llu lost=%llu non_reac=%llu malformed=%llu\n"
          "Errors: capture=%llu ring_overrun=%llu pipewire_underrun=%llu\n",
          (unsigned long long)app->received_packets,
          (unsigned long long)app->lost_packets,
          (unsigned long long)app->non_reac_packets,
          (unsigned long long)app->malformed_packets,
          (unsigned long long)app->capture_errors,
          (unsigned long long)app->ring_overruns,
          (unsigned long long)app->pipewire_underruns);
}

static void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s [-i interface] [-c channels]\n", argv0);
}

int main(int argc, char *argv[])
{
  ReacPw app;
  memset(&app, 0, sizeof(app));
  app.socket_fd = -1;
  app.channels = REAC_MAX_CHANNELS;
  app.running = 1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      snprintf(app.interface, sizeof(app.interface), "%s", argv[++i]);
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      long channels = strtol(argv[++i], NULL, 10);
      if (channels <= 0 || channels > REAC_MAX_CHANNELS || (channels % 2) != 0) {
        fprintf(stderr, "Unsupported channel count: %ld\n", channels);
        return 1;
      }
      app.channels = (uint16_t)channels;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (app.interface[0] == '\0' &&
      choose_interface(app.interface, sizeof(app.interface)) != 0) {
    return 1;
  }

  if (audio_ring_init(&app.ring, app.channels, REACJACK_PW_RING_FRAMES) != 0) {
    fprintf(stderr, "Failed to allocate audio ring\n");
    return 1;
  }

  app.socket_fd = open_capture_socket(app.interface);
  if (app.socket_fd < 0) {
    audio_ring_destroy(&app.ring);
    return 1;
  }

  pw_init(&argc, &argv);
  app.loop = pw_main_loop_new(NULL);
  if (app.loop == NULL) {
    fprintf(stderr, "Failed to create PipeWire main loop\n");
    close(app.socket_fd);
    audio_ring_destroy(&app.ring);
    return 1;
  }

  pw_loop_add_signal(pw_main_loop_get_loop(app.loop), SIGINT, do_quit, &app);
  pw_loop_add_signal(pw_main_loop_get_loop(app.loop), SIGTERM, do_quit, &app);

  if (start_pipewire_stream(&app) != 0) {
    pw_main_loop_destroy(app.loop);
    close(app.socket_fd);
    audio_ring_destroy(&app.ring);
    return 1;
  }

  if (pthread_create(&app.capture_thread, NULL, capture_thread_main, &app) != 0) {
    perror("pthread_create");
    pw_stream_destroy(app.stream);
    pw_main_loop_destroy(app.loop);
    close(app.socket_fd);
    audio_ring_destroy(&app.ring);
    return 1;
  }
  app.capture_thread_started = 1;

  fprintf(stderr, "Capturing REAC from %s into PipeWire source \"%s\" (%u channels)\n",
          app.interface, REACJACK_PW_NAME, app.channels);

  pw_main_loop_run(app.loop);
  app.running = 0;

  if (app.capture_thread_started) {
    pthread_join(app.capture_thread, NULL);
  }

  print_summary(&app);
  pw_stream_destroy(app.stream);
  pw_main_loop_destroy(app.loop);
  close(app.socket_fd);
  audio_ring_destroy(&app.ring);
  pw_deinit();
  return 0;
}
