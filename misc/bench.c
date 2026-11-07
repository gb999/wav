#define WAV_IMPLEMENTATION
#include "wav.h"
#include "wav_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_SECONDS 0.3

static volatile uint64_t sink;

static double now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void report(const char *name, double seconds, uint64_t iterations, double bytes_per_iteration) {
    double ns_per_call = seconds * 1e9 / (double)iterations;
    if(bytes_per_iteration > 0) {
        printf("%-34s %10.1f ns/call %10.0f MB/s\n", name, ns_per_call, bytes_per_iteration * (double)iterations / seconds / 1e6);
    }
    else {
        printf("%-34s %10.1f ns/call\n", name, ns_per_call);
    }
}

#define BENCH(name, bytes_per_iteration, batch, body) \
    do { \
        uint64_t iterations = 0; \
        double start = now(); \
        double elapsed; \
        do { \
            for(int batch_i = 0; batch_i < (batch); batch_i++) { \
                body; \
            } \
            iterations += (batch); \
            elapsed = now() - start; \
        } while(elapsed < MIN_SECONDS); \
        report(name, elapsed, iterations, bytes_per_iteration); \
    } while(0)

typedef struct memory {
    uint8_t *data;
    uint64_t size;
} memory;

static int64_t memory_read(void *context, uint64_t offset, void *buffer, size_t size) {
    memory *m = (memory *)context;
    if(offset >= m->size) {
        return 0;
    }
    uint64_t available = m->size - offset;
    size_t n = (size < available) ? size : (size_t)available;
    memcpy(buffer, m->data + offset, n);
    return (int64_t)n;
}

static int64_t memory_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    memory *m = (memory *)context;
    if(offset + size > m->size) {
        return -1;
    }
    memcpy(m->data + offset, buffer, size);
    return (int64_t)size;
}

static int64_t file_read(void *context, uint64_t offset, void *buffer, size_t size) {
    FILE *f = (FILE *)context;
    if(fseek(f, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    return (int64_t)fread(buffer, 1, size, f);
}

static void fill_random(uint8_t *p, size_t n) {
    uint32_t x = 12345;
    for(size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

static wav_format make_pcm(uint16_t channels, uint32_t rate, uint16_t bits) {
    wav_format f = {0};
    f.encoding = WAV_ENCODING_PCM;
    f.channels = channels;
    f.sample_rate = rate;
    f.bits_per_sample = bits;
    f.block_align = (uint16_t)(channels * (bits / 8));
    f.byte_rate = rate * f.block_align;
    return f;
}

static void bench_headers(void) {
    uint8_t header_bytes[256];
    memory m = {header_bytes, sizeof(header_bytes)};
    wav_io io = {&m, memory_read, memory_write};
    wav_format pcm16 = make_pcm(2, 48000, 16);
    wav_format pcm24_surround = make_pcm(6, 48000, 24);
    wav_error err;
    wav_stream_info si;

    BENCH("write_header RIFF pcm16", 0, 1000, sink += wav_write_header(&io, 0, WAV_WRITE_RIFF, &pcm16, 1000, NULL, &err));
    BENCH("write_header AUTO pcm16", 0, 1000, sink += wav_write_header(&io, 0, WAV_WRITE_AUTO, &pcm16, 1000, NULL, &err));
    BENCH("write_header AUTO extensible 5.1", 0, 1000, sink += wav_write_header(&io, 0, WAV_WRITE_AUTO, &pcm24_surround, 1000, NULL, &err));

    wav_write_header(&io, 0, WAV_WRITE_AUTO, &pcm16, 1000, NULL, &err);
    BENCH("parse_stream_info RIFF+JUNK", 0, 1000, sink += wav_parse_stream_info(&io, 80 + 4000, &si, &err));
    wav_write_header(&io, 0, WAV_WRITE_RF64, &pcm24_surround, 1000, NULL, &err);
    BENCH("parse_stream_info RF64 extensible", 0, 1000, sink += wav_parse_stream_info(&io, 104 + 18000, &si, &err));
}

static void bench_frame_io(void) {
    const uint64_t frames = 4u * 1024 * 1024;
    wav_format f = make_pcm(2, 48000, 16);
    uint64_t header = wav_header_size(WAV_WRITE_RIFF, &f);
    uint64_t data_bytes = frames * f.block_align;
    uint8_t *file = malloc(header + data_bytes);
    uint8_t *chunk = malloc(65536 * f.block_align);
    memory m = {file, header + data_bytes};
    wav_io io = {&m, memory_read, memory_write};
    wav_error err;
    wav_stream_info si;

    fill_random(file, header + data_bytes);
    wav_write_header(&io, 0, WAV_WRITE_RIFF, &f, frames, NULL, &err);
    wav_parse_stream_info(&io, m.size, &si, &err);

    const uint64_t chunk_frames[3] = {64, 4096, 65536};
    for(int i = 0; i < 3; i++) {
        char name[64];
        uint64_t n = chunk_frames[i];
        uint64_t calls_per_pass = frames / n;
        uint64_t pass = 0;
        snprintf(name, sizeof(name), "read_data_frames mem  %6llu frames", (unsigned long long)n);
        BENCH(name, (double)n * f.block_align, 256, {
            sink += wav_read_data_frames(&io, &si, (pass++ % calls_per_pass) * n, chunk, n, &err);
        });
    }
    for(int i = 0; i < 3; i++) {
        char name[64];
        uint64_t n = chunk_frames[i];
        uint64_t calls_per_pass = frames / n;
        uint64_t pass = 0;
        snprintf(name, sizeof(name), "write_data_frames mem %6llu frames", (unsigned long long)n);
        BENCH(name, (double)n * f.block_align, 256, {
            sink += wav_write_data_frames(&io, header + (pass++ % calls_per_pass) * n * f.block_align, &f, chunk, n, &err);
        });
    }
    BENCH("memcpy baseline 64 KiB", 65536, 4096, { memcpy(chunk, file + (sink & 0xFFFF), 65536); sink += chunk[0]; });

    FILE *tmp = tmpfile();
    if(tmp) {
        if(fwrite(file, 1, m.size, tmp) == m.size) {
            wav_io file_io = {tmp, file_read, NULL};
            uint64_t n = 4096;
            uint64_t calls_per_pass = frames / n;
            uint64_t pass = 0;
            BENCH("read_data_frames stdio   4096 frames", (double)n * f.block_align, 64, {
                sink += wav_read_data_frames(&file_io, &si, (pass++ % calls_per_pass) * n, chunk, n, &err);
            });
        }
        fclose(tmp);
    }

    free(chunk);
    free(file);
}

static void bench_conversion(void) {
    const uint64_t samples = 65536;
    uint8_t *pcm = malloc(samples * 4);
    uint8_t *pcm_out = malloc(samples * 4);
    float *f = malloc(samples * sizeof(float));
    double *d = malloc(samples * sizeof(double));
    wav_error err;

    fill_random(pcm, samples * 4);
    const uint16_t depths[4] = {8, 16, 24, 32};
    for(int i = 0; i < 4; i++) {
        char name[64];
        uint16_t bits = depths[i];
        double bytes = (double)samples * bits / 8;
        snprintf(name, sizeof(name), "pcm%u -> f32", bits);
        BENCH(name, bytes, 16, sink += wav_pcm_to_f32(pcm, bits, WAV_PCM_SCALE_POW2, f, samples, &err));
        snprintf(name, sizeof(name), "f32 -> pcm%u", bits);
        BENCH(name, bytes, 16, sink += wav_f32_to_pcm(f, pcm_out, bits, WAV_PCM_SCALE_POW2, samples, &err));
        snprintf(name, sizeof(name), "pcm%u -> f64", bits);
        BENCH(name, bytes, 16, sink += wav_pcm_to_f64(pcm, bits, WAV_PCM_SCALE_POW2, d, samples, &err));
        snprintf(name, sizeof(name), "f64 -> pcm%u", bits);
        BENCH(name, bytes, 16, sink += wav_f64_to_pcm(d, pcm_out, bits, WAV_PCM_SCALE_POW2, samples, &err));
    }

    free(d);
    free(f);
    free(pcm_out);
    free(pcm);
}

static void bench_interleave(void) {
    const uint64_t frames = 65536;
    const size_t sizes[3] = {2, 3, 4};
    uint16_t channel_counts[2] = {2, 6};
    wav_error err;

    for(int ci = 0; ci < 2; ci++) {
        for(int si = 0; si < 3; si++) {
            uint16_t channels = channel_counts[ci];
            size_t sample = sizes[si];
            uint8_t *planar = malloc(frames * channels * sample);
            uint8_t *interleaved = malloc(frames * channels * sample);
            const void *in_planes[6];
            void *out_planes[6];
            char name[64];
            for(int c = 0; c < channels; c++) {
                in_planes[c] = planar + (size_t)c * frames * sample;
                out_planes[c] = planar + (size_t)c * frames * sample;
            }
            fill_random(planar, frames * channels * sample);
            fill_random(interleaved, frames * channels * sample);

            double bytes = (double)frames * channels * sample;
            snprintf(name, sizeof(name), "interleave   %uch x %zu-byte", channels, sample);
            BENCH(name, bytes, 16, sink += wav_interleave(in_planes, interleaved, channels, frames, sample, &err));
            snprintf(name, sizeof(name), "deinterleave %uch x %zu-byte", channels, sample);
            BENCH(name, bytes, 16, sink += wav_deinterleave(interleaved, out_planes, channels, frames, sample, &err));

            free(interleaved);
            free(planar);
        }
    }
}

int main(void) {
#ifdef __VERSION__
    printf("build: %s\n\n", __VERSION__);
#endif
    bench_headers();
    printf("\n");
    bench_frame_io();
    printf("\n");
    bench_conversion();
    printf("\n");
    bench_interleave();
    return (int)(sink & 0);
}
