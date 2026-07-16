#if !defined(__APPLE__)
#error "test_hal_driver exercises the macOS CoreAudio HAL plug-in"
#endif

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include "../src/shared_audio.h"

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__,   \
              #expr);                                                           \
      exit(1);                                                                  \
    }                                                                           \
  } while (0)

#define DRIVER_BINARY "ReacJack.driver/Contents/MacOS/ReacJack"
#define DRIVER_RING_NAME "/reacjack-audio"
#define EXPECTED_CHANNELS 40
#define EXPECTED_SAMPLE_RATE 48000.0
#define TEST_FRAMES 512
#define RING_TEST_CHANNELS 2

static float ring_sample(uint16_t channel, uint32_t frame)
{
  return (channel == 0 ? 1.0f : -1.0f) * (float)(frame + 1) / 1024.0f;
}

typedef void *(*DriverFactory)(CFAllocatorRef allocator, CFUUIDRef typeUUID);

int main(void)
{
  void *bundle = dlopen(DRIVER_BINARY, RTLD_NOW | RTLD_LOCAL);
  if (bundle == NULL) {
    fprintf(stderr, "dlopen(%s): %s\n", DRIVER_BINARY, dlerror());
    return 1;
  }

  DriverFactory factory = (DriverFactory)dlsym(bundle, "ReacJack_Create");
  ASSERT_TRUE(factory != NULL);

  AudioServerPlugInDriverRef driver =
      (AudioServerPlugInDriverRef)factory(NULL, kAudioServerPlugInTypeUUID);
  ASSERT_TRUE(driver != NULL);

  /* coreaudiod resolves the driver interface through COM QueryInterface. */
  void *interface = NULL;
  ASSERT_TRUE((*driver)->QueryInterface(
                  driver, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),
                  &interface) == 0);
  ASSERT_TRUE(interface != NULL);

  /* The driver only stores the host ref, so a zeroed dummy host is enough. */
  AudioServerPlugInHostInterface host;
  memset(&host, 0, sizeof(host));
  ASSERT_TRUE((*driver)->Initialize(driver, &host) == 0);

  /* The plug-in must publish exactly one device. */
  AudioObjectPropertyAddress address = {kAudioPlugInPropertyDeviceList,
                                        kAudioObjectPropertyScopeGlobal, 0};
  UInt32 size = 0;
  ASSERT_TRUE((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0,
                                             &address, 0, NULL, &size) == 0);
  ASSERT_TRUE(size == sizeof(AudioObjectID));

  AudioObjectID device = kAudioObjectUnknown;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &address,
                                         0, NULL, sizeof(device), &size, &device) == 0);
  ASSERT_TRUE(device != kAudioObjectUnknown);

  address.mSelector = kAudioObjectPropertyName;
  CFStringRef name = NULL;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(name), &size, &name) == 0);
  ASSERT_TRUE(name != NULL);
  ASSERT_TRUE(CFStringCompare(name, CFSTR("ReacJack REAC"), 0) == kCFCompareEqualTo);
  CFRelease(name);

  address.mSelector = kAudioDevicePropertyDeviceUID;
  CFStringRef uid = NULL;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(uid), &size, &uid) == 0);
  ASSERT_TRUE(uid != NULL);
  CFRelease(uid);

  /* Input-only: one input stream, no output streams. */
  address.mSelector = kAudioDevicePropertyStreams;
  address.mScope = kAudioObjectPropertyScopeInput;
  ASSERT_TRUE((*driver)->GetPropertyDataSize(driver, device, 0, &address, 0, NULL,
                                             &size) == 0);
  ASSERT_TRUE(size == sizeof(AudioObjectID));

  AudioObjectID stream = kAudioObjectUnknown;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(stream), &size, &stream) == 0);
  ASSERT_TRUE(stream != kAudioObjectUnknown);

  address.mScope = kAudioObjectPropertyScopeOutput;
  ASSERT_TRUE((*driver)->GetPropertyDataSize(driver, device, 0, &address, 0, NULL,
                                             &size) == 0);
  ASSERT_TRUE(size == 0);
  address.mScope = kAudioObjectPropertyScopeGlobal;

  address.mSelector = kAudioStreamPropertyDirection;
  UInt32 direction = 0;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, stream, 0, &address, 0, NULL,
                                         sizeof(direction), &size, &direction) == 0);
  ASSERT_TRUE(direction == 1); /* 1 = input */

  address.mSelector = kAudioStreamPropertyVirtualFormat;
  AudioStreamBasicDescription format;
  memset(&format, 0, sizeof(format));
  ASSERT_TRUE((*driver)->GetPropertyData(driver, stream, 0, &address, 0, NULL,
                                         sizeof(format), &size, &format) == 0);
  ASSERT_TRUE(format.mSampleRate == EXPECTED_SAMPLE_RATE);
  ASSERT_TRUE(format.mFormatID == kAudioFormatLinearPCM);
  ASSERT_TRUE((format.mFormatFlags & kAudioFormatFlagIsFloat) != 0);
  ASSERT_TRUE(format.mChannelsPerFrame == EXPECTED_CHANNELS);
  ASSERT_TRUE(format.mBitsPerChannel == 32);
  ASSERT_TRUE(format.mBytesPerFrame == EXPECTED_CHANNELS * sizeof(float));

  address.mSelector = kAudioDevicePropertyNominalSampleRate;
  Float64 rate = 0.0;
  ASSERT_TRUE((*driver)->GetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(rate), &size, &rate) == 0);
  ASSERT_TRUE(rate == EXPECTED_SAMPLE_RATE);

  /* Only 48 kHz may be accepted. */
  ASSERT_TRUE((*driver)->SetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(rate), &rate) == 0);
  Float64 bad_rate = 44100.0;
  ASSERT_TRUE((*driver)->SetPropertyData(driver, device, 0, &address, 0, NULL,
                                         sizeof(bad_rate), &bad_rate) != 0);

  /* IO: start, get monotonic zero timestamps, read silence, stop.
   * No ring exists yet, so the device must record silence. */
  shared_audio_unlink(DRIVER_RING_NAME);
  ASSERT_TRUE((*driver)->StartIO(driver, device, 0) == 0);

  Float64 sample_time = -1.0;
  UInt64 host_time = 0;
  UInt64 seed = 0;
  ASSERT_TRUE((*driver)->GetZeroTimeStamp(driver, device, 0, &sample_time, &host_time,
                                          &seed) == 0);
  ASSERT_TRUE(sample_time >= 0.0);
  ASSERT_TRUE(seed != 0);

  Float64 later_sample_time = -1.0;
  UInt64 later_host_time = 0;
  usleep(300000);
  ASSERT_TRUE((*driver)->GetZeroTimeStamp(driver, device, 0, &later_sample_time,
                                          &later_host_time, &seed) == 0);
  ASSERT_TRUE(later_sample_time > sample_time);
  ASSERT_TRUE(later_host_time > host_time);

  Boolean will_do = false;
  Boolean in_place = false;
  ASSERT_TRUE((*driver)->WillDoIOOperation(driver, device, 0,
                                           kAudioServerPlugInIOOperationReadInput,
                                           &will_do, &in_place) == 0);
  ASSERT_TRUE(will_do);

  static float buffer[TEST_FRAMES * EXPECTED_CHANNELS];
  for (size_t i = 0; i < TEST_FRAMES * EXPECTED_CHANNELS; i++) {
    buffer[i] = 1.0f;
  }

  AudioServerPlugInIOCycleInfo cycle;
  memset(&cycle, 0, sizeof(cycle));
  ASSERT_TRUE((*driver)->BeginIOOperation(driver, device, 0,
                                          kAudioServerPlugInIOOperationReadInput,
                                          TEST_FRAMES, &cycle) == 0);
  ASSERT_TRUE((*driver)->DoIOOperation(driver, device, stream, 0,
                                       kAudioServerPlugInIOOperationReadInput,
                                       TEST_FRAMES, &cycle, buffer, NULL) == 0);
  ASSERT_TRUE((*driver)->EndIOOperation(driver, device, 0,
                                        kAudioServerPlugInIOOperationReadInput,
                                        TEST_FRAMES, &cycle) == 0);

  for (size_t i = 0; i < TEST_FRAMES * EXPECTED_CHANNELS; i++) {
    ASSERT_TRUE(buffer[i] == 0.0f);
  }

  ASSERT_TRUE((*driver)->StopIO(driver, device, 0) == 0);

  /* Milestone 6: when a reacjackd ring exists, ReadInput must deliver its
   * audio in the first ring channels and silence in the rest. */
  SharedAudio feeder;
  ASSERT_TRUE(shared_audio_create(&feeder, DRIVER_RING_NAME, 48000,
                                  RING_TEST_CHANNELS, 48000) == 0);

  static float feed_ch1[TEST_FRAMES], feed_ch2[TEST_FRAMES];
  float *feed_channels[RING_TEST_CHANNELS] = {feed_ch1, feed_ch2};
  for (uint32_t i = 0; i < TEST_FRAMES; i++) {
    feed_ch1[i] = ring_sample(0, i);
    feed_ch2[i] = ring_sample(1, i);
  }
  ASSERT_TRUE(shared_audio_write(&feeder, (const float *const *)feed_channels,
                                 TEST_FRAMES) == 0);

  ASSERT_TRUE((*driver)->StartIO(driver, device, 0) == 0);

  for (size_t i = 0; i < TEST_FRAMES * EXPECTED_CHANNELS; i++) {
    buffer[i] = 1.0f;
  }
  ASSERT_TRUE((*driver)->DoIOOperation(driver, device, stream, 0,
                                       kAudioServerPlugInIOOperationReadInput,
                                       TEST_FRAMES, &cycle, buffer, NULL) == 0);

  for (uint32_t frame = 0; frame < TEST_FRAMES; frame++) {
    ASSERT_TRUE(buffer[frame * EXPECTED_CHANNELS + 0] == ring_sample(0, frame));
    ASSERT_TRUE(buffer[frame * EXPECTED_CHANNELS + 1] == ring_sample(1, frame));
    for (uint32_t ch = RING_TEST_CHANNELS; ch < EXPECTED_CHANNELS; ch++) {
      ASSERT_TRUE(buffer[frame * EXPECTED_CHANNELS + ch] == 0.0f);
    }
  }

  /* A drained ring underruns to silence instead of failing. */
  ASSERT_TRUE((*driver)->DoIOOperation(driver, device, stream, 0,
                                       kAudioServerPlugInIOOperationReadInput,
                                       TEST_FRAMES, &cycle, buffer, NULL) == 0);
  for (size_t i = 0; i < TEST_FRAMES * EXPECTED_CHANNELS; i++) {
    ASSERT_TRUE(buffer[i] == 0.0f);
  }
  ASSERT_TRUE(feeder.header->underruns > 0);

  ASSERT_TRUE((*driver)->StopIO(driver, device, 0) == 0);
  shared_audio_close(&feeder);
  shared_audio_unlink(DRIVER_RING_NAME);

  puts("hal driver tests passed");
  return 0;
}
