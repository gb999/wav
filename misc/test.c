#define WAV_IMPLEMENTATION
#include "wav.h"
#include "wav_convert.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) \
    do { \
        checks++; \
        if(!(cond)) { \
            failures++; \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while(0)

typedef struct mem {
    uint8_t data[1 << 16];
    uint64_t len;
} mem;

static int64_t mem_read(void *context, uint64_t offset, void *buffer, size_t size) {
    mem *m = (mem *)context;
    if(offset >= m->len) {
        return 0;
    }
    uint64_t available = m->len - offset;
    size_t n = (size < available) ? size : (size_t)available;
    memcpy(buffer, m->data + offset, n);
    return (int64_t)n;
}

static int64_t mem_read_one_byte(void *context, uint64_t offset, void *buffer, size_t size) {
    return mem_read(context, offset, buffer, size > 0 ? 1 : 0);
}

static int64_t mem_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    mem *m = (mem *)context;
    if(offset + size > sizeof(m->data)) {
        return -1;
    }
    memcpy(m->data + offset, buffer, size);
    if(offset + size > m->len) {
        m->len = offset + size;
    }
    return (int64_t)size;
}

static int64_t fail_read(void *context, uint64_t offset, void *buffer, size_t size) {
    (void)context;
    (void)offset;
    (void)buffer;
    (void)size;
    return -1;
}

static int64_t fail_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    (void)context;
    (void)offset;
    (void)buffer;
    (void)size;
    return 0;
}

static mem g;

static wav_io fresh_io(void) {
    memset(&g, 0, sizeof(g));
    wav_io io = {&g, mem_read, mem_write};
    return io;
}

static uint32_t u16_at(uint64_t o) {
    return (uint32_t)g.data[o] | ((uint32_t)g.data[o + 1] << 8);
}

static uint32_t u32_at(uint64_t o) {
    return u16_at(o) | (u16_at(o + 2) << 16);
}

static uint64_t u64_at(uint64_t o) {
    return (uint64_t)u32_at(o) | ((uint64_t)u32_at(o + 4) << 32);
}

static wav_format make_format(wav_encoding encoding, uint16_t channels, uint32_t sample_rate, uint16_t bits) {
    wav_format f;
    memset(&f, 0, sizeof(f));
    f.encoding = encoding;
    f.channels = channels;
    f.sample_rate = sample_rate;
    f.bits_per_sample = bits;
    f.block_align = (uint16_t)(channels * ((bits + 7) / 8));
    f.byte_rate = sample_rate * f.block_align;
    return f;
}

static wav_format pcm(uint16_t channels, uint32_t sample_rate, uint16_t bits) {
    return make_format(WAV_ENCODING_PCM, channels, sample_rate, bits);
}

static void put_chunk(const wav_io *io, uint64_t *at, uint32_t id, const void *payload, uint64_t size) {
    wav_write_chunk_header(io, *at, id, size, NULL);
    mem_write(io->context, *at + WAV_CHUNK_HEADER_SIZE, payload, (size_t)size);
    wav_write_chunk_pad(io, *at + WAV_CHUNK_HEADER_SIZE + size, size, NULL);
    *at += WAV_CHUNK_HEADER_SIZE + size + (size & 1);
}

static const uint8_t pcm16_mono_8k_fmt[16] = {1, 0, 1, 0, 0x40, 0x1F, 0, 0, 0x80, 0x3E, 0, 0, 2, 0, 16, 0};

static void test_container_primitives(void) {
    wav_io io = fresh_io();
    wav_container container;
    wav_error err;

    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, 100, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF && u32_at(4) == 92 && u32_at(8) == WAV_FOURCC_WAVE);
    CHECK(wav_read_container(&io, &container, &err) == WAV_OK && container == WAV_CONTAINER_RIFF);

    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RF64, 100, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RF64 && u32_at(4) == WAV_SIZE32_SENTINEL);
    CHECK(wav_read_container(&io, &container, &err) == WAV_OK && container == WAV_CONTAINER_RF64);

    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err) == WAV_OK);
    CHECK(u32_at(4) == WAV_SIZE32_SENTINEL);

    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, 0xFFFFFFFFULL + 8, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, 0xFFFFFFFEULL + 8, &err) == WAV_OK);
    CHECK(wav_write_container(&io, 0, (wav_container)99, 100, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, 100, NULL) == WAV_OK);
    CHECK(wav_read_container(&io, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
}

static void test_container_read_errors(void) {
    wav_io io = fresh_io();
    wav_container container;
    wav_error err;

    CHECK(wav_read_container(&io, &container, &err) == WAV_EOF);

    mem_write(&g, 0, "RIFF", 4);
    CHECK(wav_read_container(&io, &container, &err) == WAV_ERROR_UNEXPECTED_EOF);

    mem_write(&g, 0, "RIFF\0\0\0\0WAVX", 12);
    CHECK(wav_read_container(&io, &container, &err) == WAV_ERROR_INVALID_FORMAT);

    mem_write(&g, 0, "RIFX\0\0\0\0WAVE", 12);
    CHECK(wav_read_container(&io, &container, &err) == WAV_ERROR_INVALID_FORMAT);

    wav_io no_read = {&g, NULL, mem_write};
    CHECK(wav_read_container(&no_read, &container, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_read_container(NULL, &container, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_io bad_read = {&g, fail_read, mem_write};
    CHECK(wav_read_container(&bad_read, &container, &err) == WAV_ERROR_IO);

    wav_io bad_write = {&g, mem_read, fail_write};
    CHECK(wav_write_container(&bad_write, 0, WAV_CONTAINER_RIFF, 100, &err) == WAV_ERROR_IO);
    wav_io no_write = {&g, mem_read, NULL};
    CHECK(wav_write_container(&no_write, 0, WAV_CONTAINER_RIFF, 100, &err) == WAV_ERROR_INVALID_ARGUMENT);
}

static void test_chunk_primitives(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_chunk chunk;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    const uint8_t abc[3] = {'a', 'b', 'c'};
    const uint8_t four[4] = {1, 2, 3, 4};

    put_chunk(&io, &at, WAV_FOURCC_LIST, abc, 3);
    put_chunk(&io, &at, WAV_FOURCC_BEXT, four, 4);
    CHECK(at == 12 + 8 + 4 + 8 + 4);
    CHECK(g.data[12 + 8 + 3] == 0);

    uint64_t cursor = WAV_CONTAINER_HEADER_SIZE;
    CHECK(wav_read_chunk_header(&io, &cursor, &chunk, &err) == WAV_OK);
    CHECK(chunk.id == WAV_FOURCC_LIST && chunk.size == 3 && chunk.offset == 20);
    CHECK(memcmp(chunk.tag, "LIST", 4) == 0);
    CHECK(cursor == 20 + 4);
    CHECK(wav_read_chunk_header(&io, &cursor, &chunk, &err) == WAV_OK);
    CHECK(chunk.id == WAV_FOURCC_BEXT && chunk.size == 4 && chunk.offset == 32);
    CHECK(wav_read_chunk_header(&io, &cursor, &chunk, &err) == WAV_EOF);

    CHECK(wav_read_chunk_header(&io, NULL, &chunk, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_read_chunk_header(&io, &cursor, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);

    uint64_t before = g.len;
    CHECK(wav_write_chunk_pad(&io, 1000, 4, &err) == WAV_OK);
    CHECK(g.len == before);
    CHECK(wav_write_chunk_pad(&io, 1000, 5, &err) == WAV_OK);
    CHECK(g.len == 1001);

    CHECK(wav_write_chunk_header(&io, 0, WAV_FOURCC_DATA, 0x1FFFFFFFFULL, &err) == WAV_OK);
    CHECK(u32_at(4) == WAV_SIZE32_SENTINEL);
    CHECK(wav_write_chunk_header(&io, 0, WAV_FOURCC_DATA, 77, &err) == WAV_OK);
    CHECK(u32_at(4) == 77);
}

static void test_riff_pcm_layout_and_roundtrip(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    uint64_t bytes = 0;

    CHECK(wav_header_size(WAV_WRITE_RIFF, &f) == 44);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 100, &bytes, &err) == WAV_OK);
    CHECK(bytes == 44);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF && u32_at(4) == 36 + 400 && u32_at(8) == WAV_FOURCC_WAVE);
    CHECK(u32_at(12) == WAV_FOURCC_FMT && u32_at(16) == 16);
    CHECK(u16_at(20) == 1 && u16_at(22) == 2 && u32_at(24) == 48000 && u32_at(28) == 192000 && u16_at(32) == 4 && u16_at(34) == 16);
    CHECK(u32_at(36) == WAV_FOURCC_DATA && u32_at(40) == 400);

    wav_stream_info si;
    CHECK(wav_parse_stream_info(&io, 44 + 400, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RIFF && si.data_offset == 44 && si.data_size == 400 && si.n_frames == 100 && !si.data_truncated);
    CHECK(si.format.encoding == WAV_ENCODING_PCM && si.format.format_tag == 1 && si.format.channels == 2);
    CHECK(si.format.sample_rate == 48000 && si.format.bits_per_sample == 16 && si.format.block_align == 4 && si.format.byte_rate == 192000);
    CHECK(si.format.valid_bits_per_sample == 16 && si.format.channel_mask == 0);
    CHECK(wav_format_check(&si.format).data == NULL);

    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(si.n_frames == 100 && si.data_size == 400);

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 100, NULL, NULL) == WAV_OK);
}

static void test_odd_data_size_pad(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 8);

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 3, NULL, &err) == WAV_OK);
    CHECK(u32_at(4) == 36 + 3 + 1);
    CHECK(u32_at(40) == 3);
    CHECK(wav_write_chunk_pad(&io, 44 + 3, 3, &err) == WAV_OK);
    CHECK(g.len == 48);

    wav_stream_info si;
    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.data_size == 3 && si.n_frames == 3);
}

static void test_auto_known_small_is_riff_with_junk(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 44100, 16);
    uint64_t bytes = 0;

    CHECK(wav_header_size(WAV_WRITE_AUTO, &f) == 80);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, 1000, &bytes, &err) == WAV_OK);
    CHECK(bytes == 80);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF && u32_at(4) == 80 - 8 + 2000);
    CHECK(u32_at(12) == WAV_FOURCC_JUNK && u32_at(16) == WAV_DS64_PAYLOAD_SIZE);
    for(uint64_t i = 20; i < 48; i++) {
        CHECK(g.data[i] == 0);
    }
    CHECK(u32_at(48) == WAV_FOURCC_FMT);
    CHECK(u32_at(72) == WAV_FOURCC_DATA && u32_at(76) == 2000);

    wav_stream_info si;
    CHECK(wav_parse_stream_info(&io, 80 + 2000, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RIFF && si.data_offset == 80 && si.n_frames == 1000);
}

static void test_streaming_unknown_size(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    wav_stream_info si;

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, WAV_UNKNOWN_FRAMES, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF && u32_at(4) == WAV_SIZE32_SENTINEL && u32_at(40) == WAV_SIZE32_SENTINEL);
    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(si.data_size == WAV_UNKNOWN_SIZE && si.n_frames == WAV_UNKNOWN_FRAMES);
    CHECK(wav_parse_stream_info(&io, 44 + 1000, &si, &err) == WAV_OK);
    CHECK(si.data_size == 1000 && si.n_frames == 250 && !si.data_truncated);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RF64, &f, WAV_UNKNOWN_FRAMES, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RF64 && u32_at(4) == WAV_SIZE32_SENTINEL);
    CHECK(u32_at(12) == WAV_FOURCC_DS64 && u32_at(16) == WAV_DS64_PAYLOAD_SIZE);
    CHECK(u64_at(20) == UINT64_MAX && u64_at(28) == UINT64_MAX && u64_at(36) == UINT64_MAX && u32_at(44) == 0);
    CHECK(u32_at(80 - 8) == WAV_FOURCC_DATA && u32_at(80 - 4) == WAV_SIZE32_SENTINEL);
    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RF64 && si.n_frames == WAV_UNKNOWN_FRAMES);
    CHECK(wav_parse_stream_info(&io, 80 + 4000, &si, &err) == WAV_OK);
    CHECK(si.data_size == 4000 && si.n_frames == 1000);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, WAV_UNKNOWN_FRAMES, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RF64);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RF64, &f, 100, NULL, &err) == WAV_OK);
    CHECK(u64_at(20) == 80 + 400 - 8 && u64_at(28) == 400 && u64_at(36) == 100);
    CHECK(u32_at(76) == WAV_SIZE32_SENTINEL);
    CHECK(wav_parse_stream_info(&io, 80 + 400, &si, &err) == WAV_OK && si.n_frames == 100);
}

static void test_auto_and_riff_size_limits(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    wav_stream_info si;
    const uint64_t frames = 1500000000ULL;
    const uint64_t data = frames * 4;

    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, frames, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RF64);
    CHECK(u32_at(12) == WAV_FOURCC_DS64);
    CHECK(u64_at(20) == 80 + data - 8 && u64_at(28) == data && u64_at(36) == frames);
    CHECK(u32_at(76) == WAV_SIZE32_SENTINEL);
    CHECK(wav_parse_stream_info(&io, 80 + data, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RF64 && si.data_size == data && si.n_frames == frames && !si.data_truncated);
    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(si.data_size == data && si.n_frames == frames);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, frames, NULL, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(g.len == 0);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RF64, &f, frames, NULL, &err) == WAV_OK);

    wav_format mono8 = pcm(1, 8000, 8);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &mono8, 4294967258ULL, NULL, &err) == WAV_OK);
    CHECK(u32_at(4) == 0xFFFFFFFEu);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &mono8, 4294967260ULL, NULL, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &mono8, 4294967258ULL - 36, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &mono8, 4294967258ULL - 34, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RF64);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RF64, &f, UINT64_MAX / 2, NULL, &err) == WAV_ERROR_OUT_OF_BOUNDS);
}

static void test_float_has_fact_and_fmt18(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = make_format(WAV_ENCODING_IEEE_FLOAT, 1, 48000, 32);
    wav_stream_info si;

    CHECK(wav_header_size(WAV_WRITE_RIFF, &f) == 58);
    CHECK(wav_header_size(WAV_WRITE_RF64, &f) == 94);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 500, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 18 && u16_at(20) == 3 && u16_at(36) == 0);
    CHECK(u32_at(38) == WAV_FOURCC_FACT && u32_at(42) == 4 && u32_at(46) == 500);
    CHECK(u32_at(50) == WAV_FOURCC_DATA && u32_at(54) == 2000);
    CHECK(wav_parse_stream_info(&io, 58 + 2000, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_IEEE_FLOAT && si.data_offset == 58 && si.n_frames == 500);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, WAV_UNKNOWN_FRAMES, NULL, &err) == WAV_OK);
    CHECK(u32_at(46) == WAV_SIZE32_SENTINEL);

    wav_format f64 = make_format(WAV_ENCODING_IEEE_FLOAT, 2, 96000, 64);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f64, 10, NULL, &err) == WAV_OK);
    CHECK(wav_parse_stream_info(&io, 58 + 160, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_IEEE_FLOAT && si.format.bits_per_sample == 64 && si.n_frames == 10);
}

static void test_alaw_mulaw(void) {
    wav_encoding encodings[2] = {WAV_ENCODING_ALAW, WAV_ENCODING_MULAW};
    uint16_t tags[2] = {6, 7};
    for(int i = 0; i < 2; i++) {
        wav_io io = fresh_io();
        wav_error err;
        wav_format f = make_format(encodings[i], 1, 8000, 8);
        wav_stream_info si;
        CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 80, NULL, &err) == WAV_OK);
        CHECK(u16_at(20) == tags[i] && u32_at(16) == 18 && u32_at(38) == WAV_FOURCC_FACT);
        CHECK(wav_parse_stream_info(&io, 58 + 80, &si, &err) == WAV_OK);
        CHECK(si.format.encoding == encodings[i] && si.n_frames == 80);
        CHECK(wav_format_check(&si.format).data == NULL);
    }
}

static void test_extensible_roundtrip(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(6, 48000, 32);
    f.valid_bits_per_sample = 24;
    f.channel_mask = WAV_CHANNEL_MASK_5_1;
    wav_stream_info si;

    CHECK(wav_header_size(WAV_WRITE_RIFF, &f) == 68);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40 && u16_at(20) == 0xFFFE && u16_at(36) == 22);
    CHECK(u16_at(38) == 24 && u32_at(40) == 0x3F && u32_at(44) == 1);
    CHECK(memcmp(g.data + 48, "\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 12) == 0);
    CHECK(wav_parse_stream_info(&io, 68 + 240, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_PCM && si.format.format_tag == 0xFFFE);
    CHECK(si.format.channels == 6 && si.format.bits_per_sample == 32 && si.format.valid_bits_per_sample == 24);
    CHECK(si.format.channel_mask == WAV_CHANNEL_MASK_5_1 && si.n_frames == 10);
    CHECK(wav_format_check(&si.format).data == NULL);

    wav_format roundtrip = si.format;
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &roundtrip, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40 && u32_at(44) == 1);

    wav_format f24 = pcm(1, 48000, 24);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f24, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40 && u16_at(20) == 0xFFFE);

    wav_format fmask = pcm(2, 48000, 16);
    fmask.channel_mask = WAV_CHANNEL_MASK_STEREO;
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &fmask, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40);

    wav_format fsurround_float = make_format(WAV_ENCODING_IEEE_FLOAT, 4, 48000, 32);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &fsurround_float, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40 && u32_at(44) == 3);
    CHECK(wav_header_size(WAV_WRITE_RIFF, &fsurround_float) == 12 + 8 + 40 + 8 + 4 + 8);
}

static void test_custom_tag_and_guid(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;

    wav_format adpcm = make_format(WAV_ENCODING_UNKNOWN, 1, 8000, 4);
    adpcm.format_tag = 2;
    adpcm.block_align = 256;
    adpcm.byte_rate = 4000;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &adpcm, 5, NULL, &err) == WAV_OK);
    CHECK(u16_at(20) == 2 && u32_at(16) == 18 && u32_at(38) == WAV_FOURCC_FACT);
    CHECK(wav_parse_stream_info(&io, 58 + 5 * 256, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_UNKNOWN && si.format.format_tag == 2 && si.format.block_align == 256 && si.n_frames == 5);
    CHECK(wav_format_check(&si.format).data == NULL);

    wav_format unknown = make_format(WAV_ENCODING_UNKNOWN, 1, 8000, 16);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &unknown, 5, NULL, &err) == WAV_ERROR_UNSUPPORTED_FORMAT);
    CHECK(g.len == 0);
    CHECK(wav_header_size(WAV_WRITE_RIFF, &unknown) == 0);

    wav_format guid = make_format(WAV_ENCODING_UNKNOWN, 1, 8000, 16);
    for(int i = 0; i < 16; i++) {
        guid.subformat_guid[i] = (uint8_t)(0xA0 + i);
    }
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &guid, 5, NULL, &err) == WAV_OK);
    CHECK(u32_at(16) == 40 && u16_at(20) == 0xFFFE);
    CHECK(memcmp(g.data + 44, guid.subformat_guid, 16) == 0);
    CHECK(wav_parse_stream_info(&io, 68 + 10, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_UNKNOWN && memcmp(si.format.subformat_guid, guid.subformat_guid, 16) == 0);

    wav_format pcm_guid = pcm(1, 8000, 16);
    pcm_guid.subformat_guid[0] = 1;
    memcpy(pcm_guid.subformat_guid + 4, "\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 12);
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &pcm_guid, 5, NULL, &err) == WAV_OK);
    CHECK(wav_header_size(WAV_WRITE_RIFF, &pcm_guid) == 12 + 8 + 40 + 8);
    CHECK(wav_parse_stream_info(&io, 68 + 10, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_PCM);
}

static void test_header_size_matches_bytes_written(void) {
    wav_format formats[4] = {
        pcm(2, 48000, 16),
        make_format(WAV_ENCODING_IEEE_FLOAT, 2, 48000, 32),
        pcm(6, 48000, 24),
        make_format(WAV_ENCODING_ALAW, 1, 8000, 8),
    };
    wav_write_mode modes[3] = {WAV_WRITE_RIFF, WAV_WRITE_RF64, WAV_WRITE_AUTO};
    for(int fi = 0; fi < 4; fi++) {
        for(int mi = 0; mi < 3; mi++) {
            wav_io io = fresh_io();
            uint64_t bytes = 0;
            wav_error err;
            CHECK(wav_write_header(&io, 0, modes[mi], &formats[fi], 7, &bytes, &err) == WAV_OK);
            CHECK(bytes == wav_header_size(modes[mi], &formats[fi]));
            CHECK(g.len == bytes);
        }
    }
    wav_format f = pcm(1, 8000, 16);
    CHECK(wav_header_size((wav_write_mode)99, &f) == 0);
    CHECK(wav_header_size(WAV_WRITE_RIFF, NULL) == 0);
    CHECK(wav_header_size(WAV_WRITE_AUTO, &f) == wav_header_size(WAV_WRITE_RF64, &f));
    CHECK(wav_header_size(WAV_WRITE_AUTO, &f) == wav_header_size(WAV_WRITE_RIFF, &f) + 8 + WAV_DS64_PAYLOAD_SIZE);
}

static void test_header_at_nonzero_offset(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);
    wav_stream_info si;
    uint64_t bytes = 0;

    CHECK(wav_write_header(&io, 100, WAV_WRITE_RIFF, &f, 4, &bytes, &err) == WAV_OK);
    CHECK(u32_at(100) == WAV_FOURCC_RIFF && u32_at(104) == 36 + 8);
    CHECK(g.len == 100 + bytes);
    (void)si;
}

static void test_write_header_argument_errors(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);

    CHECK(wav_write_header(&io, 0, (wav_write_mode)99, &f, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, NULL, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_format bad = f;
    bad.channels = 0;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &bad, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
    bad = f;
    bad.sample_rate = 0;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &bad, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
    bad = f;
    bad.bits_per_sample = 0;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &bad, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
    bad = f;
    bad.block_align = 0;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &bad, 1, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(g.len == 0);

    wav_io bad_write = {&g, mem_read, fail_write};
    CHECK(wav_write_header(&bad_write, 0, WAV_WRITE_RIFF, &f, 1, NULL, &err) == WAV_ERROR_IO);
}

static void test_inconsistent_fields_are_trusted_not_rejected(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    f.byte_rate = 12345;
    f.block_align = 8;

    CHECK(wav_format_check(&f).data != NULL);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(28) == 12345 && u16_at(32) == 8);
    CHECK(u32_at(40) == 80);

    wav_stream_info si;
    CHECK(wav_parse_stream_info(&io, 44 + 80, &si, &err) == WAV_OK);
    CHECK(si.format.block_align == 8 && si.n_frames == 10);
}

static void test_format_check(void) {
    wav_format ok = pcm(2, 48000, 16);
    CHECK(wav_format_check(&ok).data == NULL && wav_format_check(&ok).size == 0);
    CHECK(wav_format_check(NULL).data == NULL);

    wav_format f = ok;
    f.block_align = 3;
    CHECK(wav_format_check(&f).data != NULL && strstr(wav_format_check(&f).data, "block_align") != NULL);

    f = ok;
    f.byte_rate = 1;
    CHECK(wav_format_check(&f).data != NULL && strstr(wav_format_check(&f).data, "byte_rate") != NULL);

    f = ok;
    f.valid_bits_per_sample = 20;
    CHECK(wav_format_check(&f).data != NULL && strstr(wav_format_check(&f).data, "valid_bits") != NULL);

    f = pcm(2, 48000, 24);
    f.valid_bits_per_sample = 20;
    CHECK(wav_format_check(&f).data == NULL);

    f = pcm(2, 48000, 65);
    CHECK(wav_format_check(&f).data != NULL);

    f = make_format(WAV_ENCODING_IEEE_FLOAT, 1, 48000, 24);
    CHECK(wav_format_check(&f).data != NULL);
    f = make_format(WAV_ENCODING_IEEE_FLOAT, 1, 48000, 64);
    CHECK(wav_format_check(&f).data == NULL);

    f = make_format(WAV_ENCODING_ALAW, 1, 8000, 16);
    CHECK(wav_format_check(&f).data != NULL);
    f = make_format(WAV_ENCODING_MULAW, 1, 8000, 8);
    CHECK(wav_format_check(&f).data == NULL);

    f = pcm(2, 48000, 16);
    f.channel_mask = WAV_SPEAKER_FRONT_LEFT;
    CHECK(wav_format_check(&f).data != NULL && strstr(wav_format_check(&f).data, "channel_mask") != NULL);
    f.channel_mask = WAV_CHANNEL_MASK_STEREO;
    CHECK(wav_format_check(&f).data == NULL);

    f = make_format(WAV_ENCODING_UNKNOWN, 1, 8000, 4);
    f.block_align = 999;
    f.byte_rate = 1;
    CHECK(wav_format_check(&f).data == NULL);

    CHECK(WAV_CHANNEL_MASK_STEREO == 0x3);
    CHECK(WAV_CHANNEL_MASK_5_1 == 0x3F);
    CHECK(WAV_CHANNEL_MASK_7_1 == 0x63F);
}

static void test_read_and_write_frames(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);
    wav_stream_info si;
    int16_t samples[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    int16_t out[8] = {0};
    uint64_t header = wav_header_size(WAV_WRITE_RIFF, &f);

    CHECK(wav_write_data_frames(&io, header, &f, samples, 8, &err) == WAV_OK);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 8, NULL, &err) == WAV_OK);
    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.n_frames == 8);

    CHECK(wav_read_data_frames(&io, &si, 2, out, 4, &err) == WAV_OK);
    CHECK(out[0] == 30 && out[1] == 40 && out[2] == 50 && out[3] == 60);
    CHECK(wav_read_data_frames(&io, &si, 0, out, 8, &err) == WAV_OK && out[7] == 80);
    CHECK(wav_read_data_frames(&io, &si, 7, out, 1, &err) == WAV_OK && out[0] == 80);
    CHECK(wav_read_data_frames(&io, &si, 8, out, 0, &err) == WAV_OK);
    CHECK(wav_read_data_frames(&io, &si, 0, NULL, 0, &err) == WAV_OK);

    CHECK(wav_read_data_frames(&io, &si, 6, out, 3, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &si, 9, out, 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &si, UINT64_MAX, out, 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &si, 0, out, UINT64_MAX - 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &si, 0, NULL, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_read_data_frames(&io, NULL, 0, out, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_stream_info zero_block = si;
    zero_block.format.block_align = 0;
    CHECK(wav_read_data_frames(&io, &zero_block, 0, out, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_io bad_read = {&g, fail_read, mem_write};
    CHECK(wav_read_data_frames(&bad_read, &si, 0, out, 1, &err) == WAV_ERROR_IO);

    wav_stream_info streamed = si;
    streamed.n_frames = WAV_UNKNOWN_FRAMES;
    CHECK(wav_read_data_frames(&io, &streamed, 0, out, UINT64_MAX / 2 + 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &streamed, UINT64_MAX / 2, out, 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_read_data_frames(&io, &streamed, 0, out, 8, &err) == WAV_OK);

    CHECK(wav_write_data_frames(&io, header, &f, samples, 0, &err) == WAV_OK);
    CHECK(wav_write_data_frames(&io, header, &f, NULL, 0, &err) == WAV_OK);
    CHECK(wav_write_data_frames(&io, header, &f, NULL, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_data_frames(&io, header, NULL, samples, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_data_frames(&io, header, &f, samples, UINT64_MAX / 2, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_write_data_frames(&io, header, &f, samples, UINT64_MAX / 2 + 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    CHECK(wav_write_data_frames(&io, UINT64_MAX - 3, &f, samples, 8, &err) == WAV_ERROR_OUT_OF_BOUNDS);
    wav_format no_block = f;
    no_block.block_align = 0;
    CHECK(wav_write_data_frames(&io, header, &no_block, samples, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    wav_io bad_write = {&g, mem_read, fail_write};
    CHECK(wav_write_data_frames(&bad_write, header, &f, samples, 1, &err) == WAV_ERROR_IO);
    CHECK(wav_write_data_frames(&io, sizeof(g.data), &f, samples, 1, &err) == WAV_ERROR_IO);
}

static void test_short_io_reads_are_looped(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);
    wav_stream_info si;
    int16_t samples[4] = {1, 2, 3, 4};
    int16_t out[4] = {0};

    wav_write_data_frames(&io, 44, &f, samples, 4, &err);
    wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 4, NULL, &err);

    wav_io trickle = {&g, mem_read_one_byte, mem_write};
    CHECK(wav_parse_stream_info(&trickle, g.len, &si, &err) == WAV_OK && si.n_frames == 4);
    CHECK(wav_read_data_frames(&trickle, &si, 0, out, 4, &err) == WAV_OK);
    CHECK(out[0] == 1 && out[3] == 4);
}

static void test_truncated_file(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);
    wav_stream_info si;
    int16_t out[100];

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 100, NULL, &err) == WAV_OK);
    mem_write(&g, 44, out, 20);

    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.data_truncated && si.data_size == 20 && si.n_frames == 10);

    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(!si.data_truncated && si.n_frames == 100);
    CHECK(wav_read_data_frames(&io, &si, 0, out, 100, &err) == WAV_ERROR_UNEXPECTED_EOF);
    CHECK(wav_read_data_frames(&io, &si, 0, out, 10, &err) == WAV_OK);

    CHECK(wav_parse_stream_info(&io, 30, &si, &err) == WAV_OK);
    CHECK(si.data_truncated && si.data_size == 0 && si.n_frames == 0);
}

static void test_parse_skips_unknown_chunks(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    const uint8_t abc[3] = {'a', 'b', 'c'};
    const uint8_t samples[4] = {1, 0, 2, 0};

    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_LIST, abc, 3);
    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    put_chunk(&io, &at, WAV_FOURCC_BEXT, abc, 3);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);

    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.data_offset == at - 4 && si.data_size == 4 && si.n_frames == 2);
    CHECK(si.format.sample_rate == 8000 && si.format.channels == 1);
}

static void test_parse_errors(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    const uint8_t samples[4] = {1, 0, 2, 0};

    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_EOF);
    CHECK(wav_parse_stream_info(&io, 0, NULL, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 14);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    uint8_t zero_channels[16];
    memcpy(zero_channels, pcm16_mono_8k_fmt, 16);
    zero_channels[2] = 0;
    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, zero_channels, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    uint8_t zero_block[16];
    memcpy(zero_block, pcm16_mono_8k_fmt, 16);
    zero_block[12] = 0;
    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, zero_block, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    uint8_t extensible_short[16];
    memcpy(extensible_short, pcm16_mono_8k_fmt, 16);
    extensible_short[0] = 0xFE;
    extensible_short[1] = 0xFF;
    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, extensible_short, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    wav_write_chunk_header(&io, at, WAV_FOURCC_FMT, 16, &err);
    mem_write(&g, at + 8, pcm16_mono_8k_fmt, 6);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_UNEXPECTED_EOF);

    wav_io bad_read = {&g, fail_read, mem_write};
    CHECK(wav_parse_stream_info(&bad_read, 0, &si, &err) == WAV_ERROR_IO);
}

static void test_unknown_tag_in_parsed_plain_fmt(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    uint8_t mp3_fmt[16];
    const uint8_t data[4] = {0, 0, 0, 0};

    memcpy(mp3_fmt, pcm16_mono_8k_fmt, 16);
    mp3_fmt[0] = 0x55;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, mp3_fmt, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, data, 4);

    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.format.encoding == WAV_ENCODING_UNKNOWN && si.format.format_tag == 0x55);
}

static void test_ds64_data_size_used_for_rf64(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    uint8_t ds64[WAV_DS64_PAYLOAD_SIZE] = {0};
    const uint64_t big = 10ULL * 1024 * 1024 * 1024;

    for(int i = 0; i < 8; i++) {
        ds64[8 + i] = (uint8_t)(big >> (8 * i));
    }
    wav_write_container(&io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_DS64, ds64, sizeof(ds64));
    put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    wav_write_chunk_header(&io, at, WAV_FOURCC_DATA, UINT64_MAX, &err);

    CHECK(wav_parse_stream_info(&io, at + 8 + big, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RF64 && si.data_size == big && si.n_frames == big / 2 && !si.data_truncated);
    CHECK(wav_parse_stream_info(&io, at + 8 + big - 2, &si, &err) == WAV_OK);
    CHECK(si.data_truncated && si.n_frames == big / 2 - 1);

    uint8_t ds64_small[WAV_DS64_PAYLOAD_SIZE - 4] = {0};
    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_DS64, ds64_small, sizeof(ds64_small));
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);
}

static void test_error_struct_is_filled(void) {
    wav_io io = fresh_io();
    wav_error err;
    memset(&err, 0, sizeof(err));

    wav_container container;
    CHECK(wav_read_container(&io, &container, &err) == WAV_EOF);
    mem_write(&g, 0, "RIFF\0\0\0\0WAVX", 12);
    CHECK(wav_read_container(&io, &container, &err) == WAV_ERROR_INVALID_FORMAT);
    CHECK(err.code == WAV_ERROR_INVALID_FORMAT && err.offset == 8 && strstr(err.message, "WAVE") != NULL);
}


static void test_auto_pipeline_reserve_stream_then_header(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    wav_stream_info si;
    int16_t frames[6] = {1, -1, 2, -2, 3, -3};
    int16_t out[6] = {0};

    uint64_t header = wav_header_size(WAV_WRITE_AUTO, &f);
    CHECK(wav_write_data_frames(&io, header, &f, frames, 2, &err) == WAV_OK);
    CHECK(wav_write_data_frames(&io, header + 2 * f.block_align, &f, frames + 4, 1, &err) == WAV_OK);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, 3, NULL, &err) == WAV_OK);
    CHECK(u32_at(0) == WAV_FOURCC_RIFF);
    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.data_offset == header && si.n_frames == 3 && !si.data_truncated);
    CHECK(wav_read_data_frames(&io, &si, 0, out, 3, &err) == WAV_OK);
    CHECK(out[0] == 1 && out[1] == -1 && out[2] == 2 && out[3] == -2 && out[4] == 3 && out[5] == -3);

    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, WAV_UNKNOWN_FRAMES, NULL, &err) == WAV_OK);
    CHECK(wav_write_data_frames(&io, header, &f, frames, 3, &err) == WAV_OK);
    CHECK(wav_parse_stream_info(&io, g.len, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RF64 && si.n_frames == 3);
    CHECK(wav_read_data_frames(&io, &si, 0, out, 3, &err) == WAV_OK && out[5] == -3);
}

typedef struct sparse_file {
    uint64_t size;
    int n_patches;
    struct {
        uint64_t offset;
        size_t size;
        uint8_t bytes[64];
    } patches[32];
} sparse_file;

static int64_t sparse_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    sparse_file *f = (sparse_file *)context;
    if(size > 64 || f->n_patches == 32 || offset + size > f->size) {
        return -1;
    }
    f->patches[f->n_patches].offset = offset;
    f->patches[f->n_patches].size = size;
    memcpy(f->patches[f->n_patches].bytes, buffer, size);
    f->n_patches++;
    return (int64_t)size;
}

static int64_t sparse_read(void *context, uint64_t offset, void *buffer, size_t size) {
    sparse_file *f = (sparse_file *)context;
    if(offset >= f->size) {
        return 0;
    }
    uint64_t available = f->size - offset;
    size_t n = (size < available) ? size : (size_t)available;
    memset(buffer, 0, n);
    for(int p = 0; p < f->n_patches; p++) {
        uint64_t start = f->patches[p].offset;
        uint64_t end = start + f->patches[p].size;
        for(uint64_t i = (start > offset ? start : offset); i < end && i < offset + n; i++) {
            ((uint8_t *)buffer)[i - offset] = f->patches[p].bytes[i - start];
        }
    }
    return (int64_t)n;
}

static void test_auto_switches_to_rf64_on_a_sparse_virtual_file(void) {
    wav_error err;
    wav_format f = pcm(2, 48000, 16);
    uint64_t header = wav_header_size(WAV_WRITE_AUTO, &f);
    uint64_t n_frames = 0x40000000ULL + 16;
    uint64_t data_size = n_frames * f.block_align;
    static sparse_file file;
    memset(&file, 0, sizeof(file));
    file.size = header + data_size;
    wav_io io = {&file, sparse_read, sparse_write};

    int16_t first[2] = {111, -111};
    int16_t last[2] = {222, -222};
    int16_t middle[2] = {1, 1};
    CHECK(wav_write_data_frames(&io, header, &f, first, 1, &err) == WAV_OK);
    CHECK(wav_write_data_frames(&io, header + (n_frames - 1) * f.block_align, &f, last, 1, &err) == WAV_OK);

    uint64_t bytes = 0;
    CHECK(wav_write_header(&io, 0, WAV_WRITE_AUTO, &f, n_frames, &bytes, &err) == WAV_OK);
    CHECK(bytes == header);

    wav_container container;
    CHECK(wav_read_container(&io, &container, &err) == WAV_OK && container == WAV_CONTAINER_RF64);

    wav_stream_info si;
    CHECK(wav_parse_stream_info(&io, file.size, &si, &err) == WAV_OK);
    CHECK(si.container == WAV_CONTAINER_RF64 && si.data_size == data_size && si.n_frames == n_frames && !si.data_truncated);
    CHECK(wav_read_data_frames(&io, &si, 0, middle, 1, &err) == WAV_OK && middle[0] == 111 && middle[1] == -111);
    CHECK(wav_read_data_frames(&io, &si, n_frames - 1, middle, 1, &err) == WAV_OK && middle[0] == 222 && middle[1] == -222);
    CHECK(wav_read_data_frames(&io, &si, 0x20000000ULL, middle, 1, &err) == WAV_OK && middle[0] == 0 && middle[1] == 0);
    CHECK(wav_read_data_frames(&io, &si, n_frames, middle, 1, &err) == WAV_ERROR_OUT_OF_BOUNDS);

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, n_frames, NULL, &err) == WAV_ERROR_OUT_OF_BOUNDS);
}

static void le16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void test_full_scale_is_visible(void) {
    CHECK(wav_pcm_full_scale(8, WAV_PCM_SCALE_POW2) == 128.0);
    CHECK(wav_pcm_full_scale(16, WAV_PCM_SCALE_POW2) == 32768.0);
    CHECK(wav_pcm_full_scale(24, WAV_PCM_SCALE_POW2) == 8388608.0);
    CHECK(wav_pcm_full_scale(32, WAV_PCM_SCALE_POW2) == 2147483648.0);
    CHECK(wav_pcm_full_scale(8, WAV_PCM_SCALE_MAX) == 127.0);
    CHECK(wav_pcm_full_scale(16, WAV_PCM_SCALE_MAX) == 32767.0);
    CHECK(wav_pcm_full_scale(24, WAV_PCM_SCALE_MAX) == 8388607.0);
    CHECK(wav_pcm_full_scale(32, WAV_PCM_SCALE_MAX) == 2147483647.0);

    CHECK(wav_pcm_full_scale(1, WAV_PCM_SCALE_POW2) == 128.0);
    CHECK(wav_pcm_full_scale(12, WAV_PCM_SCALE_POW2) == 32768.0);
    CHECK(wav_pcm_full_scale(20, WAV_PCM_SCALE_MAX) == 8388607.0);

    CHECK(wav_pcm_full_scale(0, WAV_PCM_SCALE_POW2) == 0.0);
    CHECK(wav_pcm_full_scale(65, WAV_PCM_SCALE_POW2) == 0.0);
    CHECK(wav_pcm_full_scale(33, WAV_PCM_SCALE_POW2) == 549755813888.0);
    CHECK(wav_pcm_full_scale(48, WAV_PCM_SCALE_MAX) == 140737488355327.0);
    CHECK(wav_pcm_full_scale(64, WAV_PCM_SCALE_POW2) == 9223372036854775808.0);
    CHECK(wav_pcm_full_scale(64, WAV_PCM_SCALE_MAX) == wav_pcm_full_scale(64, WAV_PCM_SCALE_POW2));
    CHECK(wav_pcm_full_scale(16, (wav_pcm_scale)0) == 0.0);
    CHECK(wav_pcm_full_scale(16, (wav_pcm_scale)99) == 0.0);
}

static void test_pcm_to_float_values(void) {
    wav_error err;
    float f[3];
    double d[3];

    const uint8_t pcm8[3] = {0, 128, 255};
    CHECK(wav_pcm_to_f32(pcm8, 8, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_OK);
    CHECK(f[0] == -1.0f && f[1] == 0.0f && f[2] == 127.0f / 128.0f);
    CHECK(wav_pcm_to_f32(pcm8, 8, WAV_PCM_SCALE_MAX, f, 3, &err) == WAV_OK);
    CHECK(f[0] == (float)(-128.0 / 127.0) && f[1] == 0.0f && f[2] == 1.0f);

    uint8_t pcm16[6];
    le16(pcm16, INT16_MIN);
    le16(pcm16 + 2, 0);
    le16(pcm16 + 4, INT16_MAX);
    CHECK(wav_pcm_to_f32(pcm16, 16, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_OK);
    CHECK(f[0] == -1.0f && f[1] == 0.0f && f[2] == 32767.0f / 32768.0f);
    CHECK(wav_pcm_to_f32(pcm16, 16, WAV_PCM_SCALE_MAX, f, 3, &err) == WAV_OK);
    CHECK(f[0] < -1.0f && f[1] == 0.0f && f[2] == 1.0f);
    CHECK(wav_pcm_to_f64(pcm16, 16, WAV_PCM_SCALE_POW2, d, 3, &err) == WAV_OK);
    CHECK(d[0] == -1.0 && d[1] == 0.0 && d[2] == 32767.0 / 32768.0);
    CHECK(wav_pcm_to_f64(pcm16, 16, WAV_PCM_SCALE_MAX, d, 3, &err) == WAV_OK);
    CHECK(d[0] == -32768.0 / 32767.0 && d[1] == 0.0 && d[2] == 1.0);

    const uint8_t pcm24[9] = {0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F};
    CHECK(wav_pcm_to_f64(pcm24, 24, WAV_PCM_SCALE_POW2, d, 3, &err) == WAV_OK);
    CHECK(d[0] == -1.0 && d[1] == 0.0 && d[2] == 8388607.0 / 8388608.0);
    CHECK(wav_pcm_to_f64(pcm24, 24, WAV_PCM_SCALE_MAX, d, 3, &err) == WAV_OK && d[2] == 1.0);

    const uint8_t pcm24_negative_one[3] = {0xFF, 0xFF, 0xFF};
    CHECK(wav_pcm_to_f64(pcm24_negative_one, 24, WAV_PCM_SCALE_POW2, d, 1, &err) == WAV_OK && d[0] == -1.0 / 8388608.0);

    const uint8_t pcm32[12] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x7F};
    CHECK(wav_pcm_to_f64(pcm32, 32, WAV_PCM_SCALE_POW2, d, 3, &err) == WAV_OK);
    CHECK(d[0] == -1.0 && d[1] == 0.0 && d[2] == 2147483647.0 / 2147483648.0);
    CHECK(wav_pcm_to_f32(pcm32, 32, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_OK);
    CHECK(f[0] == -1.0f && f[1] == 0.0f && f[2] == 1.0f);

    const uint8_t pcm64[24] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    CHECK(wav_pcm_to_f64(pcm64, 64, WAV_PCM_SCALE_POW2, d, 3, &err) == WAV_OK);
    CHECK(d[0] == -1.0 && d[1] == 0.0 && d[2] == 1.0);
    CHECK(wav_pcm_to_f32(pcm64, 64, WAV_PCM_SCALE_MAX, f, 3, &err) == WAV_OK);
    CHECK(f[0] == -1.0f && f[1] == 0.0f && f[2] == 1.0f);

    const uint8_t pcm40[10] = {0x00, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    CHECK(wav_pcm_to_f64(pcm40, 40, WAV_PCM_SCALE_POW2, d, 2, &err) == WAV_OK);
    CHECK(d[0] == -1.0 && d[1] == 549755813887.0 / 549755813888.0);

    uint8_t shifted[1 + 6];
    memcpy(shifted + 1, pcm16, 6);
    CHECK(wav_pcm_to_f32(shifted + 1, 16, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_OK && f[0] == -1.0f);

    float f12[3];
    CHECK(wav_pcm_to_f32(pcm16, 12, WAV_PCM_SCALE_POW2, f12, 3, &err) == WAV_OK);
    CHECK(memcmp(f, f12, sizeof(f)) == 0);

    CHECK(wav_pcm_to_f32(NULL, 16, WAV_PCM_SCALE_POW2, NULL, 0, &err) == WAV_OK);
    CHECK(wav_pcm_to_f64(NULL, 16, WAV_PCM_SCALE_POW2, NULL, 0, &err) == WAV_OK);
    CHECK(wav_pcm_to_f32(pcm16, 65, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_pcm_to_f32(pcm16, 0, WAV_PCM_SCALE_POW2, f, 3, NULL) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_pcm_to_f32(pcm16, 16, (wav_pcm_scale)0, f, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_pcm_to_f64(pcm16, 16, (wav_pcm_scale)99, d, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_pcm_to_f32(NULL, 16, WAV_PCM_SCALE_POW2, f, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_pcm_to_f64(pcm16, 16, WAV_PCM_SCALE_POW2, NULL, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
}

static int encode16(double x, wav_pcm_scale scale, bool use_double) {
    uint8_t b[2];
    wav_error err;
    if(use_double) {
        CHECK(wav_f64_to_pcm(&x, b, 16, scale, 1, &err) == WAV_OK);
    }
    else {
        float xf = (float)x;
        CHECK(wav_f32_to_pcm(&xf, b, 16, scale, 1, &err) == WAV_OK);
    }
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void test_float_to_pcm_values(void) {
    wav_error err;
    uint8_t out[12];
    const double nan = NAN;
    const double inf = INFINITY;

    for(int use_double = 0; use_double < 2; use_double++) {
        bool dbl = use_double == 1;
        CHECK(encode16(1.0, WAV_PCM_SCALE_POW2, dbl) == 32767);
        CHECK(encode16(-1.0, WAV_PCM_SCALE_POW2, dbl) == -32768);
        CHECK(encode16(0.5, WAV_PCM_SCALE_POW2, dbl) == 16384);
        CHECK(encode16(2.0, WAV_PCM_SCALE_POW2, dbl) == 32767);
        CHECK(encode16(-2.0, WAV_PCM_SCALE_POW2, dbl) == -32768);
        CHECK(encode16(nan, WAV_PCM_SCALE_POW2, dbl) == 0);
        CHECK(encode16(inf, WAV_PCM_SCALE_POW2, dbl) == 32767);
        CHECK(encode16(-inf, WAV_PCM_SCALE_POW2, dbl) == -32768);

        CHECK(encode16(1.0, WAV_PCM_SCALE_MAX, dbl) == 32767);
        CHECK(encode16(-1.0, WAV_PCM_SCALE_MAX, dbl) == -32767);
        CHECK(encode16(0.0, WAV_PCM_SCALE_MAX, dbl) == 0);
        CHECK(encode16(-2.0, WAV_PCM_SCALE_MAX, dbl) == -32768);
        CHECK(encode16(2.0, WAV_PCM_SCALE_MAX, dbl) == 32767);

        CHECK(encode16(0.5 / 32768.0, WAV_PCM_SCALE_POW2, dbl) == 1);
        CHECK(encode16(-0.5 / 32768.0, WAV_PCM_SCALE_POW2, dbl) == -1);
        CHECK(encode16(0.49 / 32768.0, WAV_PCM_SCALE_POW2, dbl) == 0);
        CHECK(encode16(-0.49 / 32768.0, WAV_PCM_SCALE_POW2, dbl) == 0);
    }

    const float in8[4] = {-1.0f, 0.0f, 1.0f, 2.0f};
    CHECK(wav_f32_to_pcm(in8, out, 8, WAV_PCM_SCALE_POW2, 4, &err) == WAV_OK);
    CHECK(out[0] == 0 && out[1] == 128 && out[2] == 255 && out[3] == 255);
    CHECK(wav_f32_to_pcm(in8, out, 8, WAV_PCM_SCALE_MAX, 4, &err) == WAV_OK);
    CHECK(out[0] == 1 && out[1] == 128 && out[2] == 255 && out[3] == 255);

    const double in24[3] = {-1.0, 0.0, 1.0};
    CHECK(wav_f64_to_pcm(in24, out, 24, WAV_PCM_SCALE_POW2, 3, &err) == WAV_OK);
    CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x80);
    CHECK(out[3] == 0 && out[4] == 0 && out[5] == 0);
    CHECK(out[6] == 0xFF && out[7] == 0xFF && out[8] == 0x7F);

    CHECK(wav_f64_to_pcm(in24, out, 32, WAV_PCM_SCALE_POW2, 3, &err) == WAV_OK);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0x80);
    CHECK(out[8] == 0xFF && out[9] == 0xFF && out[10] == 0xFF && out[11] == 0x7F);

    const double in64[5] = {-1.0, 1.0, 2.0, -2.0, 0.5};
    uint8_t out64[40];
    CHECK(wav_f64_to_pcm(in64, out64, 64, WAV_PCM_SCALE_POW2, 5, &err) == WAV_OK);
    CHECK(out64[7] == 0x80 && out64[0] == 0);
    CHECK(out64[8] == 0xFF && out64[15] == 0x7F);
    CHECK(out64[16] == 0xFF && out64[23] == 0x7F);
    CHECK(out64[24] == 0x00 && out64[31] == 0x80);
    CHECK(out64[32 + 7] == 0x40 && out64[32 + 6] == 0 && out64[32] == 0);
    CHECK(wav_f64_to_pcm(&in64[1], out64, 64, WAV_PCM_SCALE_MAX, 1, &err) == WAV_OK && out64[7] == 0x7F && out64[0] == 0xFF);

    CHECK(wav_f64_to_pcm(&in64[1], out64, 40, WAV_PCM_SCALE_MAX, 1, &err) == WAV_OK);
    CHECK(out64[0] == 0xFF && out64[3] == 0xFF && out64[4] == 0x7F);

    const float in_wide_f32[3] = {NAN, -0.5f, 0.25f};
    CHECK(wav_f32_to_pcm(in_wide_f32, out64, 64, WAV_PCM_SCALE_POW2, 3, &err) == WAV_OK);
    for(int i = 0; i < 8; i++) {
        CHECK(out64[i] == 0);
    }
    CHECK(out64[8 + 7] == 0xC0 && out64[8 + 6] == 0);
    CHECK(out64[16 + 7] == 0x20 && out64[16 + 6] == 0);
    const double in_wide_f64[2] = {NAN, -0.5};
    CHECK(wav_f64_to_pcm(in_wide_f64, out64, 48, WAV_PCM_SCALE_POW2, 2, &err) == WAV_OK);
    CHECK(out64[0] == 0 && out64[5] == 0 && out64[6 + 5] == 0xC0);

    const double in12[1] = {0.5};
    uint8_t out12[2];
    uint8_t out16[2];
    CHECK(wav_f64_to_pcm(in12, out12, 12, WAV_PCM_SCALE_POW2, 1, &err) == WAV_OK);
    CHECK(wav_f64_to_pcm(in12, out16, 16, WAV_PCM_SCALE_POW2, 1, &err) == WAV_OK);
    CHECK(memcmp(out12, out16, 2) == 0);

    CHECK(wav_f32_to_pcm(NULL, NULL, 16, WAV_PCM_SCALE_POW2, 0, &err) == WAV_OK);
    CHECK(wav_f64_to_pcm(NULL, NULL, 16, WAV_PCM_SCALE_POW2, 0, &err) == WAV_OK);
    CHECK(wav_f64_to_pcm(in24, out, 65, WAV_PCM_SCALE_POW2, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_f32_to_pcm(in8, out, 16, (wav_pcm_scale)0, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_f64_to_pcm(in24, out, 0, WAV_PCM_SCALE_POW2, 3, NULL) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_f32_to_pcm(NULL, out, 16, WAV_PCM_SCALE_POW2, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_f64_to_pcm(in24, NULL, 16, WAV_PCM_SCALE_POW2, 3, &err) == WAV_ERROR_INVALID_ARGUMENT);
}

static void fill_pcm_pattern(uint8_t *pcm, uint16_t bits, uint64_t n) {
    size_t bytes = (size_t)(bits + 7) / 8;
    uint32_t x = 777;
    for(uint64_t i = 0; i < n; i++) {
        for(size_t b = 0; b < bytes; b++) {
            if(bits <= 16) {
                pcm[i * bytes + b] = (uint8_t)(i >> (8 * b));
            }
            else {
                x = x * 1664525u + 1013904223u;
                pcm[i * bytes + b] = (uint8_t)(x >> 24);
            }
        }
    }
    if(bits > 16) {
        memset(pcm, 0, bytes);
        pcm[bytes - 1] = 0x80;
        memset(pcm + bytes, 0xFF, bytes);
        pcm[2 * bytes - 1] = 0x7F;
        memset(pcm + 2 * bytes, 0xFF, bytes);
        memset(pcm + 3 * bytes, 0, bytes);
    }
}

static int64_t pcm_value(const uint8_t *p, size_t bytes) {
    uint64_t raw = 0;
    for(size_t b = 0; b < bytes; b++) {
        raw |= (uint64_t)p[b] << (8 * b);
    }
    uint64_t sign_bit = 1ULL << (bytes * 8 - 1);
    return (int64_t)((raw ^ sign_bit) - sign_bit);
}

static void test_pcm_float_roundtrip(void) {
    wav_error err;
    static uint8_t pcm_in[65536 * 8];
    static uint8_t pcm_out[65536 * 8];
    static float f[65536];
    static double d[65536];
    const wav_pcm_scale scales[2] = {WAV_PCM_SCALE_POW2, WAV_PCM_SCALE_MAX};
    const uint16_t depths[8] = {8, 16, 24, 32, 40, 48, 56, 64};

    for(int di = 0; di < 8; di++) {
        for(int si = 0; si < 2; si++) {
            uint16_t bits = depths[di];
            uint64_t n = (bits == 8) ? 256 : 65536;
            size_t bytes = bits / 8;
            fill_pcm_pattern(pcm_in, bits, n);

            memset(pcm_out, 0, sizeof(pcm_out));
            CHECK(wav_pcm_to_f64(pcm_in, bits, scales[si], d, n, &err) == WAV_OK);
            CHECK(wav_f64_to_pcm(d, pcm_out, bits, scales[si], n, &err) == WAV_OK);
            if(bits <= 48) {
                CHECK(memcmp(pcm_in, pcm_out, n * bytes) == 0);
            }
            else {
                int64_t tolerance = (int64_t)1 << (bits - 52);
                int64_t worst = 0;
                for(uint64_t i = 0; i < n; i++) {
                    int64_t a = pcm_value(pcm_in + i * bytes, bytes);
                    int64_t b = pcm_value(pcm_out + i * bytes, bytes);
                    int64_t diff = (a > b) ? (int64_t)((uint64_t)a - (uint64_t)b) : (int64_t)((uint64_t)b - (uint64_t)a);
                    worst = diff > worst ? diff : worst;
                }
                CHECK(worst <= tolerance);
            }

            bool float_is_exact = (scales[si] == WAV_PCM_SCALE_POW2) ? bits <= 24 : bits <= 16;
            if(float_is_exact) {
                memset(pcm_out, 0, sizeof(pcm_out));
                CHECK(wav_pcm_to_f32(pcm_in, bits, scales[si], f, n, &err) == WAV_OK);
                CHECK(wav_f32_to_pcm(f, pcm_out, bits, scales[si], n, &err) == WAV_OK);
                CHECK(memcmp(pcm_in, pcm_out, n * bytes) == 0);
            }
        }
    }

    fill_pcm_pattern(pcm_in, 32, 65536);
    CHECK(wav_pcm_to_f32(pcm_in, 32, WAV_PCM_SCALE_POW2, f, 65536, &err) == WAV_OK);
    CHECK(wav_f32_to_pcm(f, pcm_out, 32, WAV_PCM_SCALE_POW2, 65536, &err) == WAV_OK);
    int64_t worst = 0;
    for(uint64_t i = 0; i < 65536; i++) {
        int64_t diff = pcm_value(pcm_in + i * 4, 4) - pcm_value(pcm_out + i * 4, 4);
        diff = diff < 0 ? -diff : diff;
        worst = diff > worst ? diff : worst;
    }
    CHECK(worst <= 128);
}

static void test_interleave_roundtrip(void) {
    wav_error err;
    const size_t sizes[5] = {1, 2, 3, 4, 8};
    const uint16_t n_channels = 3;
    const uint64_t n_frames = 5;

    for(int si = 0; si < 5; si++) {
        size_t sample = sizes[si];
        uint8_t planar[3][5 * 8];
        uint8_t back[3][5 * 8];
        uint8_t interleaved[3 * 5 * 8];
        const void *in_planes[3];
        void *out_planes[3];

        for(int c = 0; c < n_channels; c++) {
            for(uint64_t i = 0; i < n_frames * sample; i++) {
                planar[c][i] = (uint8_t)(c * 100 + i);
            }
            memset(back[c], 0, sizeof(back[c]));
            in_planes[c] = planar[c];
            out_planes[c] = back[c];
        }

        CHECK(wav_interleave(in_planes, interleaved, n_channels, n_frames, sample, &err) == WAV_OK);
        for(int c = 0; c < n_channels; c++) {
            for(uint64_t i = 0; i < n_frames; i++) {
                CHECK(memcmp(interleaved + (i * n_channels + c) * sample, planar[c] + i * sample, sample) == 0);
            }
        }
        CHECK(wav_deinterleave(interleaved, out_planes, n_channels, n_frames, sample, &err) == WAV_OK);
        for(int c = 0; c < n_channels; c++) {
            CHECK(memcmp(back[c], planar[c], n_frames * sample) == 0);
        }
    }

    uint8_t a[4] = {0};
    const void *planes[2] = {a, a};
    void *out_planes[2] = {a, a};
    uint8_t dst[8];
    CHECK(wav_interleave(NULL, NULL, 2, 0, 4, &err) == WAV_OK);
    CHECK(wav_deinterleave(NULL, NULL, 2, 0, 4, &err) == WAV_OK);
    CHECK(wav_interleave(planes, dst, 0, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_interleave(planes, dst, 2, 1, 0, NULL) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_interleave(NULL, dst, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_interleave(planes, NULL, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    const void *with_null[2] = {a, NULL};
    CHECK(wav_interleave(with_null, dst, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(err.offset == 1);
    void *out_with_null[2] = {a, NULL};
    CHECK(wav_deinterleave(dst, out_with_null, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_deinterleave(NULL, out_planes, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_deinterleave(dst, NULL, 2, 1, 4, &err) == WAV_ERROR_INVALID_ARGUMENT);
}


typedef struct budget_io {
    mem *m;
    int reads_left;
    int writes_left;
} budget_io;

static int64_t budget_read(void *context, uint64_t offset, void *buffer, size_t size) {
    budget_io *b = (budget_io *)context;
    if(b->reads_left-- <= 0) {
        return -1;
    }
    return mem_read(b->m, offset, buffer, size);
}

static int64_t budget_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    budget_io *b = (budget_io *)context;
    if(b->writes_left-- <= 0) {
        return -1;
    }
    return mem_write(b->m, offset, buffer, size);
}

static void test_set_error_truncates_and_accepts_missing_message(void) {
    wav_error err;
    char long_message[300];
    memset(long_message, 'x', sizeof(long_message));
    wav_strview too_long = {long_message, sizeof(long_message)};

    CHECK(wav__set_error(&err, WAV_ERROR_IO, 7, too_long) == WAV_ERROR_IO);
    CHECK(err.code == WAV_ERROR_IO && err.offset == 7);
    CHECK(strlen(err.message) == sizeof(err.message) - 1);

    wav_strview none = {NULL, 5};
    CHECK(wav__set_error(&err, WAV_ERROR_IO, 0, none) == WAV_ERROR_IO);
    CHECK(err.message[0] == '\0');
    CHECK(wav__set_error(NULL, WAV_ERROR_IO, 0, none) == WAV_ERROR_IO);
}

static void test_write_primitives_reject_missing_io(void) {
    wav_error err;
    CHECK(wav_write_container(NULL, 0, WAV_CONTAINER_RIFF, 100, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_chunk_header(NULL, 0, WAV_FOURCC_DATA, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_chunk_pad(NULL, 0, 1, &err) == WAV_ERROR_INVALID_ARGUMENT);
    CHECK(wav_write_data_frames(NULL, 0, &(wav_format){.block_align = 2}, "ab", 1, &err) == WAV_ERROR_INVALID_ARGUMENT);

    wav_io io = fresh_io();
    CHECK(wav_write_container(&io, 0, WAV_CONTAINER_RIFF, 4, &err) == WAV_OK);
    CHECK(u32_at(4) == 0);
}

static void test_write_header_fails_cleanly_at_every_io_step(void) {
    wav_format formats[3] = {pcm(2, 48000, 16), make_format(WAV_ENCODING_IEEE_FLOAT, 2, 48000, 32), pcm(6, 48000, 24)};
    wav_write_mode modes[3] = {WAV_WRITE_RIFF, WAV_WRITE_RF64, WAV_WRITE_AUTO};

    for(int fi = 0; fi < 3; fi++) {
        for(int mi = 0; mi < 3; mi++) {
            bool completed = false;
            for(int budget = 0; budget < 20 && !completed; budget++) {
                fresh_io();
                budget_io b = {&g, 0, budget};
                wav_io io = {&b, budget_read, budget_write};
                wav_error err;
                wav_result res = wav_write_header(&io, 0, modes[mi], &formats[fi], 10, NULL, &err);
                if(res == WAV_OK) {
                    completed = true;
                }
                else {
                    CHECK(res == WAV_ERROR_IO);
                }
            }
            CHECK(completed);
        }
    }
}

static void build_rf64_file(const wav_io *io, uint64_t *end) {
    wav_error err;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    uint8_t ds64[WAV_DS64_PAYLOAD_SIZE] = {0};
    const uint8_t samples[4] = {1, 0, 2, 0};
    ds64[8] = 4;
    wav_write_container(io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
    put_chunk(io, &at, WAV_FOURCC_DS64, ds64, sizeof(ds64));
    put_chunk(io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
    put_chunk(io, &at, WAV_FOURCC_DATA, samples, 4);
    *end = at;
}

static void test_parse_fails_cleanly_at_every_io_step(void) {
    wav_io io = fresh_io();
    uint64_t end = 0;
    build_rf64_file(&io, &end);

    bool completed = false;
    for(int budget = 0; budget < 30 && !completed; budget++) {
        budget_io b = {&g, budget, 0};
        wav_io limited = {&b, budget_read, budget_write};
        wav_stream_info si;
        wav_error err;
        wav_result res = wav_parse_stream_info(&limited, end, &si, &err);
        if(res == WAV_OK) {
            completed = true;
            CHECK(si.container == WAV_CONTAINER_RF64 && si.n_frames == 2);
        }
        else {
            CHECK(res == WAV_ERROR_IO);
        }
    }
    CHECK(completed);
}

static void test_parse_reports_chunks_missing_their_payload(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;

    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    wav_write_chunk_header(&io, WAV_CONTAINER_HEADER_SIZE, WAV_FOURCC_FMT, 16, &err);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_UNEXPECTED_EOF);

    io = fresh_io();
    wav_write_container(&io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
    wav_write_chunk_header(&io, WAV_CONTAINER_HEADER_SIZE, WAV_FOURCC_DS64, WAV_DS64_PAYLOAD_SIZE, &err);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_UNEXPECTED_EOF);
}

static void test_parse_rejects_malformed_fmt_fields(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;
    uint64_t at = WAV_CONTAINER_HEADER_SIZE;
    const uint8_t samples[4] = {1, 0, 2, 0};

    uint8_t extensible_no_cb[40] = {0};
    memcpy(extensible_no_cb, pcm16_mono_8k_fmt, 16);
    extensible_no_cb[0] = 0xFE;
    extensible_no_cb[1] = 0xFF;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, extensible_no_cb, 40);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);

    uint8_t zero_bits[16];
    memcpy(zero_bits, pcm16_mono_8k_fmt, 16);
    zero_bits[14] = 0;
    io = fresh_io();
    at = WAV_CONTAINER_HEADER_SIZE;
    wav_write_container(&io, 0, WAV_CONTAINER_RIFF, WAV_UNKNOWN_SIZE, &err);
    put_chunk(&io, &at, WAV_FOURCC_FMT, zero_bits, 16);
    put_chunk(&io, &at, WAV_FOURCC_DATA, samples, 4);
    CHECK(wav_parse_stream_info(&io, 0, &si, &err) == WAV_ERROR_INVALID_FORMAT);
}

static void test_extensible_decision_and_guid_kinds(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;

    wav_format valid12 = pcm(2, 48000, 16);
    valid12.valid_bits_per_sample = 12;
    CHECK(wav_header_size(WAV_WRITE_RIFF, &valid12) == 12 + 8 + 40 + 8);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &valid12, 4, NULL, &err) == WAV_OK);
    CHECK(wav_parse_stream_info(&io, 68 + 16, &si, &err) == WAV_OK && si.format.valid_bits_per_sample == 12);

    wav_format valid16 = pcm(2, 48000, 16);
    valid16.valid_bits_per_sample = 16;
    CHECK(wav_header_size(WAV_WRITE_RIFF, &valid16) == 44);

    wav_format float_guid = make_format(WAV_ENCODING_IEEE_FLOAT, 1, 48000, 32);
    float_guid.subformat_guid[0] = 3;
    memcpy(float_guid.subformat_guid + 4, "\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 12);
    CHECK(wav_header_size(WAV_WRITE_RIFF, &float_guid) == 12 + 8 + 40 + 8 + 4 + 8);

    wav_format float32 = make_format(WAV_ENCODING_IEEE_FLOAT, 1, 48000, 32);
    CHECK(wav_format_check(&float32).data == NULL);
}

static void test_fact_chunk_saturates_for_huge_frame_counts(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = make_format(WAV_ENCODING_IEEE_FLOAT, 2, 48000, 32);

    CHECK(wav_write_header(&io, 0, WAV_WRITE_RF64, &f, 5000000000ULL, NULL, &err) == WAV_OK);
    CHECK(u32_at(12 + 8 + WAV_DS64_PAYLOAD_SIZE + 8 + 18 + 8) == WAV_SIZE32_SENTINEL);
}

static void test_read_past_end_of_unsized_stream(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_format f = pcm(1, 8000, 16);
    wav_stream_info si;
    int16_t out[4];

    wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, WAV_UNKNOWN_FRAMES, NULL, &err);
    CHECK(wav_parse_stream_info(&io, WAV_UNKNOWN_SIZE, &si, &err) == WAV_OK);
    CHECK(wav_read_data_frames(&io, &si, 1000, out, 4, &err) == WAV_ERROR_UNEXPECTED_EOF);
}


static void test_zero_byte_rate_is_derived_when_possible(void) {
    wav_io io = fresh_io();
    wav_error err;
    wav_stream_info si;

    wav_format f = pcm(2, 48000, 16);
    f.byte_rate = 0;
    CHECK(wav_format_check(&f).data == NULL);
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, 10, NULL, &err) == WAV_OK);
    CHECK(u32_at(28) == 192000);
    CHECK(wav_parse_stream_info(&io, 44 + 40, &si, &err) == WAV_OK && si.format.byte_rate == 192000);

    wav_format huge = pcm(1, 0xFFFFFFFFu, 32);
    huge.byte_rate = 0;
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &huge, 1, NULL, &err) == WAV_OK);
    CHECK(u32_at(28) == 0xFFFFFFFFu);

    wav_format compressed = make_format(WAV_ENCODING_UNKNOWN, 1, 8000, 4);
    compressed.format_tag = 2;
    compressed.block_align = 256;
    compressed.byte_rate = 0;
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &compressed, 1, NULL, &err) == WAV_OK);
    CHECK(u32_at(28) == 0);

    wav_format given = pcm(2, 48000, 16);
    given.byte_rate = 7;
    io = fresh_io();
    CHECK(wav_write_header(&io, 0, WAV_WRITE_RIFF, &given, 1, NULL, &err) == WAV_OK);
    CHECK(u32_at(28) == 7);
}

static void test_rf64_trusts_ds64_for_the_data_size(void) {
    wav_error err;
    wav_stream_info si;
    const uint8_t samples[8] = {1, 0, 2, 0, 3, 0, 4, 0};
    const uint64_t declared_in_header[3] = {0, 100, WAV_SIZE32_SENTINEL};

    for(int i = 0; i < 3; i++) {
        wav_io io = fresh_io();
        uint64_t at = WAV_CONTAINER_HEADER_SIZE;
        uint8_t ds64[WAV_DS64_PAYLOAD_SIZE] = {0};
        ds64[8] = 8;
        wav_write_container(&io, 0, WAV_CONTAINER_RF64, WAV_UNKNOWN_SIZE, &err);
        put_chunk(&io, &at, WAV_FOURCC_DS64, ds64, sizeof(ds64));
        put_chunk(&io, &at, WAV_FOURCC_FMT, pcm16_mono_8k_fmt, 16);
        wav_write_chunk_header(&io, at, WAV_FOURCC_DATA, declared_in_header[i], &err);
        mem_write(&g, at + 8, samples, 8);

        CHECK(wav_parse_stream_info(&io, at + 8 + 8, &si, &err) == WAV_OK);
        CHECK(si.data_size == 8 && si.n_frames == 4 && !si.data_truncated);
    }
}

static int64_t endless_chunk_read(void *context, uint64_t offset, void *buffer, size_t size) {
    static const uint8_t header[8] = {'d', 'a', 't', 'a', 0xFF, 0xFF, 0xFF, 0xFF};
    (void)context;
    for(size_t i = 0; i < size; i++) {
        ((uint8_t *)buffer)[i] = header[(offset + i) % 8];
    }
    return (int64_t)size;
}

static void test_chunk_walk_rejects_offset_overflow(void) {
    wav_io io = {NULL, endless_chunk_read, NULL};
    wav_error err;
    wav_chunk chunk;

    uint64_t offset = 0;
    CHECK(wav_read_chunk_header(&io, &offset, &chunk, &err) == WAV_OK);
    CHECK(chunk.size == WAV_SIZE32_SENTINEL && offset == 8 + 0xFFFFFFFFULL + 1);

    offset = UINT64_MAX - 7 - 8;
    uint64_t before = offset;
    CHECK(wav_read_chunk_header(&io, &offset, &chunk, &err) == WAV_ERROR_INVALID_FORMAT);
    CHECK(offset == before);

    offset = 0xFFFFFFFFFFFFFFF0ULL;
    CHECK(wav_read_chunk_header(&io, &offset, &chunk, &err) == WAV_ERROR_INVALID_FORMAT);
}

int main(void) {
    test_container_primitives();
    test_container_read_errors();
    test_chunk_primitives();
    test_riff_pcm_layout_and_roundtrip();
    test_odd_data_size_pad();
    test_auto_known_small_is_riff_with_junk();
    test_streaming_unknown_size();
    test_auto_and_riff_size_limits();
    test_float_has_fact_and_fmt18();
    test_alaw_mulaw();
    test_extensible_roundtrip();
    test_custom_tag_and_guid();
    test_header_size_matches_bytes_written();
    test_header_at_nonzero_offset();
    test_write_header_argument_errors();
    test_inconsistent_fields_are_trusted_not_rejected();
    test_format_check();
    test_read_and_write_frames();
    test_short_io_reads_are_looped();
    test_truncated_file();
    test_parse_skips_unknown_chunks();
    test_parse_errors();
    test_unknown_tag_in_parsed_plain_fmt();
    test_ds64_data_size_used_for_rf64();
    test_error_struct_is_filled();
    test_auto_pipeline_reserve_stream_then_header();
    test_auto_switches_to_rf64_on_a_sparse_virtual_file();
    test_full_scale_is_visible();
    test_pcm_to_float_values();
    test_float_to_pcm_values();
    test_pcm_float_roundtrip();
    test_interleave_roundtrip();
    test_set_error_truncates_and_accepts_missing_message();
    test_write_primitives_reject_missing_io();
    test_write_header_fails_cleanly_at_every_io_step();
    test_parse_fails_cleanly_at_every_io_step();
    test_parse_reports_chunks_missing_their_payload();
    test_parse_rejects_malformed_fmt_fields();
    test_extensible_decision_and_guid_kinds();
    test_fact_chunk_saturates_for_huge_frame_counts();
    test_read_past_end_of_unsized_stream();
    test_zero_byte_rate_is_derived_when_possible();
    test_rf64_trusts_ds64_for_the_data_size();
    test_chunk_walk_rejects_offset_overflow();

    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
