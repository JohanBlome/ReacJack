#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shared_audio.h"

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__,   \
              #expr);                                                           \
      exit(1);                                                                  \
    }                                                                           \
  } while (0)

#define TEST_RING_NAME "/rj-test-ring"

static float sample_value(uint16_t channel, uint32_t frame)
{
  return (float)channel + (float)frame / 1024.0f;
}

static void fill_channels(float *const *channels,
                          uint16_t channel_count,
                          uint32_t frames,
                          uint32_t first_frame)
{
  for (uint16_t ch = 0; ch < channel_count; ch++) {
    for (uint32_t i = 0; i < frames; i++) {
      channels[ch][i] = sample_value(ch, first_frame + i);
    }
  }
}

static void check_channels(float *const *channels,
                           uint16_t channel_count,
                           uint32_t frames,
                           uint32_t first_frame)
{
  for (uint16_t ch = 0; ch < channel_count; ch++) {
    for (uint32_t i = 0; i < frames; i++) {
      ASSERT_TRUE(channels[ch][i] == sample_value(ch, first_frame + i));
    }
  }
}

static void test_create_and_open(void)
{
  SharedAudio writer;
  SharedAudio reader;

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 480) == 0);
  ASSERT_TRUE(writer.header != NULL);
  ASSERT_TRUE(writer.header->sample_rate == 48000);
  ASSERT_TRUE(writer.header->channels == 2);
  ASSERT_TRUE(writer.header->capacity_frames == 480);

  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);
  ASSERT_TRUE(reader.header != NULL);
  ASSERT_TRUE(reader.header->sample_rate == 48000);
  ASSERT_TRUE(reader.header->channels == 2);
  ASSERT_TRUE(reader.header->capacity_frames == 480);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  ASSERT_TRUE(shared_audio_unlink(TEST_RING_NAME) == 0);
}

static void test_open_missing_ring_fails(void)
{
  SharedAudio reader;

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) != 0);
}

static void test_basic_write_read(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[12], write_ch2[12];
  float read_ch1[12], read_ch2[12];
  float *write_channels[2] = {write_ch1, write_ch2};
  float *read_channels[2] = {read_ch1, read_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 480) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 0);
  ASSERT_TRUE(shared_audio_writable_frames(&writer) == 480);

  fill_channels(write_channels, 2, 12, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 12);
  ASSERT_TRUE(shared_audio_writable_frames(&writer) == 480 - 12);

  ASSERT_TRUE(shared_audio_read(&reader, read_channels, 12) == 12);
  check_channels(read_channels, 2, 12, 0);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 0);
  ASSERT_TRUE(shared_audio_writable_frames(&writer) == 480);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_wraparound_write_read(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[12], write_ch2[12];
  float read_ch1[12], read_ch2[12];
  float *write_channels[2] = {write_ch1, write_ch2};
  float *read_channels[2] = {read_ch1, read_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 16) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 12, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);
  ASSERT_TRUE(shared_audio_read(&reader, read_channels, 12) == 12);
  check_channels(read_channels, 2, 12, 0);

  /* Positions are now at 12 of 16; the next write must wrap. */
  fill_channels(write_channels, 2, 12, 12);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 12);
  ASSERT_TRUE(shared_audio_read(&reader, read_channels, 12) == 12);
  check_channels(read_channels, 2, 12, 12);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_underrun_reads_silence(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[5], write_ch2[5];
  float read_ch1[8], read_ch2[8];
  float *write_channels[2] = {write_ch1, write_ch2};
  float *read_channels[2] = {read_ch1, read_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 480) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 5, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 5) == 0);

  for (uint32_t i = 0; i < 8; i++) {
    read_ch1[i] = 1.0f;
    read_ch2[i] = 1.0f;
  }

  ASSERT_TRUE(shared_audio_read(&reader, read_channels, 8) == 5);
  check_channels(read_channels, 2, 5, 0);
  for (uint32_t i = 5; i < 8; i++) {
    ASSERT_TRUE(read_ch1[i] == 0.0f);
    ASSERT_TRUE(read_ch2[i] == 0.0f);
  }
  ASSERT_TRUE(reader.header->underruns == 1);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_overrun_drops_whole_write(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[12], write_ch2[12];
  float read_ch1[12], read_ch2[12];
  float *write_channels[2] = {write_ch1, write_ch2};
  float *read_channels[2] = {read_ch1, read_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 16) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 12, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);

  /* Only 4 frames free: this write must be dropped in full. */
  fill_channels(write_channels, 2, 12, 100);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) != 0);
  ASSERT_TRUE(writer.header->overruns == 1);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 12);

  /* The earlier audio must be intact. */
  ASSERT_TRUE(shared_audio_read(&reader, read_channels, 12) == 12);
  check_channels(read_channels, 2, 12, 0);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_rejects_header_mismatch(void)
{
  SharedAudio writer;
  SharedAudio reader;

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 480) == 0);

  writer.header->abi_version++;
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) != 0);
  writer.header->abi_version--;

  writer.header->header_bytes++;
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) != 0);
  writer.header->header_bytes--;

  writer.header->magic++;
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) != 0);
  writer.header->magic--;

  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);
  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

int main(void)
{
  test_create_and_open();
  test_open_missing_ring_fails();
  test_basic_write_read();
  test_wraparound_write_read();
  test_underrun_reads_silence();
  test_overrun_drops_whole_write();
  test_rejects_header_mismatch();

  puts("shared_audio tests passed");
  return 0;
}
