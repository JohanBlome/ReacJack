#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>

#if defined(__linux__)
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#elif defined(__APPLE__)
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <pcap/pcap.h>
#else
#error "ReacJack currently supports Linux and macOS capture backends"
#endif

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/types.h>

#include "reac_decode.h"

#define NUMBER_OF_CHANNELS REAC_MAX_CHANNELS
#define PACKAGES_IN_BUFFER 80
#define METER_ITERATIONS 400

#if defined(__linux__)
#define CONF_RING_FRAMES 128
#define PKT_OFFSET \
  (TPACKET_ALIGN(sizeof(struct tpacket_hdr)) + TPACKET_ALIGN(sizeof(struct sockaddr_ll)))
#endif

typedef struct {
  const unsigned char *data;
  int size;
  int needs_release;
} CaptureFrame;

typedef enum {
  ERR_OK = 0,
  ERR_CAPTURE = -1,
  ERR_RING_READ = -2,
  ERR_SHORT_PACKET = -3,
  ERR_BAD_PACKET_SIZE = -4,
  ERR_TOO_MANY_CHANNELS = -5,
  ERR_JACK_BUFFER_TOO_LARGE = -6,
  ERR_JACK_PORT = -7
} ReacError;

static volatile sig_atomic_t isRunning = 1;

static long lostPackages = 0;
static long netBufferUnderrun = 0;
static long jackBufferUnderrun = 0;
static long nonREAC = 0;
static long malformedPackets = 0;
static long captureErrors = 0;

static jack_client_t *client = NULL;
static jack_port_t *outputPorts[NUMBER_OF_CHANNELS];
static jack_ringbuffer_t *ringbuffer = NULL;
static jack_default_audio_sample_t *outBuffer[NUMBER_OF_CHANNELS];

static int dataSize = 0;
static unsigned char *processBuffer = NULL;
static size_t processBufferBytes = 0;
static jack_nframes_t currentNumberOfChannels = 0;
static unsigned int sampleRate = 48000;
static unsigned int currentSamplesPerPeriod = 0;

static int error = ERR_OK;
static char active[NUMBER_OF_CHANNELS + 1];
static int doMeasure = 1;
static float lastTS = 0;
static float lastJackTime = 0;
static long receivedPackages = 0;
static jack_time_t processStartTime = 0;
static uint16_t lastCounter = 0;
static int haveLastCounter = 0;
static float packetTime = 1000.0f * REAC_FRAMES_PER_PACKET / 48000.0f;
static size_t spaceAmount = 0;
static size_t dataAmount = 0;

static float channelRMS[NUMBER_OF_CHANNELS];
static float channelPeak[NUMBER_OF_CHANNELS];
static double meterRMS[NUMBER_OF_CHANNELS * METER_ITERATIONS];
static double meterPeak[NUMBER_OF_CHANNELS * METER_ITERATIONS];
static unsigned int meterIteration = 0;

#if defined(__linux__)
static int socketFd = -1;
static char *ring = NULL;
static int rxring_offset = 0;
#elif defined(__APPLE__)
static pcap_t *pcapHandle = NULL;
#endif

static void set_thread_name(const char *name)
{
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

static void request_shutdown(int signum)
{
  (void)signum;
  isRunning = 0;
}

static void silence_outputs(jack_nframes_t nframes, jack_nframes_t channels)
{
  jack_nframes_t limit = channels;
  if (limit == 0 || limit > NUMBER_OF_CHANNELS) {
    limit = NUMBER_OF_CHANNELS;
  }

  for (jack_nframes_t i = 0; i < limit; i++) {
    if (outputPorts[i] == NULL) {
      continue;
    }

    jack_default_audio_sample_t *out =
        (jack_default_audio_sample_t *)jack_port_get_buffer(outputPorts[i], nframes);
    if (out != NULL) {
      memset(out, 0, nframes * sizeof(jack_default_audio_sample_t));
    }
  }
}

static float sample_to_db(float value)
{
  if (value <= 0.0000001f || !isfinite(value)) {
    return -120.0f;
  }

  return 20.0f * log10f(value);
}

static void note_packet_counter(uint16_t counter)
{
  if (haveLastCounter) {
    uint16_t expected = (uint16_t)(lastCounter + 1);
    if (counter != expected) {
      uint16_t gap = (uint16_t)(counter - expected);
      if (gap < 32768) {
        lostPackages += gap;
      } else {
        lostPackages++;
      }
    }
  }

  lastCounter = counter;
  haveLastCounter = 1;
}

static int write_payload_to_ring(const unsigned char *payload, size_t bytes)
{
  if (bytes == 0) {
    return -1;
  }

  if (jack_ringbuffer_write_space(ringbuffer) < bytes) {
    netBufferUnderrun++;
    return -1;
  }

  size_t written = jack_ringbuffer_write(ringbuffer, (const char *)payload, bytes);
  return written == bytes ? 0 : -1;
}

#if defined(__linux__)
static char *init_packetsock_ring(int fd, int ringtype)
{
  struct tpacket_req tp;
  memset(&tp, 0, sizeof(tp));

  tp.tp_block_size = CONF_RING_FRAMES * getpagesize();
  tp.tp_block_nr = 1;
  tp.tp_frame_size = getpagesize();
  tp.tp_frame_nr = CONF_RING_FRAMES;

  if (setsockopt(fd, SOL_PACKET, ringtype, (void *)&tp, sizeof(tp)) != 0) {
    perror("setsockopt PACKET_RX_RING");
    return NULL;
  }

  int val = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(val)) != 0) {
    perror("setsockopt SO_TIMESTAMPING");
  }

  char *mapped = (char *)mmap(0, tp.tp_block_size * tp.tp_block_nr,
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }

  return mapped;
}

static int init_packetsock(char **mappedRing, int ringtype)
{
  int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) {
    perror("socket PF_PACKET. Root privileges or cap_net_raw are required");
    return -1;
  }

  if (mappedRing != NULL) {
    *mappedRing = init_packetsock_ring(fd, ringtype);
    if (*mappedRing == NULL) {
      close(fd);
      return -1;
    }
  }

  return fd;
}

static int exit_packetsock(int fd, char *mappedRing)
{
  int result = 0;

  if (mappedRing != NULL && munmap(mappedRing, CONF_RING_FRAMES * getpagesize()) != 0) {
    perror("munmap");
    result = 1;
  }

  if (fd >= 0 && close(fd) != 0) {
    perror("close");
    result = 1;
  }

  return result;
}

static void *process_rx(const int fd, char *rx_ring, int *size)
{
  struct tpacket_hdr *header;
  struct pollfd pollset;

  header = (struct tpacket_hdr *)(rx_ring + (rxring_offset * getpagesize()));

  while (isRunning && !(header->tp_status & TP_STATUS_USER)) {
    pollset.fd = fd;
    pollset.events = POLLIN;
    pollset.revents = 0;

    int ret = poll(&pollset, 1, 1000);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      captureErrors++;
      return NULL;
    }
  }

  if (!isRunning) {
    return NULL;
  }

  if (header->tp_status & TP_STATUS_COPY) {
    malformedPackets++;
    header->tp_status = 0;
    rxring_offset = (rxring_offset + 1) & (CONF_RING_FRAMES - 1);
    return NULL;
  }

  *size = (int)header->tp_len;
  return ((unsigned char *)header) + PKT_OFFSET + 2;
}

static void process_rx_release(char *rx_ring)
{
  struct tpacket_hdr *header;

  header = (struct tpacket_hdr *)(rx_ring + (rxring_offset * getpagesize()));
  header->tp_status = 0;
  rxring_offset = (rxring_offset + 1) & (CONF_RING_FRAMES - 1);
}

static int capture_open(const char *interface)
{
  socketFd = init_packetsock(&ring, PACKET_RX_RING);
  if (socketFd < 0) {
    return -1;
  }

  struct sockaddr_ll sock_address;
  memset(&sock_address, 0, sizeof(sock_address));
  sock_address.sll_family = PF_PACKET;
  sock_address.sll_protocol = htons(ETH_P_ALL);
  sock_address.sll_ifindex = (int)if_nametoindex(interface);
  if (sock_address.sll_ifindex == 0) {
    fprintf(stderr, "Unknown interface: %s\n", interface);
    return -1;
  }

  if (bind(socketFd, (struct sockaddr *)&sock_address, sizeof(sock_address)) < 0) {
    perror("bind");
    return -1;
  }

  return 0;
}

static int capture_next(CaptureFrame *frame)
{
  int packetSize = 0;
  void *packet = process_rx(socketFd, ring, &packetSize);
  if (packet == NULL) {
    return isRunning ? -1 : 1;
  }

  frame->data = (const unsigned char *)packet;
  frame->size = packetSize;
  frame->needs_release = 1;
  return 0;
}

static void capture_release(const CaptureFrame *frame)
{
  if (frame->needs_release) {
    process_rx_release(ring);
  }
}

static void capture_close(void)
{
  exit_packetsock(socketFd, ring);
  socketFd = -1;
  ring = NULL;
}
#elif defined(__APPLE__)
static int capture_open(const char *interface)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  errbuf[0] = '\0';

  pcapHandle = pcap_create(interface, errbuf);
  if (pcapHandle == NULL) {
    fprintf(stderr, "pcap_create(%s): %s\n", interface, errbuf);
    return -1;
  }

  pcap_set_snaplen(pcapHandle, 2048);
  pcap_set_promisc(pcapHandle, 1);
  pcap_set_timeout(pcapHandle, 100);
#ifdef BIOCIMMEDIATE
  pcap_set_immediate_mode(pcapHandle, 1);
#endif

  int rc = pcap_activate(pcapHandle);
  if (rc < 0) {
    fprintf(stderr, "pcap_activate(%s): %s\n", interface, pcap_geterr(pcapHandle));
    pcap_close(pcapHandle);
    pcapHandle = NULL;
    return -1;
  }
  if (pcap_datalink(pcapHandle) != DLT_EN10MB) {
    fprintf(stderr, "pcap interface %s is not exposing Ethernet frames\n", interface);
    pcap_close(pcapHandle);
    pcapHandle = NULL;
    return -1;
  }

  struct bpf_program filter;
  if (pcap_compile(pcapHandle, &filter, "ether proto 0x8819", 1, PCAP_NETMASK_UNKNOWN) == 0) {
    if (pcap_setfilter(pcapHandle, &filter) != 0) {
      fprintf(stderr, "pcap_setfilter: %s\n", pcap_geterr(pcapHandle));
    }
    pcap_freecode(&filter);
  } else {
    fprintf(stderr, "pcap_compile: %s\n", pcap_geterr(pcapHandle));
  }

  return 0;
}

static int capture_next(CaptureFrame *frame)
{
  while (isRunning) {
    struct pcap_pkthdr *header = NULL;
    const unsigned char *packet = NULL;
    int rc = pcap_next_ex(pcapHandle, &header, &packet);

    if (rc == 1) {
      frame->data = packet;
      frame->size = (int)header->caplen;
      frame->needs_release = 0;
      return 0;
    }
    if (rc == 0) {
      continue;
    }
    if (rc == -2) {
      return 1;
    }

    fprintf(stderr, "pcap_next_ex: %s\n", pcap_geterr(pcapHandle));
    captureErrors++;
    return -1;
  }

  return 1;
}

static void capture_release(const CaptureFrame *frame)
{
  (void)frame;
}

static void capture_close(void)
{
  if (pcapHandle != NULL) {
    pcap_close(pcapHandle);
    pcapHandle = NULL;
  }
}
#endif

static int is_link_layer_family(int family)
{
#if defined(__linux__)
  return family == AF_PACKET;
#elif defined(__APPLE__)
  return family == AF_LINK;
#else
  (void)family;
  return 0;
#endif
}

static int chooseInterfaces(char *name, size_t nameSize)
{
  struct ifaddrs *ifaddr;

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return -1;
  }

  int shown = 0;
  int selectedIndex = -1;
  for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || !is_link_layer_family(ifa->ifa_addr->sa_family)) {
      continue;
    }

    shown++;
    printf("%d - %-8s\n", shown, ifa->ifa_name);
  }

  if (shown == 0) {
    fprintf(stderr, "No capture interfaces found\n");
    freeifaddrs(ifaddr);
    return -1;
  }

  printf("Enter the interface number: ");
  int result = scanf("%d", &selectedIndex);
  if (result != 1 || selectedIndex < 1 || selectedIndex > shown) {
    fprintf(stderr, "No valid interface selected\n");
    freeifaddrs(ifaddr);
    return -1;
  }

  int current = 0;
  for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || !is_link_layer_family(ifa->ifa_addr->sa_family)) {
      continue;
    }

    current++;
    if (current == selectedIndex) {
      snprintf(name, nameSize, "%s", ifa->ifa_name);
      freeifaddrs(ifaddr);
      return 0;
    }
  }

  freeifaddrs(ifaddr);
  return -1;
}

static void shutdownReac(void *arg)
{
  (void)arg;
  printf("\n** jack has shut down **\n");
  printf("Lost packages: %ld\n", lostPackages);
  isRunning = 0;
}

static void *copyData(void *argument)
{
  (void)argument;
  set_thread_name("data grabber");

  while (isRunning) {
    CaptureFrame frame;
    memset(&frame, 0, sizeof(frame));

    int captureResult = capture_next(&frame);
    if (captureResult > 0) {
      break;
    }
    if (captureResult < 0) {
      error = ERR_CAPTURE;
      usleep(1000);
      continue;
    }

    ReacPacketView packet;
    ReacStatus parseStatus =
        reac_parse_packet(frame.data, frame.size < 0 ? 0 : (size_t)frame.size, &packet);
    if (parseStatus == REAC_STATUS_NOT_REAC) {
      nonREAC++;
      capture_release(&frame);
      continue;
    }
    if (parseStatus != REAC_STATUS_OK) {
      malformedPackets++;
      error = parseStatus == REAC_STATUS_SHORT_PACKET ? ERR_SHORT_PACKET : ERR_BAD_PACKET_SIZE;
      capture_release(&frame);
      continue;
    }

    int payloadSize = (int)packet.payload_size;
    jack_nframes_t channelCount = (jack_nframes_t)packet.channels;
    if (dataSize == 0) {
      dataSize = payloadSize;
      currentNumberOfChannels = channelCount;
      printf("* Current number of channels is %u *\n", (unsigned int)currentNumberOfChannels);
      printf("* Overhead: %zu, dataSize = %d *\n", reac_packet_overhead(), dataSize);
      memset(active, '_', NUMBER_OF_CHANNELS);
      active[NUMBER_OF_CHANNELS] = '\0';
    } else if (dataSize != payloadSize) {
      malformedPackets++;
      error = ERR_BAD_PACKET_SIZE;
      capture_release(&frame);
      continue;
    }

    receivedPackages++;
    note_packet_counter(packet.counter);

    if (write_payload_to_ring(packet.payload, (size_t)dataSize) != 0) {
      error = ERR_RING_READ;
    }

    capture_release(&frame);
  }

  return NULL;
}

static int process(jack_nframes_t nframes, void *arg)
{
  (void)arg;
  static int processThreadNamed = 0;
  if (!processThreadNamed) {
    set_thread_name("translate");
    processThreadNamed = 1;
  }

  currentSamplesPerPeriod = nframes;
  dataAmount = jack_ringbuffer_read_space(ringbuffer);
  spaceAmount = jack_ringbuffer_write_space(ringbuffer);

  if (currentNumberOfChannels == 0 || dataSize == 0) {
    silence_outputs(nframes, NUMBER_OF_CHANNELS);
    return 0;
  }

  size_t bufferSize = (size_t)nframes * currentNumberOfChannels * REAC_BYTES_PER_SAMPLE;
  if (bufferSize > processBufferBytes) {
    error = ERR_JACK_BUFFER_TOO_LARGE;
    jackBufferUnderrun++;
    silence_outputs(nframes, currentNumberOfChannels);
    return 0;
  }

  if (processStartTime == 0) {
    processStartTime = jack_last_frame_time(client);
  } else {
    lastJackTime =
        1000.0f * (jack_last_frame_time(client) - processStartTime) / (float)sampleRate;
    lastTS = receivedPackages * packetTime;
  }

  if (dataAmount < bufferSize) {
    jackBufferUnderrun++;
    processStartTime = jack_last_frame_time(client);
    receivedPackages = (long)(dataAmount / (REAC_BYTES_PER_SAMPLE * currentNumberOfChannels));
    silence_outputs(nframes, currentNumberOfChannels);
    return 0;
  }

  size_t read = jack_ringbuffer_read(ringbuffer, (char *)processBuffer, bufferSize);
  if (read != bufferSize) {
    error = ERR_RING_READ;
    silence_outputs(nframes, currentNumberOfChannels);
    return 0;
  }

  for (jack_nframes_t i = 0; i < currentNumberOfChannels; i++) {
    outBuffer[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(outputPorts[i], nframes);
    if (outBuffer[i] == NULL) {
      error = ERR_JACK_PORT;
      silence_outputs(nframes, currentNumberOfChannels);
      return 0;
    }
  }

  float *decodeOutputs[NUMBER_OF_CHANNELS];
  for (jack_nframes_t i = 0; i < currentNumberOfChannels; i++) {
    decodeOutputs[i] = (float *)outBuffer[i];
  }

  ReacStatus decodeStatus =
      reac_decode_samples_to_channels(processBuffer, read, (uint16_t)currentNumberOfChannels,
                                      nframes, decodeOutputs);
  if (decodeStatus != REAC_STATUS_OK) {
    error = ERR_BAD_PACKET_SIZE;
    silence_outputs(nframes, currentNumberOfChannels);
    return 0;
  }

  double sqVal[NUMBER_OF_CHANNELS];
  double peak[NUMBER_OF_CHANNELS];
  memset(sqVal, 0, sizeof(sqVal));
  memset(peak, 0, sizeof(peak));

  if (doMeasure) {
    for (jack_nframes_t frame = 0; frame < nframes; frame++) {
      for (jack_nframes_t ch = 0; ch < currentNumberOfChannels; ch++) {
        double sample = outBuffer[ch][frame];
        sqVal[ch] += sample * sample;
        if (peak[ch] < fabs(sample)) {
          peak[ch] = fabs(sample);
        }
      }
    }
  }

  if (doMeasure) {
    for (jack_nframes_t ch = 0; ch < currentNumberOfChannels; ch++) {
      int offset = (int)(ch * METER_ITERATIONS + meterIteration);
      meterRMS[offset] = sqVal[ch];
      meterPeak[offset] = peak[ch];
    }

    meterIteration = (meterIteration + 1) % METER_ITERATIONS;

    for (jack_nframes_t ch = 0; ch < currentNumberOfChannels; ch++) {
      double rms = 0;
      double peakVal = 0;
      for (int v = 0; v < METER_ITERATIONS; v++) {
        int offset = (int)(ch * METER_ITERATIONS + v);
        rms += meterRMS[offset];
        if (meterPeak[offset] > peakVal) {
          peakVal = meterPeak[offset];
        }
      }

      channelRMS[ch] = (float)sqrt(rms / (double)(METER_ITERATIONS * nframes));
      channelPeak[ch] = (float)peakVal;
    }
  }

  error = ERR_OK;
  return 0;
}

int main(int argc, char *argv[])
{
  for (int c = 1; c < argc; c++) {
    if (strcmp("-silent", argv[c]) == 0) {
      doMeasure = 0;
    }
  }

  signal(SIGINT, request_shutdown);
  signal(SIGTERM, request_shutdown);

  char interface[IF_NAMESIZE];
  if (chooseInterfaces(interface, sizeof(interface)) != 0) {
    return 1;
  }

  if (capture_open(interface) != 0) {
    capture_close();
    return 1;
  }

  printf("Open jack\n");
  client = jack_client_open("ReacJack", JackNoStartServer, 0);
  if (client == NULL) {
    fprintf(stderr, "Failed to open JACK. Is the JACK server running?\n");
    capture_close();
    return 1;
  }

  jack_set_process_callback(client, process, 0);
  jack_on_shutdown(client, shutdownReac, 0);

  char portName[32];
  int portsOk = 1;
  for (int i = 0; i < NUMBER_OF_CHANNELS; i++) {
    snprintf(portName, sizeof(portName), "Reac %.2i", i + 1);
    outputPorts[i] =
        jack_port_register(client, portName, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (outputPorts[i] == NULL) {
      fprintf(stderr, "Failed to create JACK port: %s\n", portName);
      portsOk = 0;
    }
  }
  if (!portsOk) {
    jack_client_close(client);
    capture_close();
    return 1;
  }

  sampleRate = jack_get_sample_rate(client);
  packetTime = 1000.0f * REAC_FRAMES_PER_PACKET / (float)sampleRate;

  jack_nframes_t jackBufferFrames = jack_get_buffer_size(client);
  processBufferBytes =
      (size_t)jackBufferFrames * NUMBER_OF_CHANNELS * REAC_BYTES_PER_SAMPLE;
  processBuffer = (unsigned char *)malloc(processBufferBytes);
  if (processBuffer == NULL) {
    fprintf(stderr, "Failed to allocate JACK process buffer\n");
    jack_client_close(client);
    capture_close();
    return 1;
  }

  ringbuffer = jack_ringbuffer_create(NUMBER_OF_CHANNELS * REAC_BYTES_PER_SAMPLE *
                                      REAC_FRAMES_PER_PACKET * PACKAGES_IN_BUFFER);
  if (ringbuffer == NULL) {
    fprintf(stderr, "Failed to allocate JACK ringbuffer\n");
    free(processBuffer);
    jack_client_close(client);
    capture_close();
    return 1;
  }

  if (jack_activate(client)) {
    fprintf(stderr, "Failed to activate JACK client\n");
    jack_ringbuffer_free(ringbuffer);
    free(processBuffer);
    jack_client_close(client);
    capture_close();
    return 1;
  }

  pthread_t captureThread;
  if (pthread_create(&captureThread, NULL, copyData, NULL) != 0) {
    perror("pthread_create");
    isRunning = 0;
  }

  sleep(2);
  jack_ringbuffer_reset(ringbuffer);
  lostPackages = 0;
  netBufferUnderrun = 0;
  jackBufferUnderrun = 0;
  malformedPackets = 0;
  captureErrors = 0;
  receivedPackages = 0;
  error = ERR_OK;
  processStartTime = 0;
  haveLastCounter = 0;

  char values[8192];
  while (isRunning) {
    time_t timer = time(NULL);
    struct tm *tm_info = localtime(&timer);
    int offset = 0;

    if (tm_info != NULL) {
      offset += strftime(values, sizeof(values), "%Y:%m:%d %H:%M:%S\n", tm_info);
    }

    offset += snprintf(values + offset, sizeof(values) - (size_t)offset,
                       "\n** REAC, error: %d **\n"
                       "channels: %u, s.p.f. = %u\n"
                       "lost packages: %ld\n"
                       "underrun, net: %ld, jack: %ld\n"
                       "malformed: %ld, capture errors: %ld\n"
                       "Non REAC: %ld\n"
                       "Last ts: %.3f ms\n"
                       "Jack time: %.3f ms -> diff = %.3f ms\n"
                       "\nSpace: |%zu|\nData: %zu\n",
                       error, (unsigned int)currentNumberOfChannels, currentSamplesPerPeriod,
                       lostPackages, netBufferUnderrun, jackBufferUnderrun, malformedPackets,
                       captureErrors, nonREAC, lastTS, lastJackTime, lastTS - lastJackTime,
                       spaceAmount, dataAmount);

    if (doMeasure && offset < (int)sizeof(values)) {
      offset += snprintf(values + offset, sizeof(values) - (size_t)offset,
                         "\nActive: |%s|\n\n\tRMS\tPEAK\tDYN\t(in db)\n", active);

      for (jack_nframes_t ch = 0; ch < currentNumberOfChannels && offset < (int)sizeof(values);
           ch++) {
        float rmsDb = sample_to_db(channelRMS[ch]);
        float peakDb = sample_to_db(channelPeak[ch]);
        active[ch] = rmsDb < -80.0f ? '_' : '*';

        if (active[ch] == '*') {
          int col1 = rmsDb > -6.0f ? 31 : (rmsDb > -12.0f ? 33 : (rmsDb > -60.0f ? 32 : 34));
          int col2 = peakDb > -6.0f ? 31 : (peakDb > -12.0f ? 33 : (peakDb > -60.0f ? 32 : 34));
          offset += snprintf(values + offset, sizeof(values) - (size_t)offset,
                             "%u\t\033[1;%dm%.1f\033[4;0m\t"
                             "\033[1;%dm%.1f\033[4;0m\t%.1f\n",
                             (unsigned int)ch + 1, col1, rmsDb, col2, peakDb,
                             peakDb - rmsDb);
        } else {
          offset += snprintf(values + offset, sizeof(values) - (size_t)offset, "%u\n",
                             (unsigned int)ch + 1);
        }
      }
    }

    printf("\033[2J%s", values);
    usleep(100000);
  }

  printf("Join capture thread\n");
  pthread_join(captureThread, NULL);

  jack_deactivate(client);
  jack_ringbuffer_free(ringbuffer);
  jack_client_close(client);
  capture_close();
  free(processBuffer);

  printf("Exit\n");
  return 0;
}
