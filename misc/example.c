#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_IMPLEMENTATION
#include "wav.h"
#include "wav_convert.h"

#include "example_platform.h"

#define PI 3.14159265358979323846

// 0. Simple Read

void example_simple_read(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_stream_info stream;
    wav_error err;

    if(wav_parse_stream_info(&io, plat_size(&file), &stream, &err) != WAV_OK) {
        printf("Parse error: %s\n", err.message);
        plat_close(&file);
        return;
    }

    printf("Channels: %u, Sample Rate: %u, Frames: %llu\n",
           stream.format.channels, stream.format.sample_rate, (unsigned long long)stream.n_frames);

    uint64_t chunk_size = 1024;
    int16_t *buffer = malloc(chunk_size * stream.format.block_align);
    uint64_t cur_frame = 0;

    // Exact read contract: clamp with min() so we never request past stream.n_frames
    while(cur_frame < stream.n_frames) {
        uint64_t remaining = stream.n_frames - cur_frame;
        uint64_t to_read = (remaining < chunk_size) ? remaining : chunk_size;

        if(wav_read_data_frames(&io, &stream, cur_frame, buffer, to_read, &err) != WAV_OK) {
            printf("Read error at frame %llu: %s\n", (unsigned long long)cur_frame, err.message);
            break;
        }

        cur_frame += to_read;
    }

    free(buffer);
    plat_close(&file);
}

// 1. Write Sine Wave: 220 Hz, 44100 Fs, 16-bit Mono, 2 Seconds
//    Pattern: Reserve header space -> stream frames -> write header ONCE at end.

void example_write_sine_wave(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, true)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_error err;

    wav_format format;
    memset(&format, 0, sizeof(format));
    format.encoding = WAV_ENCODING_PCM;
    format.channels = 1;
    format.sample_rate = 44100;
    format.bits_per_sample = 16;
    format.block_align = 2;
    format.byte_rate = 44100 * 2;

    uint64_t n_frames = 44100 * 2; // 2 seconds

    // 1. Calculate reserved header space upfront (80 bytes for AUTO)
    uint64_t header_bytes = wav_header_size(WAV_WRITE_AUTO, &format);

    int16_t buffer[1024];
    uint64_t cur_frame = 0;
    double phase = 0.0;
    double phase_inc = (2.0 * PI * 220.0) / 44100.0;

    // 2. Stream audio samples directly into storage starting at byte 80
    while(cur_frame < n_frames) {
        uint64_t remaining = n_frames - cur_frame;
        uint64_t to_write = (remaining < 1024) ? remaining : 1024;

        for(uint64_t i = 0; i < to_write; ++i) {
            buffer[i] = (int16_t)(sin(phase) * 32767.0);
            phase += phase_inc;
            if(phase >= 2.0 * PI) {
                phase -= 2.0 * PI;
            }
        }

        uint64_t offset = header_bytes + (cur_frame * format.block_align);
        wav_write_data_frames(&io, offset, &format, buffer, to_write, &err);
        cur_frame += to_write;
    }

    // 3. Write final header EXACTLY ONCE at offset 0
    wav_write_header(&io, 0, WAV_WRITE_AUTO, &format, n_frames, NULL, &err);

    plat_close(&file);
}

// 2. Multi-Threaded Read (Concurrent Reads Across Worker Threads)

typedef struct thread_read_job {
    wav_io io;
    wav_stream_info stream;
    uint64_t start_frame;
    uint64_t n_frames;
    double sum;
} thread_read_job;

static void *read_worker(void *arg) {
    thread_read_job *job = (thread_read_job *)arg;
    int16_t *buffer = malloc(job->n_frames * job->stream.format.block_align);
    wav_error err;

    // Lock-free concurrent read: threads read non-overlapping frame slices at explicit offsets
    if(wav_read_data_frames(&job->io, &job->stream, job->start_frame, buffer, job->n_frames, &err) == WAV_OK) {
        for(uint64_t i = 0; i < job->n_frames; ++i) {
            job->sum += buffer[i];
        }
    }

    free(buffer);
    return NULL;
}

void example_multithreaded_read(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_stream_info stream;
    wav_error err;

    if(wav_parse_stream_info(&io, plat_size(&file), &stream, &err) != WAV_OK) {
        plat_close(&file);
        return;
    }

    plat_thread threads[2];
    thread_read_job jobs[2];
    uint64_t half = stream.n_frames / 2;

    jobs[0] = (thread_read_job){ .io = io, .stream = stream, .start_frame = 0, .n_frames = half, .sum = 0.0 };
    jobs[1] = (thread_read_job){ .io = io, .stream = stream, .start_frame = half, .n_frames = stream.n_frames - half, .sum = 0.0 };

    plat_thread_start(&threads[0], read_worker, &jobs[0]);
    plat_thread_start(&threads[1], read_worker, &jobs[1]);

    plat_thread_join(&threads[0]);
    plat_thread_join(&threads[1]);

    printf("Parallel read completed. Sum: %f\n", jobs[0].sum + jobs[1].sum);
    plat_close(&file);
}

// 3. Multi-Threaded Write (Workers Write Non-Overlapping Blocks Concurrently)

typedef struct thread_write_job {
    wav_io io;
    wav_format format;
    uint64_t byte_offset;
    uint64_t n_frames;
    double frequency;
} thread_write_job;

static void *write_worker(void *arg) {
    thread_write_job *job = (thread_write_job *)arg;
    float *buffer = malloc(job->n_frames * job->format.block_align);
    wav_error err;

    double phase = 0.0;
    double phase_inc = (2.0 * PI * job->frequency) / job->format.sample_rate;

    for(uint64_t i = 0; i < job->n_frames; ++i) {
        buffer[i] = (float)sin(phase);
        phase += phase_inc;
    }

    // Workers write concurrently into their reserved slice of the same file
    wav_write_data_frames(&job->io, job->byte_offset, &job->format, buffer, job->n_frames, &err);

    free(buffer);
    return NULL;
}

void example_multithreaded_write(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, true)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_error err;

    wav_format format;
    memset(&format, 0, sizeof(format));
    format.encoding = WAV_ENCODING_IEEE_FLOAT;
    format.channels = 1;
    format.sample_rate = 48000;
    format.bits_per_sample = 32;
    format.block_align = 4;
    format.byte_rate = 48000 * 4;

    uint64_t total_frames = 48000 * 4; // 4 seconds
    uint64_t header_bytes = wav_header_size(WAV_WRITE_AUTO, &format);

    plat_thread threads[2];
    thread_write_job jobs[2];
    uint64_t half = total_frames / 2;

    jobs[0] = (thread_write_job){ .io = io, .format = format, .byte_offset = header_bytes, .n_frames = half, .frequency = 440.0 };
    jobs[1] = (thread_write_job){ .io = io, .format = format, .byte_offset = header_bytes + (half * format.block_align), .n_frames = total_frames - half, .frequency = 880.0 };

    plat_thread_start(&threads[0], write_worker, &jobs[0]);
    plat_thread_start(&threads[1], write_worker, &jobs[1]);

    plat_thread_join(&threads[0]);
    plat_thread_join(&threads[1]);

    // Write header once at the end with exact frames
    wav_write_header(&io, 0, WAV_WRITE_AUTO, &format, total_frames, NULL, &err);

    plat_close(&file);
}

// 4. Memory-Mapped Reader (Zero-Copy Sample Processing)

typedef struct mem_context {
    const uint8_t *data;
    size_t size;
} mem_context;

static int64_t mem_read(void *context, uint64_t offset, void *buffer, size_t size) {
    mem_context *m = (mem_context *)context;
    if(offset >= m->size) {
        return 0;
    }
    size_t avail = m->size - offset;
    size_t to_copy = (size < avail) ? size : avail;
    memcpy(buffer, m->data + offset, to_copy);
    return (int64_t)to_copy;
}

void example_mmapped_reader(const char *path) {
    plat_map map;
    if(!plat_map_open(&map, path)) {
        return;
    }

    mem_context mctx = { .data = map.data, .size = (size_t)map.size };
    wav_io io = { .context = &mctx, .read = mem_read, .write = NULL };

    wav_stream_info stream;
    wav_error err;

    if(wav_parse_stream_info(&io, map.size, &stream, &err) == WAV_OK) {
        // Direct zero-copy: samples pointer is calculated directly via data_offset
        const float *samples = (const float *)(map.data + stream.data_offset);

        float peak = 0.0f;
        for(uint64_t i = 0; i < stream.n_frames; ++i) {
            float val = fabsf(samples[i]);
            if(val > peak) {
                peak = val;
            }
        }
        printf("Zero-copy processing finished. Peak: %f\n", peak);
    }

    plat_map_close(&map);
}

// 5. Inspecting Chunks Manually (Using wav_read_chunk_header)

void example_inspect_chunks(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_container container;
    wav_error err;

    if(wav_read_container(&io, &container, &err) != WAV_OK) {
        plat_close(&file);
        return;
    }

    // Subchunks in RIFF/RF64 begin right after the container header
    uint64_t offset = WAV_CONTAINER_HEADER_SIZE;
    wav_chunk chunk;

    // Pure, literal chunk header reader: walks chunks one by one
    while(wav_read_chunk_header(&io, &offset, &chunk, &err) == WAV_OK) {
        printf("Chunk '%.4s' (ID 0x%08X) at payload offset %llu, size %llu\n",
               chunk.tag, chunk.id, (unsigned long long)chunk.offset, (unsigned long long)chunk.size);

        if(chunk.id == WAV_FOURCC_BEXT) {
            // Read broadcast metadata directly from chunk.offset into our own buffer
            char bext_data[512];
            size_t to_read = (chunk.size < sizeof(bext_data)) ? (size_t)chunk.size : sizeof(bext_data);
            io.read(io.context, chunk.offset, bext_data, to_read);
        }
    }

    plat_close(&file);
}

// 6. Convenience: PCM -> float, deinterleave (wav_convert.h)

void example_convert(const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return;
    }

    wav_io io = plat_io(&file);
    wav_stream_info stream;
    wav_error err;

    if(wav_parse_stream_info(&io, plat_size(&file), &stream, &err) != WAV_OK || stream.format.encoding != WAV_ENCODING_PCM) {
        plat_close(&file);
        return;
    }

    uint64_t n_frames = stream.n_frames < 1024 ? stream.n_frames : 1024;
    uint16_t channels = stream.format.channels;
    uint16_t bits = stream.format.bits_per_sample;
    void *raw = malloc(n_frames * stream.format.block_align);
    float *interleaved = malloc(n_frames * channels * sizeof(float));
    float *planar = malloc(n_frames * channels * sizeof(float));
    float **planes = malloc(channels * sizeof(float *));
    for(uint16_t c = 0; c < channels; c++) {
        planes[c] = planar + c * n_frames;
    }

    if(wav_read_data_frames(&io, &stream, 0, raw, n_frames, &err) == WAV_OK &&
       wav_pcm_to_f32(raw, bits, WAV_PCM_SCALE_POW2, interleaved, n_frames * channels, &err) == WAV_OK &&
       wav_deinterleave(interleaved, (void *const *)planes, channels, n_frames, sizeof(float), &err) == WAV_OK) {
        printf("First sample of channel 0 as float: %f\n", n_frames > 0 ? planes[0][0] : 0.0f);
    }

    free(planes);
    free(planar);
    free(interleaved);
    free(raw);
    plat_close(&file);
}

int main(void) {
    example_write_sine_wave("sine220.wav");
    example_simple_read("sine220.wav");
    example_multithreaded_read("sine220.wav");
    example_multithreaded_write("parallel_synth.wav");
    example_mmapped_reader("parallel_synth.wav");
    example_inspect_chunks("parallel_synth.wav");
    example_convert("sine220.wav");
    return 0;
}
