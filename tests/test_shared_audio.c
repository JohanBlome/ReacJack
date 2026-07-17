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

static void test_interleaved_read(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[12], write_ch2[12];
  float interleaved[12 * 2];
  float *write_channels[2] = {write_ch1, write_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 16) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 12, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);
  ASSERT_TRUE(shared_audio_read_interleaved(&reader, interleaved, 2, 12) == 12);
  for (uint32_t frame = 0; frame < 12; frame++) {
    ASSERT_TRUE(interleaved[frame * 2 + 0] == sample_value(0, frame));
    ASSERT_TRUE(interleaved[frame * 2 + 1] == sample_value(1, frame));
  }

  /* Positions are at 12 of 16: the next read must wrap correctly. */
  fill_channels(write_channels, 2, 12, 12);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 12) == 0);
  ASSERT_TRUE(shared_audio_read_interleaved(&reader, interleaved, 2, 12) == 12);
  for (uint32_t frame = 0; frame < 12; frame++) {
    ASSERT_TRUE(interleaved[frame * 2 + 0] == sample_value(0, 12 + frame));
    ASSERT_TRUE(interleaved[frame * 2 + 1] == sample_value(1, 12 + frame));
  }

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_interleaved_read_pads_wide_output_and_underrun(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[5], write_ch2[5];
  float interleaved[8 * 4];
  float *write_channels[2] = {write_ch1, write_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 480) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 5, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels, 5) == 0);

  for (size_t i = 0; i < 8 * 4; i++) {
    interleaved[i] = 1.0f;
  }

  /* A 4-channel reader of a 2-channel ring: channels 2-3 must be silent,
   * and the 3 missing frames must be silent in every channel. */
  ASSERT_TRUE(shared_audio_read_interleaved(&reader, interleaved, 4, 8) == 5);
  for (uint32_t frame = 0; frame < 5; frame++) {
    ASSERT_TRUE(interleaved[frame * 4 + 0] == sample_value(0, frame));
    ASSERT_TRUE(interleaved[frame * 4 + 1] == sample_value(1, frame));
    ASSERT_TRUE(interleaved[frame * 4 + 2] == 0.0f);
    ASSERT_TRUE(interleaved[frame * 4 + 3] == 0.0f);
  }
  for (uint32_t frame = 5; frame < 8; frame++) {
    for (uint32_t ch = 0; ch < 4; ch++) {
      ASSERT_TRUE(interleaved[frame * 4 + ch] == 0.0f);
    }
  }
  ASSERT_TRUE(reader.header->underruns == 1);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_seek_to_fill(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[100], write_ch2[100];
  float *write_channels[2] = {write_ch1, write_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 4800) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  for (int block = 0; block < 10; block++) {
    fill_channels(write_channels, 2, 100, (uint32_t)block * 100);
    ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels,
                                   100) == 0);
  }

  ASSERT_TRUE(shared_audio_seek_to_fill(&reader, 480) == 480);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 480);
  ASSERT_TRUE(reader.header->resets == 1);

  /* Seeking beyond what is buffered clamps to the available fill. */
  ASSERT_TRUE(shared_audio_seek_to_fill(&reader, 2000) == 480);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 480);
  ASSERT_TRUE(reader.header->resets == 2);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_regulated_read_drops_when_fill_high(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[100], write_ch2[100];
  float interleaved[12 * 2];
  float *write_channels[2] = {write_ch1, write_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 4800) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  for (int block = 0; block < 6; block++) {
    fill_channels(write_channels, 2, 100, (uint32_t)block * 100);
    ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels,
                                   100) == 0);
  }

  /* Fill 600 against target 480 +/- 48: the read must first drop
   * SHARED_AUDIO_MAX_CORRECTION frames, then deliver the next frames. */
  ASSERT_TRUE(shared_audio_read_interleaved_regulated(&reader, interleaved, 2, 12,
                                                      480, 48) == 12);
  for (uint32_t frame = 0; frame < 12; frame++) {
    uint32_t source = SHARED_AUDIO_MAX_CORRECTION + frame;
    ASSERT_TRUE(interleaved[frame * 2 + 0] == sample_value(0, source));
    ASSERT_TRUE(interleaved[frame * 2 + 1] == sample_value(1, source));
  }
  ASSERT_TRUE(reader.header->dropped_frames == SHARED_AUDIO_MAX_CORRECTION);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) ==
              600 - SHARED_AUDIO_MAX_CORRECTION - 12);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_regulated_read_duplicates_when_fill_low(void)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[100], write_ch2[100];
  float interleaved[50 * 2];
  float *write_channels[2] = {write_ch1, write_ch2};

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 4800) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 100, 0);
  ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels,
                                 100) == 0);

  /* Fill 100 against target 240 +/- 24: the read consumes fewer real frames
   * and pads the tail by duplicating the last real frame. */
  uint32_t real = 50 - SHARED_AUDIO_MAX_CORRECTION;
  ASSERT_TRUE(shared_audio_read_interleaved_regulated(&reader, interleaved, 2, 50,
                                                      240, 24) == real);
  for (uint32_t frame = 0; frame < real; frame++) {
    ASSERT_TRUE(interleaved[frame * 2 + 0] == sample_value(0, frame));
    ASSERT_TRUE(interleaved[frame * 2 + 1] == sample_value(1, frame));
  }
  for (uint32_t frame = real; frame < 50; frame++) {
    ASSERT_TRUE(interleaved[frame * 2 + 0] == sample_value(0, real - 1));
    ASSERT_TRUE(interleaved[frame * 2 + 1] == sample_value(1, real - 1));
  }
  ASSERT_TRUE(reader.header->inserted_frames == SHARED_AUDIO_MAX_CORRECTION);
  ASSERT_TRUE(shared_audio_readable_frames(&reader) == 100 - real);

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void run_drift_simulation(uint32_t writer_frames,
                                 uint32_t reader_frames,
                                 uint64_t *out_min_fill,
                                 uint64_t *out_max_fill,
                                 SharedAudioHeader *out_header)
{
  SharedAudio writer;
  SharedAudio reader;
  float write_ch1[16], write_ch2[16];
  float interleaved[16 * 2];
  float *write_channels[2] = {write_ch1, write_ch2};
  const uint32_t target = 480;
  const uint32_t tolerance = 48;

  shared_audio_unlink(TEST_RING_NAME);
  ASSERT_TRUE(shared_audio_create(&writer, TEST_RING_NAME, 48000, 2, 4800) == 0);
  ASSERT_TRUE(shared_audio_open(&reader, TEST_RING_NAME) == 0);

  fill_channels(write_channels, 2, 16, 0);
  while (shared_audio_readable_frames(&reader) < target) {
    ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels,
                                   writer_frames) == 0);
  }

  uint64_t min_fill = UINT64_MAX;
  uint64_t max_fill = 0;
  for (int iteration = 0; iteration < 20000; iteration++) {
    ASSERT_TRUE(shared_audio_write(&writer, (const float *const *)write_channels,
                                   writer_frames) == 0);
    shared_audio_read_interleaved_regulated(&reader, interleaved, 2, reader_frames,
                                            target, tolerance);
    uint64_t fill = shared_audio_readable_frames(&reader);
    if (fill < min_fill) {
      min_fill = fill;
    }
    if (fill > max_fill) {
      max_fill = fill;
    }
  }

  *out_min_fill = min_fill;
  *out_max_fill = max_fill;
  *out_header = *reader.header;

  shared_audio_close(&reader);
  shared_audio_close(&writer);
  shared_audio_unlink(TEST_RING_NAME);
}

static void test_regulation_bounds_fast_writer(void)
{
  uint64_t min_fill = 0;
  uint64_t max_fill = 0;
  SharedAudioHeader header;

  /* Writer gains one frame per cycle on the reader (~8% fast). */
  run_drift_simulation(13, 12, &min_fill, &max_fill, &header);

  ASSERT_TRUE(max_fill <= 480 + 48 + 13 + SHARED_AUDIO_MAX_CORRECTION);
  ASSERT_TRUE(header.overruns == 0);
  ASSERT_TRUE(header.underruns == 0);
  ASSERT_TRUE(header.dropped_frames > 0);
}

static void test_regulation_bounds_slow_writer(void)
{
  uint64_t min_fill = 0;
  uint64_t max_fill = 0;
  SharedAudioHeader header;

  /* Writer loses one frame per cycle on the reader (~8% slow). */
  run_drift_simulation(11, 12, &min_fill, &max_fill, &header);

  ASSERT_TRUE(min_fill >= 480 - 48 - 12 - SHARED_AUDIO_MAX_CORRECTION);
  ASSERT_TRUE(header.overruns == 0);
  ASSERT_TRUE(header.underruns == 0);
  ASSERT_TRUE(header.inserted_frames > 0);
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
  test_interleaved_read();
  test_interleaved_read_pads_wide_output_and_underrun();
  test_seek_to_fill();
  test_regulated_read_drops_when_fill_high();
  test_regulated_read_duplicates_when_fill_low();
  test_regulation_bounds_fast_writer();
  test_regulation_bounds_slow_writer();
  test_rejects_header_mismatch();

  puts("shared_audio tests passed");
  return 0;
}
