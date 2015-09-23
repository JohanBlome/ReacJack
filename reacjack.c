#include <stdio.h>
#include <stdlib.h>
#include <jack/jack.h>
#include <jack/types.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/times.h>
#include <linux/if_ether.h>
#include <errno.h>
#include <jack/ringbuffer.h>
#include <sys/mman.h>
#include <linux/if_packet.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <math.h>
#include <linux/if_link.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netdb.h>
#include <net/if.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>

#include "reac.h"
#define NUMBER_OF_CHANNELS 40
#define PACKAGES_IN_BUFFER 24


long lostPackages = 0;
long bufferUnderrun = 0;

jack_client_t *client;
jack_port_t* outputPorts[NUMBER_OF_CHANNELS];
jack_ringbuffer_t *ringbuffer;
jack_default_audio_sample_t *outBuffer[NUMBER_OF_CHANNELS];

int isRunning = 1;
int socketFd = 0;
int dataSize = 0;
char *ring;
jack_nframes_t currentNumberOfChannels = 0;
uint iterations = 400;
uint iteration = 0;

int error = 0;
char active[NUMBER_OF_CHANNELS + 1];
int doMeasure = 1;

long nonREAC = 0;

const float Q = 1.0 / 0x7fffff;
FILE *dump;
FILE *packageDump;
FILE *ringbufferDump;

float channelRMS[NUMBER_OF_CHANNELS];
float channelPeak[NUMBER_OF_CHANNELS];

const int MAX_INT = 1<<23;

uint16_t getCounter(REACPacketHeader *header)
{
    uint16_t ret = header->counter[0];
    ret += ((uint16_t) header->counter[1]) << 8;
    return ret;
}

/// The number of frames in the ring
//  This number is not set in stone. Nor are block_size, block_nr or frame_size
#define CONF_RING_FRAMES          128

/// Offset of data from start of frame
#define PKT_OFFSET      (TPACKET_ALIGN(sizeof(struct tpacket_hdr)) + \
                         TPACKET_ALIGN(sizeof(struct sockaddr_ll)))

/// (unimportant) macro for loud failure
#define RETURN_ERROR(lvl, msg) \
  do {                    \
    fprintf(stderr, msg); \
    return lvl;            \
  } while(0);

static int rxring_offset;

/// Initialize a packet socket ring buffer
//  @param ringtype is one of PACKET_RX_RING or PACKET_TX_RING
static char *
init_packetsock_ring(int fd, int ringtype)
{
  struct tpacket_req tp;
  char *ring;

  // tell kernel to export data through mmap()ped ring
  tp.tp_block_size = CONF_RING_FRAMES * getpagesize();
  tp.tp_block_nr = 1;
  tp.tp_frame_size = getpagesize();
  tp.tp_frame_nr = CONF_RING_FRAMES;
  if (setsockopt(fd, SOL_PACKET, ringtype, (void*) &tp, sizeof(tp)))
    RETURN_ERROR(NULL, "setsockopt() ring\n");

#ifdef TPACKET_V2
  val = TPACKET_V1;
  setsockopt(fd, SOL_PACKET, PACKET_HDRLEN, &val, sizeof(val));
#endif

  // open ring
  ring = (char*)mmap(0, tp.tp_block_size * tp.tp_block_nr,
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (!ring)
    RETURN_ERROR(NULL, "mmap()\n");

  return ring;
}

/// Create a packet socket. If param ring is not NULL, the buffer is mapped
//  @param ring will, if set, point to the mapped ring on return
//  @return the socket fd
static int
init_packetsock(char **ring, int ringtype)
{
  int fd;

  // open packet socket
  fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0)
    RETURN_ERROR(-1, "Root priliveges are required\nsocket() rx. \n");

  if (ring) {
    *ring = init_packetsock_ring(fd, ringtype);

    if (!*ring) {
      close(fd);
      return -1;
    }
  }

  return fd;
}

static int
exit_packetsock(int fd, char *ring)
{
  if (munmap(ring, CONF_RING_FRAMES * getpagesize())) {
    perror("munmap");
    return 1;
  }

  if (close(fd)) {
    perror("close");
    return 1;
  }

  return 0;
}

/// Blocking read, returns a single packet (from packet ring)
static void *
process_rx(const int fd, char *rx_ring, int *size)
{
  struct tpacket_hdr *header;
  struct pollfd pollset;
  int ret;

  // fetch a frame
  header = (tpacket_hdr *) (rx_ring + (rxring_offset * getpagesize()));
  assert((((unsigned long) header) & (getpagesize() - 1)) == 0);

  // TP_STATUS_USER means that the process owns the packet.
  // When a slot does not have this flag set, the frame is not
  // ready for consumption.
  while (!(header->tp_status & TP_STATUS_USER)) {

    // if none available: wait on more data
    pollset.fd = fd;
    pollset.events = POLLIN;
    pollset.revents = 0;
    ret = poll(&pollset, 1, -1 /* negative means infinite */);
    if (ret < 0) {
        printf("Failure\n");
      if (errno != EINTR)
        RETURN_ERROR(NULL, "poll()\n");
      return NULL;
    }
  }

  // check data
  if (header->tp_status & TP_STATUS_COPY)
    RETURN_ERROR(NULL, "skipped: incomplete packed\n");

   *size = header->tp_len;
  return ((void *) header)+ PKT_OFFSET+2;
}

// Release the slot back to the kernel
static void
process_rx_release(char *rx_ring)
{
  struct tpacket_hdr *header;

  // clear status to grant to kernel
  header = (tpacket_hdr *) (rx_ring + (rxring_offset * getpagesize()));
  header->tp_status = 0;

  // update consumer pointer
  rxring_offset = (rxring_offset + 1) & (CONF_RING_FRAMES - 1);
}


static void shutdownReac(void *arg){
    printf("\n** jack has shut down **\n");
    printf("Lost packages: %ld\n", lostPackages);

    isRunning = 0;
}

// callback function handed to the JACK server:
static int process(jack_nframes_t nframes, void *arg){
    pthread_setname_np(pthread_self(), "translate");
    double measureRMS[iterations * NUMBER_OF_CHANNELS];
    double measurePEAK[iterations * NUMBER_OF_CHANNELS];
    //Read from the ringbuffer and deinterleave
    size_t bufferSize = nframes*40*3;
    char buffer[bufferSize];
    char* in;
    uint i,j;
    jack_default_audio_sample_t *ch1;
    jack_default_audio_sample_t *ch2;

    size_t available = jack_ringbuffer_read_space(ringbuffer);
    if(available >= bufferSize) {
      size_t read = jack_ringbuffer_read(ringbuffer, buffer, bufferSize);
      if(read != bufferSize) {
        error = -2;
      }

      for(i = 0; i < currentNumberOfChannels; i++) {
          outBuffer[i] = (jack_default_audio_sample_t *) jack_port_get_buffer(outputPorts[i], nframes);
      }

      in = buffer;
      int32_t val1, val2;
      double sqVal[NUMBER_OF_CHANNELS];
      double peak[NUMBER_OF_CHANNELS];

      char *data1 = (char*)&val1;
      char *data2 = (char*)&val2;
      
      for(i = 0; i < nframes; i++)
        {
          for(j = 0; j < currentNumberOfChannels; j+=2) //2 channels at the time...
          {
            ch1 = outBuffer[j];
            ch2 = outBuffer[j+1];

            data1[0] = *(in+3);
            data1[1] = *(in);
            data1[2] = *(in+1);
            data1[3] = 0;
            
            if(val1>>23)
            {
              val1=val1|0xff000000;
              ch1[i] = (float)val1/(float)MAX_INT24;
            }
            else
            {
              ch1[i] = (float)val1/(float)(MAX_INT24-1);
            }

            data2[0] = *(in+4);
            data2[1] = *(in+5);
            data2[2] = *(in+2);
            data2[3] = 0;

            if(val2>>23)
            {
              val2=val2|0xff000000;
              ch2[i] = (float)val2/(float)MAX_INT24;
            }
            else
            {
              ch2[i] = (float)val2/(float)(MAX_INT24-1);
            }
            sqVal[j] += ((double)*(ch1+i))*((double)*(ch1+i));
            sqVal[j+1] += ((double)*(ch2+i))*((double)*(ch2+i));
            
            if(peak[j] < fabs(*(ch1+i))) peak[j] = fabs(*(ch1+i));
            if(peak[j+1] < fabs(*(ch2+i))) peak[j + 1] = fabs(*(ch2+i));
            
            in+=6;
          }
          

        }
        if(doMeasure) {
          int offset = 0;
          for(uint ch = 0; ch < currentNumberOfChannels; ch++) {
            offset = ch*iterations + iteration;
            measureRMS[offset] = sqVal[ch];
            measurePEAK[offset] = peak[ch];
            sqVal[ch] = 0;
            peak[ch] = 0;
          }
          iteration = (iteration >= iterations)? 0: iteration+1;
          
          for(uint ch = 0; ch < currentNumberOfChannels; ch++) {
            double val = 0;
            double peakVal = 0;          
            for(int v = 0; v < iterations; v++) {
              offset = ch*iterations + v;
              val += measureRMS[offset];
              if(measurePEAK[offset] > peakVal) {
                peakVal = measurePEAK[offset]; 
              }
            }
            channelRMS[ch] =  sqrt(val/(float)(iterations * nframes));
            channelPeak[ch] = peakVal;
          }
        }
        
        
    } else {
      printf("Buffer underrun\r");
      for(i = 0; i < currentNumberOfChannels; i++) {
          memset((jack_default_audio_sample_t *) jack_port_get_buffer(outputPorts[i], nframes), 0, nframes * sizeof(jack_default_audio_sample_t));
      }     
    }
    return 0;
}

const int overHead = sizeof(EthernetHeader)+sizeof(REACPacketHeader);//Exclude the reac end marker

void *copyData(void *argument)
{
    struct timespec ts;
    int lastCounter = 0;
    unsigned long packetTime;
    char *buffer;
    pthread_setname_np(pthread_self(), "data grabber");
    while(isRunning)
    {
        int n;// = read(socketFd, buffer, 1500);
        buffer = (char*)process_rx(socketFd, ring, &n);

        EthernetHeader *ethernetHeader = (EthernetHeader *)buffer;
        REACPacketHeader *reacHeader = (REACPacketHeader *)(buffer+sizeof(EthernetHeader));
        //Check protocol
        if (ethernetHeader->type != *(uint16_t*)REAC_PROTOCOL) {    
          process_rx_release(ring);
          nonREAC++;
          continue;
        }

        //Copy the data

        if(dataSize == 0) {
            dataSize = n-overHead-2;
            currentNumberOfChannels = dataSize/12/3;
            
            printf("* Current number of channels is %i *\n", currentNumberOfChannels);
            printf("* Overhead: %i, dataSize = %i *", overHead, dataSize);
            memset(active, 0, NUMBER_OF_CHANNELS);
            active[NUMBER_OF_CHANNELS]= '\0';
        }
        else if(dataSize != n-overHead-2) {
            //ERROR datasize has changed
            process_rx_release(ring);
            continue;
        }

        size_t space = jack_ringbuffer_write_space(ringbuffer);
        if(space >= dataSize) {
          size_t written = jack_ringbuffer_write(ringbuffer, buffer+overHead, dataSize);
        } else {
          error = -1;
          bufferUnderrun++;
        }
        int c = getCounter(reacHeader);
        if(c!=lastCounter+1 && c !=0 && lastCounter != -1)
        {
          lostPackages += c-lastCounter;
        }
        
        lastCounter = c;
   
        process_rx_release(ring);
    }
    return 0;
}

void chooseInterfaces(char* name) {
  struct ifaddrs *ifaddr, *ifa;
  int family, s, n;
  char host[NI_MAXHOST];

  if(getifaddrs(&ifaddr) == -1) {
    //error
    return;
  } 

  for (ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
     if (ifa->ifa_addr == NULL)
         continue;

        family = ifa->ifa_addr->sa_family;

         /* Display interface name and family (including symbolic
            form of the latter for the common families) */

          if(family == AF_PACKET) {
         printf("%d - %-8s\n", n+1,
                ifa->ifa_name);
          }

  }


  printf("Enter the interface number:");
  int inum = -1;
  int result = scanf("%d", &inum);
  if(result == 0) {
    printf("No interface selected\n");
    freeifaddrs(ifaddr);
    exit(1);
  }
    if(inum < 1 || inum > n)
    {
        printf("\nInterface number out of range.\n");
        /* Free the device list */
      freeifaddrs(ifaddr);
      exit(1);
    }

  for (ifa = ifaddr, n = 0; ifa != NULL && n != inum-1; ifa = ifa->ifa_next, n++);
  
  memcpy(name, ifa->ifa_name, strlen(ifa->ifa_name));
  freeifaddrs(ifaddr);
}

int main(int argc, char* argv[])
{
    int c;

    for(c = 1; c < argc; c++)
    {
        printf("Check: %s\n", argv[c]);
        if(strcmp("-silent", argv[c]) == 0)
        {
            doMeasure = 0;
        }
    }

    char interface[32];
    chooseInterfaces(interface);
    
    socketFd= init_packetsock(&ring, PACKET_RX_RING);

    if(socketFd < 0)
    {
        printf("Socket failet, errno = %i\n", errno);
        exit(-1);
    }

    printf("fd = %i\n", socketFd);

    struct sockaddr_ll sock_address;
    memset(&sock_address, 0, sizeof(sock_address));
    sock_address.sll_family = PF_PACKET;
    sock_address.sll_protocol = htons(ETH_P_ALL);
    sock_address.sll_ifindex = if_nametoindex(interface);
    if (bind(socketFd, (struct sockaddr*) &sock_address, sizeof(sock_address)) < 0) {
      perror("bind failed\n");
      close(socketFd);
      return -4;
    }

    // initial Jack setup, get sample rate:
    client = jack_client_open("ReacJack", JackNoStartServer, 0);
    if (client == 0) {
      // opening client failed:
      printf("Failed to open Jack, bailing out\n");
      return 1;
    }
    jack_set_process_callback(client, process, 0);
    printf("Register shutdown callback");
    jack_on_shutdown(client, shutdownReac, 0);

    int i = 0;
    char* name = (char*)malloc(8*sizeof(char));

    for(; i < NUMBER_OF_CHANNELS; i++) {
        sprintf(name, "Reac %.2i", i+1);
        printf("Create port: %s\n", name);
        outputPorts[i] = jack_port_register(client,
                                  name,
                                  JACK_DEFAULT_AUDIO_TYPE,
                                  JackPortIsOutput,
                                  0
                                  );
    }

    unsigned int sampleRate = jack_get_sample_rate (client);

    ringbuffer = jack_ringbuffer_create (NUMBER_OF_CHANNELS * 3 * 12 * PACKAGES_IN_BUFFER);


    if (jack_activate(client)) {
      // activating client failed:
      printf("Failed to activate client\n");
      return 1;
    }

        //The capture thread...
    pthread_t captureThread;
    pthread_create(&captureThread, NULL/* &attr*/, copyData, NULL);



    sleep(2);

    jack_ringbuffer_reset(ringbuffer);
    lostPackages = 0; //Work has just started...
    bufferUnderrun = 0;
    error = 0;
    bool done = false;
    char values[8192];
    struct tm* tm_info;

    time_t timer;
    while(!done) {
      time(&timer);
      tm_info = localtime(&timer);
      int col1 = 0;
      int col2 = 0;
      strftime(values, 26, "%Y:%m:%d %H:%M:%S\n", tm_info);
      sprintf(values, "%s\n** REAC, error: %d **\nchannels: %d\nlost packages: %ld\nunderrun: %ld\nNon REAC: %ld\n", values, error, currentNumberOfChannels, lostPackages, bufferUnderrun, nonREAC);
      sprintf(values, "%s\nSpace: |%d|\n\Data: %d\n", values, jack_ringbuffer_write_space(ringbuffer), jack_ringbuffer_read_space(ringbuffer));
        if(doMeasure) {
        sprintf(values, "%s\nActive: |%s|\n\n\tRMS\tPEAK\tDYN\t(in db)\n", values, active);

       

          
          for(uint ch = 0; ch < currentNumberOfChannels; ch++) {
            
            channelRMS[ch] = 20 * log10(channelRMS[ch]);
            channelPeak[ch] = 20 * log10(channelPeak[ch]);
             if(channelRMS[ch] < -80) {
              active[ch] = '_';
            } else {
              active[ch] = '*';
            }
            if(active[ch] == '*') {
              if(channelRMS[ch] > -6) {
                col1 = 31;
              } else if(channelRMS[ch] > -12) {
                col1 = 33;
              } else if(channelRMS[ch] > -60) {
                col1 = 32;
              } else {
                col1 = 34;
              }
              if(channelPeak[ch] > -6) {
                col2 = 31;
              } else if(channelPeak[ch] > -12) {
                col2 = 33;
              } else if(channelPeak[ch] > -60) {
                col2 = 32;
              } else {
                col2 = 34;
              }
              sprintf(values, "%s%d\t\033[1;%dm%.1f\033[4;0m\t\033[1;%dm%.1f\033[4;0m\t%.1f\n", values, ch + 1, col1, channelRMS[ch], col2, channelPeak[ch], channelPeak[ch] - channelRMS[ch]);
            } else {
              sprintf(values, "%s%d\n", values, ch + 1);
            }
          //  printf("Ch %d\n", ch +1);
          }
        }
        
        
        printf("\033[2J%s", values);
        usleep(100000); //sleep 100ms
    }
    printf("Join capture thread\n");
    pthread_join(captureThread, NULL);

    fclose(dump);
    fclose(packageDump);
    printf("Exit");
    return 0;
}
