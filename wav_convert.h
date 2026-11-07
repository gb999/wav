#ifndef WAV_CONVERT_H
#define WAV_CONVERT_H

// Optional helpers on top of wav.h.
//
// PCM <-> float. bits_per_sample is the wav_format value, 1..64. A sample is packed little-endian in ceil(bits / 8) bytes,
// left-justified (12-bit converts as 16, 20 as 24). 8-bit is unsigned, wider is signed. 64-bit loses its low bits to double.
// Encoding rounds to nearest, clamps, maps NaN to 0 and does not dither.
// n_samples counts samples (n_frames * channels). Interleave / deinterleave take sample_size in bytes.

#include "wav.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wav_pcm_scale {
    // 1.0 <-> 2^(n-1) where n is the container size in bits. Encoding +1.0 clamps to the largest value.
    WAV_PCM_SCALE_POW2 = 1,
    // 1.0 <-> 2^(n-1) - 1. Decoding the smallest value gives just below -1.0. Same as POW2 for 64-bit, double cannot tell them apart.
    WAV_PCM_SCALE_MAX
} wav_pcm_scale;

// the number a sample is divided by when decoding. 0.0 if bits_per_sample is not 1..64 or scale is not a wav_pcm_scale.
WAV_IFC double wav_pcm_full_scale(uint16_t bits_per_sample, wav_pcm_scale scale);
WAV_IFC wav_result wav_pcm_to_f32(const void *src, uint16_t bits_per_sample, wav_pcm_scale scale, float *dst, uint64_t n_samples, wav_error *error);
WAV_IFC wav_result wav_pcm_to_f64(const void *src, uint16_t bits_per_sample, wav_pcm_scale scale, double *dst, uint64_t n_samples, wav_error *error);
WAV_IFC wav_result wav_f32_to_pcm(const float *src, void *dst, uint16_t bits_per_sample, wav_pcm_scale scale, uint64_t n_samples, wav_error *error);
WAV_IFC wav_result wav_f64_to_pcm(const double *src, void *dst, uint16_t bits_per_sample, wav_pcm_scale scale, uint64_t n_samples, wav_error *error);
WAV_IFC wav_result wav_interleave(const void *const *channels, void *dst, uint16_t n_channels, uint64_t n_frames, size_t sample_size, wav_error *error);
WAV_IFC wav_result wav_deinterleave(const void *src, void *const *channels, uint16_t n_channels, uint64_t n_frames, size_t sample_size, wav_error *error);

#ifdef __cplusplus
}
#endif

#endif // WAV_CONVERT_H

#if defined(WAV_IMPLEMENTATION) && !defined(WAV_CONVERT_IMPLEMENTATION_DONE)
#define WAV_CONVERT_IMPLEMENTATION_DONE

static unsigned wav__container_bytes(uint16_t bits_per_sample) {
    return ((unsigned)bits_per_sample + 7) / 8;
}

WAV_IFC double wav_pcm_full_scale(uint16_t bits_per_sample, wav_pcm_scale scale) {
    if(bits_per_sample == 0 || bits_per_sample > 64) {
        return 0.0;
    }
    double half_range = (double)(1ULL << (wav__container_bytes(bits_per_sample) * 8 - 1));
    if(scale == WAV_PCM_SCALE_POW2) {
        return half_range;
    }
    if(scale == WAV_PCM_SCALE_MAX) {
        return half_range - 1.0;
    }
    return 0.0;
}

static int64_t wav__read_wide_pcm_sample(const uint8_t *p, unsigned container_bytes) {
    uint64_t raw = 0;
    for(unsigned b = 0; b < container_bytes; b++) {
        raw |= (uint64_t)p[b] << (8 * b);
    }
    uint64_t sign_bit = 1ULL << (container_bytes * 8 - 1);
    return (int64_t)((raw ^ sign_bit) - sign_bit);
}

static int64_t wav__read_pcm_sample(const uint8_t *p, unsigned container_bytes) {
    switch(container_bytes) {
        case 1: return (int64_t)p[0] - 128;
        case 2: return (int16_t)wav__read_u16_le(p);
        case 3: return (((int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16)) ^ 0x800000) - 0x800000;
        case 4: return (int32_t)wav__read_u32_le(p);
        default: return wav__read_wide_pcm_sample(p, container_bytes);
    }
}

static int64_t wav__largest_pcm_value(unsigned container_bytes) {
    return (int64_t)((1ULL << (container_bytes * 8 - 1)) - 1);
}

static int64_t wav__quantize(double x, double full_scale, int64_t largest) {
    double scaled = x * full_scale;
    if(scaled != scaled) {
        return 0;
    }
    int64_t smallest = -largest - 1;
    if(scaled >= (double)largest) {
        return largest;
    }
    if(scaled <= (double)smallest) {
        return smallest;
    }
    return (int64_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

static void wav__write_wide_pcm_sample(uint8_t *out, unsigned container_bytes, int64_t value) {
    uint64_t raw = (uint64_t)value;
    for(unsigned b = 0; b < container_bytes; b++) {
        out[b] = (uint8_t)(raw >> (8 * b));
    }
}

static wav_result wav__check_convert(const void *a, const void *b, uint16_t bits_per_sample, wav_pcm_scale scale, uint64_t n_samples, wav_error *error) {
    if(wav_pcm_full_scale(bits_per_sample, scale) == 0.0) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("bits_per_sample must be 1..64 and scale a wav_pcm_scale"));
    }
    if(n_samples > 0 && (!a || !b)) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid src or dst"));
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_pcm_to_f64(const void *src, uint16_t bits_per_sample, wav_pcm_scale scale, double *dst, uint64_t n_samples, wav_error *error) {
    wav_result res = wav__check_convert(src, dst, bits_per_sample, scale, n_samples, error);
    if(res != WAV_OK) {
        return res;
    }
    const uint8_t *in = (const uint8_t *)src;
    unsigned container_bytes = wav__container_bytes(bits_per_sample);
    double full_scale = wav_pcm_full_scale(bits_per_sample, scale);
    for(uint64_t i = 0; i < n_samples; i++) {
        dst[i] = (double)wav__read_pcm_sample(in, container_bytes) / full_scale;
        in += container_bytes;
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_pcm_to_f32(const void *src, uint16_t bits_per_sample, wav_pcm_scale scale, float *dst, uint64_t n_samples, wav_error *error) {
    wav_result res = wav__check_convert(src, dst, bits_per_sample, scale, n_samples, error);
    if(res != WAV_OK) {
        return res;
    }
    const uint8_t *in = (const uint8_t *)src;
    unsigned container_bytes = wav__container_bytes(bits_per_sample);
    double full_scale = wav_pcm_full_scale(bits_per_sample, scale);
    for(uint64_t i = 0; i < n_samples; i++) {
        dst[i] = (float)((double)wav__read_pcm_sample(in, container_bytes) / full_scale);
        in += container_bytes;
    }
    return WAV_OK;
}

static int32_t wav__quantize32(double x, double full_scale, double smallest, double largest) {
    double scaled = x * full_scale;
    scaled = scaled < smallest ? smallest : scaled;
    scaled = scaled > largest ? largest : scaled;
    scaled = scaled == scaled ? scaled : 0.0;
    return (int32_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

#define WAV__DEFINE_ENCODER(suffix, src_type) \
    static void wav__encode_##suffix(const src_type *src, uint8_t *out, unsigned container_bytes, double full_scale, uint64_t n_samples) { \
        double largest = (double)wav__largest_pcm_value(container_bytes); \
        double smallest = -largest - 1.0; \
        switch(container_bytes) { \
            case 1: \
                for(uint64_t i = 0; i < n_samples; i++) { \
                    out[i] = (uint8_t)(wav__quantize32((double)src[i], full_scale, smallest, largest) + 128); \
                } \
                break; \
            case 2: \
                for(uint64_t i = 0; i < n_samples; i++, out += 2) { \
                    uint32_t v = (uint32_t)wav__quantize32((double)src[i], full_scale, smallest, largest); \
                    out[0] = (uint8_t)v; \
                    out[1] = (uint8_t)(v >> 8); \
                } \
                break; \
            case 3: \
                for(uint64_t i = 0; i < n_samples; i++, out += 3) { \
                    uint32_t v = (uint32_t)wav__quantize32((double)src[i], full_scale, smallest, largest); \
                    out[0] = (uint8_t)v; \
                    out[1] = (uint8_t)(v >> 8); \
                    out[2] = (uint8_t)(v >> 16); \
                } \
                break; \
            case 4: \
                for(uint64_t i = 0; i < n_samples; i++, out += 4) { \
                    uint32_t v = (uint32_t)wav__quantize32((double)src[i], full_scale, smallest, largest); \
                    out[0] = (uint8_t)v; \
                    out[1] = (uint8_t)(v >> 8); \
                    out[2] = (uint8_t)(v >> 16); \
                    out[3] = (uint8_t)(v >> 24); \
                } \
                break; \
            default: \
                for(uint64_t i = 0; i < n_samples; i++, out += container_bytes) { \
                    wav__write_wide_pcm_sample(out, container_bytes, wav__quantize((double)src[i], full_scale, wav__largest_pcm_value(container_bytes))); \
                } \
                break; \
        } \
    }

WAV__DEFINE_ENCODER(f64, double)
WAV__DEFINE_ENCODER(f32, float)

WAV_IFC wav_result wav_f64_to_pcm(const double *src, void *dst, uint16_t bits_per_sample, wav_pcm_scale scale, uint64_t n_samples, wav_error *error) {
    wav_result res = wav__check_convert(src, dst, bits_per_sample, scale, n_samples, error);
    if(res == WAV_OK) {
        wav__encode_f64(src, (uint8_t *)dst, wav__container_bytes(bits_per_sample), wav_pcm_full_scale(bits_per_sample, scale), n_samples);
    }
    return res;
}

WAV_IFC wav_result wav_f32_to_pcm(const float *src, void *dst, uint16_t bits_per_sample, wav_pcm_scale scale, uint64_t n_samples, wav_error *error) {
    wav_result res = wav__check_convert(src, dst, bits_per_sample, scale, n_samples, error);
    if(res == WAV_OK) {
        wav__encode_f32(src, (uint8_t *)dst, wav__container_bytes(bits_per_sample), wav_pcm_full_scale(bits_per_sample, scale), n_samples);
    }
    return res;
}

static void wav__copy_strided(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride, uint64_t count, size_t sample_size) {
    switch(sample_size) {
        case 2:
            for(uint64_t i = 0; i < count; i++, dst += dst_stride, src += src_stride) {
                dst[0] = src[0];
                dst[1] = src[1];
            }
            break;
        case 4:
            for(uint64_t i = 0; i < count; i++, dst += dst_stride, src += src_stride) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            break;
        default:
            for(uint64_t i = 0; i < count; i++, dst += dst_stride, src += src_stride) {
                for(size_t b = 0; b < sample_size; b++) {
                    dst[b] = src[b];
                }
            }
            break;
    }
}

static wav_result wav__check_layout(const void *const *pointers, uint16_t n_channels, uint64_t n_frames, size_t sample_size, wav_error *error) {
    if(n_channels == 0 || sample_size == 0) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("n_channels and sample_size must be non-zero"));
    }
    if(n_frames == 0) {
        return WAV_OK;
    }
    if(!pointers) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid channels"));
    }
    for(uint16_t c = 0; c < n_channels; c++) {
        if(!pointers[c]) {
            return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, c, WAV_STRVIEW("Null channel pointer"));
        }
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_interleave(const void *const *channels, void *dst, uint16_t n_channels, uint64_t n_frames, size_t sample_size, wav_error *error) {
    wav_result res = wav__check_layout(channels, n_channels, n_frames, sample_size, error);
    if(res != WAV_OK || n_frames == 0) {
        return res;
    }
    if(!dst) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid dst"));
    }
    size_t frame_size = (size_t)n_channels * sample_size;
    for(uint16_t c = 0; c < n_channels; c++) {
        wav__copy_strided((uint8_t *)dst + c * sample_size, frame_size, (const uint8_t *)channels[c], sample_size, n_frames, sample_size);
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_deinterleave(const void *src, void *const *channels, uint16_t n_channels, uint64_t n_frames, size_t sample_size, wav_error *error) {
    wav_result res = wav__check_layout((const void *const *)channels, n_channels, n_frames, sample_size, error);
    if(res != WAV_OK || n_frames == 0) {
        return res;
    }
    if(!src) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid src"));
    }
    size_t frame_size = (size_t)n_channels * sample_size;
    for(uint16_t c = 0; c < n_channels; c++) {
        wav__copy_strided((uint8_t *)channels[c], sample_size, (const uint8_t *)src + c * sample_size, frame_size, n_frames, sample_size);
    }
    return WAV_OK;
}

#endif // WAV_IMPLEMENTATION
