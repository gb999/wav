#ifndef WAV_H
#define WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#ifndef WAV_IFC
#ifdef WAV_STATIC
#define WAV_IFC static
#else
#define WAV_IFC extern
#endif
#endif

#define WAV_FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#define WAV_FOURCC_RIFF WAV_FOURCC('R', 'I', 'F', 'F')
#define WAV_FOURCC_RF64 WAV_FOURCC('R', 'F', '6', '4')
#define WAV_FOURCC_WAVE WAV_FOURCC('W', 'A', 'V', 'E')
#define WAV_FOURCC_FMT  WAV_FOURCC('f', 'm', 't', ' ')
#define WAV_FOURCC_DATA WAV_FOURCC('d', 'a', 't', 'a')
#define WAV_FOURCC_DS64 WAV_FOURCC('d', 's', '6', '4')
#define WAV_FOURCC_JUNK WAV_FOURCC('J', 'U', 'N', 'K')
#define WAV_FOURCC_BEXT WAV_FOURCC('b', 'e', 'x', 't')
#define WAV_FOURCC_FACT WAV_FOURCC('f', 'a', 'c', 't')
#define WAV_FOURCC_CUE  WAV_FOURCC('c', 'u', 'e', ' ')
#define WAV_FOURCC_LIST WAV_FOURCC('L', 'I', 'S', 'T')
#define WAV_UNKNOWN_FRAMES UINT64_MAX
#define WAV_UNKNOWN_SIZE UINT64_MAX
#define WAV_CONTAINER_HEADER_SIZE 12
#define WAV_CHUNK_HEADER_SIZE 8
#define WAV_DS64_PAYLOAD_SIZE 28
#define WAV_SIZE32_SENTINEL ((uint32_t)0xFFFFFFFF)

#define WAV_SPEAKER_FRONT_LEFT            0x00001
#define WAV_SPEAKER_FRONT_RIGHT           0x00002
#define WAV_SPEAKER_FRONT_CENTER          0x00004
#define WAV_SPEAKER_LOW_FREQUENCY         0x00008
#define WAV_SPEAKER_BACK_LEFT             0x00010
#define WAV_SPEAKER_BACK_RIGHT            0x00020
#define WAV_SPEAKER_FRONT_LEFT_OF_CENTER  0x00040
#define WAV_SPEAKER_FRONT_RIGHT_OF_CENTER 0x00080
#define WAV_SPEAKER_BACK_CENTER           0x00100
#define WAV_SPEAKER_SIDE_LEFT             0x00200
#define WAV_SPEAKER_SIDE_RIGHT            0x00400
#define WAV_SPEAKER_TOP_CENTER            0x00800
#define WAV_SPEAKER_TOP_FRONT_LEFT        0x01000
#define WAV_SPEAKER_TOP_FRONT_CENTER      0x02000
#define WAV_SPEAKER_TOP_FRONT_RIGHT       0x04000
#define WAV_SPEAKER_TOP_BACK_LEFT         0x08000
#define WAV_SPEAKER_TOP_BACK_CENTER       0x10000
#define WAV_SPEAKER_TOP_BACK_RIGHT        0x20000

#define WAV_CHANNEL_MASK_STEREO (WAV_SPEAKER_FRONT_LEFT | WAV_SPEAKER_FRONT_RIGHT)
#define WAV_CHANNEL_MASK_5_1 (WAV_CHANNEL_MASK_STEREO | WAV_SPEAKER_FRONT_CENTER | WAV_SPEAKER_LOW_FREQUENCY | WAV_SPEAKER_BACK_LEFT | WAV_SPEAKER_BACK_RIGHT)
#define WAV_CHANNEL_MASK_7_1 (WAV_CHANNEL_MASK_5_1 | WAV_SPEAKER_SIDE_LEFT | WAV_SPEAKER_SIDE_RIGHT)

typedef struct wav_strview {
    const char *data;
    size_t size;
} wav_strview;

#define WAV_STRVIEW(s) ((wav_strview){(s), sizeof(s) - 1})

typedef enum wav_result {
    WAV_OK = 0,
    WAV_EOF,
    WAV_ERROR_IO,
    WAV_ERROR_UNEXPECTED_EOF,
    WAV_ERROR_INVALID_FORMAT,
    WAV_ERROR_UNSUPPORTED_FORMAT,
    WAV_ERROR_OUT_OF_BOUNDS,
    WAV_ERROR_INVALID_ARGUMENT
} wav_result;

typedef struct wav_error {
    wav_result code;
    uint64_t offset;
    char message[128];
} wav_error;

typedef enum wav_container {
    WAV_CONTAINER_RIFF,
    WAV_CONTAINER_RF64,
} wav_container;

typedef enum wav_write_mode {
    WAV_WRITE_RIFF,
    WAV_WRITE_RF64,
    // picks RIFF or RF64 when the header is written. It always reserves the RF64 layout, so when RIFF wins
    // you waste 36 bytes on a JUNK chunk. Ask for WAV_WRITE_RIFF if you know the file fits in 32 bits.
    WAV_WRITE_AUTO
} wav_write_mode;

typedef enum wav_encoding {
    WAV_ENCODING_PCM,
    WAV_ENCODING_IEEE_FLOAT,
    WAV_ENCODING_ALAW,
    WAV_ENCODING_MULAW,
    WAV_ENCODING_UNKNOWN
} wav_encoding;

// Samples are passed through untouched, little-endian.
//   PCM: 8-bit is unsigned (silence = 128), 16/24/32-bit are signed, 24-bit is packed.
//   Float: IEEE 754. A-law / mu-law: G.711 bytes, 8 bits.
// wav_format_check() reports inconsistent fields. parse and write do not check them, they trust what you give.
typedef struct wav_format {
    wav_encoding encoding;
    // fmt chunk tag. 0 = derive from encoding when writing, non-zero = write as given (for codecs the lib does not know).
    // parse stores the raw tag, 0xFFFE for extensible files.
    uint16_t format_tag;
    uint16_t channels;
    // whole Hz, the format cannot store fractions :(
    uint32_t sample_rate;
    // size of one sample in its container. PCM 1..64, float 32 or 64, A-law / mu-law 8.
    uint16_t bits_per_sample;
    // bytes per frame, must be non-zero. PCM, float, A-law, mu-law: channels * ceil(bits_per_sample / 8). Compressed: the codec block.
    uint16_t block_align;
    // bytes per second. PCM, float, A-law, mu-law: sample_rate * block_align, and 0 means derive that when writing.
    // Compressed: the average, written as given.
    uint32_t byte_rate;
    // bits that carry signal, 0 = bits_per_sample. Fewer than bits_per_sample: the signal sits in the high bits (20 valid in 24).
    uint16_t valid_bits_per_sample;
    // speaker positions, see WAV_SPEAKER_*. Bit i set = speaker i present. Channels are stored in ascending bit order.
    // 0 = no assignment. Otherwise exactly `channels` bits must be set.
    uint32_t channel_mask;
    // all zero = derived from encoding when writing. non-zero = written as given (forces an extensible fmt chunk).
    uint8_t subformat_guid[16];
} wav_format;

typedef struct wav_io {
    void *context;
    // transfers up to size bytes at offset. returns bytes read (>0), 0 on EOF, or -1 on error.
    int64_t (*read)(void *context, uint64_t offset, void *buffer, size_t size);

    // transfers up to size bytes at offset. returns bytes written (>=0), or -1 on error.
    int64_t (*write)(void *context, uint64_t offset, const void *buffer, size_t size);
} wav_io;

typedef struct wav_chunk {
    union {
        char tag[4];
        uint32_t id;
    };
    uint64_t offset;
    uint64_t size;
} wav_chunk;

typedef struct wav_stream_info {
    wav_container container;
    wav_format format;
    uint64_t data_offset;
    uint64_t data_size;
    // 1 frame = 1 sample per channel. Channels are interleaved: [ch0, ch1, ch0, ch1, ...]
    uint64_t n_frames;
    // the data chunk claimed more bytes than the file has. data_size and n_frames are clamped to the file.
    bool data_truncated;
} wav_stream_info;


// primitives
// wav_error *error is optional in every function: pass NULL to ignore it.
WAV_IFC wav_result wav_read_container(const wav_io *io, wav_container *container, wav_error *error);
WAV_IFC wav_result wav_write_container(const wav_io *io, uint64_t offset, wav_container container, uint64_t file_size, wav_error *error);
// chunk.size is the 32-bit field as stored. In RF64 the data chunk holds WAV_SIZE32_SENTINEL and the real size is in ds64,
// so to walk past it continue from stream_info.data_offset + data_size (+1 if odd), not from the advanced offset.
WAV_IFC wav_result wav_read_chunk_header(const wav_io *io, uint64_t *offset, wav_chunk *chunk, wav_error *error);
WAV_IFC wav_result wav_write_chunk_header(const wav_io *io, uint64_t offset, uint32_t id, uint64_t size, wav_error *error);
WAV_IFC wav_result wav_write_chunk_pad(const wav_io *io, uint64_t offset, uint64_t payload_size, wav_error *error);

// audio layer
// file_size is the total file size in bytes, or WAV_UNKNOWN_SIZE if you cannot tell (a pipe).
//   known: an unsized data chunk (streamed file) runs to the end of the file, and a data chunk larger than the file is clamped (data_truncated).
//   unknown: an unsized data chunk stays WAV_UNKNOWN_SIZE / WAV_UNKNOWN_FRAMES, and the chunk size is trusted.
WAV_IFC wav_result wav_parse_stream_info(const wav_io *io, uint64_t file_size, wav_stream_info *stream_info, wav_error *error);
WAV_IFC wav_result wav_read_data_frames(const wav_io *io, const wav_stream_info *stream_info, uint64_t start_frame, void *buffer, uint64_t n_frames, wav_error *error);
WAV_IFC uint64_t wav_header_size(wav_write_mode mode, const wav_format *format);
// first inconsistency in the format, or {NULL, 0}. Formats with an unknown encoding are never reported.
WAV_IFC wav_strview wav_format_check(const wav_format *format);
// n_frames == WAV_UNKNOWN_FRAMES means streaming, the size is not known up front.
//   RF64 / AUTO: RF64 with all-ones sizes in ds64, which is well defined.
//   RIFF: you asked for a malformed file, so the 32-bit sizes are filled with 0xFFFFFFFF.
// AUTO with a known size: RIFF if it fits in 32 bits, else RF64.
// Odd data size: the header counts the pad byte, you write it with wav_write_chunk_pad after the data.
// bytes_written is optional (NULL to skip), it receives the header size.
WAV_IFC wav_result wav_write_header(const wav_io *io, uint64_t offset, wav_write_mode mode, const wav_format *format, uint64_t n_frames, uint64_t *bytes_written, wav_error *error);
WAV_IFC wav_result wav_write_data_frames(const wav_io *io, uint64_t offset, const wav_format *format, const void *buffer, uint64_t n_frames, wav_error *error);

#ifdef __cplusplus
}
#endif

#endif // WAV_H

#if defined(WAV_IMPLEMENTATION) && !defined(WAV_IMPLEMENTATION_DONE)
#define WAV_IMPLEMENTATION_DONE

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "wav.h passes sample bytes through untouched and only supports little-endian hosts"
#endif

static uint16_t wav__read_u16_le(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t wav__read_u32_le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t wav__read_u64_le(const uint8_t *b) {
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) | ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
}

static uint64_t wav__riff_size(uint64_t file_size) {
    return (file_size >= WAV_CHUNK_HEADER_SIZE) ? (file_size - WAV_CHUNK_HEADER_SIZE) : 0;
}

static void wav__write_u16_le(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
}

static void wav__write_u32_le(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static void wav__write_u64_le(uint8_t *b, uint64_t v) {
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    b[4] = (uint8_t)(v >> 32);
    b[5] = (uint8_t)(v >> 40);
    b[6] = (uint8_t)(v >> 48);
    b[7] = (uint8_t)(v >> 56);
}

static wav_result wav__set_error(wav_error *err, wav_result code, uint64_t offset, wav_strview msg) {
    if(err) {
        err->code = code;
        err->offset = offset;
        size_t n = msg.size;
        if(n >= sizeof(err->message)) {
            n = sizeof(err->message) - 1;
        }
        if(msg.data) {
            for(size_t i = 0; i < n; i++) {
                err->message[i] = msg.data[i];
            }
        }
        else {
            n = 0;
        }
        err->message[n] = '\0';
    }
    return code;
}

static wav_result wav__read_exact(const wav_io *io, uint64_t offset, void *buffer, size_t size, wav_error *error) {
    if(!io || !io->read) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid io"));
    }
    uint8_t *dst = (uint8_t *)buffer;
    size_t total = 0;
    while(total < size) {
        int64_t n = io->read(io->context, offset + total, dst + total, size - total);
        if(n < 0) {
            return wav__set_error(error, WAV_ERROR_IO, offset + total, WAV_STRVIEW("I/O read failure"));
        }
        if(n == 0) {
            if(total == 0) {
                return WAV_EOF;
            }
            return wav__set_error(error, WAV_ERROR_UNEXPECTED_EOF, offset + total, WAV_STRVIEW("Truncated read"));
        }
        total += (size_t)n;
    }
    return WAV_OK;
}

static wav_result wav__write_exact(const wav_io *io, uint64_t offset, const void *buffer, size_t size, wav_error *error) {
    if(!io || !io->write) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid io"));
    }
    const uint8_t *src = (const uint8_t *)buffer;
    size_t total = 0;
    while(total < size) {
        int64_t n = io->write(io->context, offset + total, src + total, size - total);
        if(n <= 0) {
            return wav__set_error(error, WAV_ERROR_IO, offset + total, WAV_STRVIEW("I/O write failure"));
        }
        total += (size_t)n;
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_read_container(const wav_io *io, wav_container *container, wav_error *error) {
    if(!container) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid container argument"));
    }

    uint8_t hdr[WAV_CONTAINER_HEADER_SIZE];
    wav_result res = wav__read_exact(io, 0, hdr, sizeof(hdr), error);
    if(res != WAV_OK) {
        return res;
    }

    uint32_t cid = wav__read_u32_le(hdr);
    uint32_t fmt = wav__read_u32_le(hdr + 8);

    if(fmt != WAV_FOURCC_WAVE) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, WAV_CHUNK_HEADER_SIZE, WAV_STRVIEW("Missing 'WAVE' format identifier"));
    }
    if(cid == WAV_FOURCC_RIFF) {
        *container = WAV_CONTAINER_RIFF;
        return WAV_OK;
    }
    if(cid == WAV_FOURCC_RF64) {
        *container = WAV_CONTAINER_RF64;
        return WAV_OK;
    }
    return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, 0, WAV_STRVIEW("Not a RIFF or RF64 container"));
}

WAV_IFC wav_result wav_write_container(const wav_io *io, uint64_t offset, wav_container container, uint64_t file_size, wav_error *error) {
    uint32_t cid = 0;
    uint32_t size32 = 0;

    if(container == WAV_CONTAINER_RF64) {
        cid = WAV_FOURCC_RF64;
        size32 = WAV_SIZE32_SENTINEL;
    }
    else if(container == WAV_CONTAINER_RIFF) {
        cid = WAV_FOURCC_RIFF;
        if(file_size == WAV_UNKNOWN_SIZE) {
            size32 = WAV_SIZE32_SENTINEL;
        }
        else {
            uint64_t riff_size = wav__riff_size(file_size);
            if(riff_size >= WAV_SIZE32_SENTINEL) {
                return wav__set_error(error, WAV_ERROR_OUT_OF_BOUNDS, offset, WAV_STRVIEW("File size exceeds 4 GiB RIFF limit"));
            }
            size32 = (uint32_t)riff_size;
        }
    }
    else {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid container type"));
    }

    uint8_t hdr[WAV_CONTAINER_HEADER_SIZE];
    wav__write_u32_le(hdr, cid);
    wav__write_u32_le(hdr + 4, size32);
    wav__write_u32_le(hdr + WAV_CHUNK_HEADER_SIZE, WAV_FOURCC_WAVE);

    return wav__write_exact(io, offset, hdr, sizeof(hdr), error);
}

WAV_IFC wav_result wav_read_chunk_header(const wav_io *io, uint64_t *offset, wav_chunk *chunk, wav_error *error) {
    if(!offset || !chunk) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid argument"));
    }

    uint8_t hdr[WAV_CHUNK_HEADER_SIZE];
    wav_result res = wav__read_exact(io, *offset, hdr, sizeof(hdr), error);
    if(res != WAV_OK) {
        return res;
    }

    uint64_t size = (uint64_t)wav__read_u32_le(hdr + 4);
    uint64_t span = WAV_CHUNK_HEADER_SIZE + size + (size & 1);
    if(*offset > UINT64_MAX - span) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, *offset, WAV_STRVIEW("Chunk runs past the 64-bit offset range"));
    }

    chunk->id = wav__read_u32_le(hdr);
    chunk->offset = *offset + WAV_CHUNK_HEADER_SIZE;
    chunk->size = size;
    *offset += span;
    return WAV_OK;
}

WAV_IFC wav_result wav_write_chunk_header(const wav_io *io, uint64_t offset, uint32_t id, uint64_t size, wav_error *error) {
    uint32_t size32 = (size >= WAV_SIZE32_SENTINEL) ? WAV_SIZE32_SENTINEL : (uint32_t)size;
    uint8_t hdr[WAV_CHUNK_HEADER_SIZE];
    wav__write_u32_le(hdr, id);
    wav__write_u32_le(hdr + 4, size32);
    return wav__write_exact(io, offset, hdr, sizeof(hdr), error);
}

WAV_IFC wav_result wav_write_chunk_pad(const wav_io *io, uint64_t offset, uint64_t payload_size, wav_error *error) {
    if(payload_size & 1) {
        uint8_t zero = 0;
        return wav__write_exact(io, offset, &zero, 1, error);
    }
    return WAV_OK;
}

#define WAV__FACT_PAYLOAD_SIZE 4
#define WAV__FMT_SIZE_PCM 16
#define WAV__FMT_SIZE_WITH_CB_SIZE 18
#define WAV__FMT_SIZE_EXTENSIBLE 40
#define WAV__TAG_PCM 0x0001
#define WAV__TAG_IEEE_FLOAT 0x0003
#define WAV__TAG_ALAW 0x0006
#define WAV__TAG_MULAW 0x0007
#define WAV__TAG_EXTENSIBLE 0xFFFE

static const uint8_t wav__guid_suffix[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static uint16_t wav__tag_from_encoding(wav_encoding encoding) {
    switch(encoding) {
        case WAV_ENCODING_PCM: return WAV__TAG_PCM;
        case WAV_ENCODING_IEEE_FLOAT: return WAV__TAG_IEEE_FLOAT;
        case WAV_ENCODING_ALAW: return WAV__TAG_ALAW;
        case WAV_ENCODING_MULAW: return WAV__TAG_MULAW;
        default: return 0;
    }
}

static wav_encoding wav__encoding_from_tag(uint32_t tag) {
    switch(tag) {
        case WAV__TAG_PCM: return WAV_ENCODING_PCM;
        case WAV__TAG_IEEE_FLOAT: return WAV_ENCODING_IEEE_FLOAT;
        case WAV__TAG_ALAW: return WAV_ENCODING_ALAW;
        case WAV__TAG_MULAW: return WAV_ENCODING_MULAW;
        default: return WAV_ENCODING_UNKNOWN;
    }
}

static bool wav__has_guid(const wav_format *format) {
    for(size_t i = 0; i < sizeof(format->subformat_guid); i++) {
        if(format->subformat_guid[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool wav__guid_is_standard(const uint8_t *guid) {
    for(size_t i = 0; i < sizeof(wav__guid_suffix); i++) {
        if(guid[4 + i] != wav__guid_suffix[i]) {
            return false;
        }
    }
    return true;
}

static uint16_t wav__plain_tag(const wav_format *format) {
    if(format->format_tag != 0 && format->format_tag != WAV__TAG_EXTENSIBLE) {
        return format->format_tag;
    }
    return wav__tag_from_encoding(format->encoding);
}

static bool wav__is_pcm(const wav_format *format) {
    if(wav__has_guid(format)) {
        return wav__guid_is_standard(format->subformat_guid) && wav__read_u32_le(format->subformat_guid) == WAV__TAG_PCM;
    }
    return wav__plain_tag(format) == WAV__TAG_PCM;
}

static wav_result wav__check_format(const wav_format *format, uint64_t offset, wav_error *error) {
    if(!format) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid format argument"));
    }
    if(wav__plain_tag(format) == 0 && !wav__has_guid(format)) {
        return wav__set_error(error, WAV_ERROR_UNSUPPORTED_FORMAT, offset, WAV_STRVIEW("Unknown encoding needs a format_tag or a subformat_guid"));
    }
    if(format->channels == 0 || format->sample_rate == 0 || format->bits_per_sample == 0 || format->block_align == 0) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Format needs channels, sample_rate, bits_per_sample and block_align"));
    }
    return WAV_OK;
}

static unsigned wav__popcount(uint32_t v) {
    unsigned n = 0;
    for(; v != 0; v &= v - 1) {
        n++;
    }
    return n;
}

WAV_IFC wav_strview wav_format_check(const wav_format *format) {
    wav_strview consistent = {NULL, 0};
    if(!format || format->encoding == WAV_ENCODING_UNKNOWN) {
        return consistent;
    }
    if(format->encoding == WAV_ENCODING_PCM && format->bits_per_sample > 64) {
        return WAV_STRVIEW("PCM bits_per_sample must be 1..64");
    }
    if(format->encoding == WAV_ENCODING_IEEE_FLOAT && format->bits_per_sample != 32 && format->bits_per_sample != 64) {
        return WAV_STRVIEW("float bits_per_sample must be 32 or 64");
    }
    if((format->encoding == WAV_ENCODING_ALAW || format->encoding == WAV_ENCODING_MULAW) && format->bits_per_sample != 8) {
        return WAV_STRVIEW("A-law and mu-law bits_per_sample must be 8");
    }
    uint64_t container_bytes = ((uint64_t)format->bits_per_sample + 7) / 8;
    if(format->block_align != (uint64_t)format->channels * container_bytes) {
        return WAV_STRVIEW("block_align != channels * ceil(bits_per_sample / 8)");
    }
    if(format->byte_rate != 0 && format->byte_rate != (uint64_t)format->sample_rate * format->block_align) {
        return WAV_STRVIEW("byte_rate != sample_rate * block_align");
    }
    if(format->channel_mask != 0 && wav__popcount(format->channel_mask) != format->channels) {
        return WAV_STRVIEW("channel_mask must have exactly one bit set per channel");
    }
    if(format->valid_bits_per_sample > format->bits_per_sample) {
        return WAV_STRVIEW("valid_bits_per_sample > bits_per_sample");
    }
    return consistent;
}

static bool wav__needs_extensible(const wav_format *format) {
    return wav__has_guid(format) ||
           format->channels > 2 ||
           (format->encoding == WAV_ENCODING_PCM && format->bits_per_sample > 16) ||
           format->channel_mask != 0 ||
           (format->valid_bits_per_sample != 0 && format->valid_bits_per_sample != format->bits_per_sample);
}

static uint32_t wav__fmt_payload_size(const wav_format *format) {
    if(wav__needs_extensible(format)) {
        return WAV__FMT_SIZE_EXTENSIBLE;
    }
    return wav__is_pcm(format) ? WAV__FMT_SIZE_PCM : WAV__FMT_SIZE_WITH_CB_SIZE;
}

static uint64_t wav__header_size(bool ds64_slot, const wav_format *format) {
    uint64_t size = WAV_CONTAINER_HEADER_SIZE;
    if(ds64_slot) {
        size += WAV_CHUNK_HEADER_SIZE + WAV_DS64_PAYLOAD_SIZE;
    }
    size += WAV_CHUNK_HEADER_SIZE + wav__fmt_payload_size(format);
    if(!wav__is_pcm(format)) {
        size += WAV_CHUNK_HEADER_SIZE + WAV__FACT_PAYLOAD_SIZE;
    }
    size += WAV_CHUNK_HEADER_SIZE;
    return size;
}

static uint32_t wav__effective_byte_rate(const wav_format *format) {
    if(format->byte_rate != 0 || format->encoding == WAV_ENCODING_UNKNOWN) {
        return format->byte_rate;
    }
    uint64_t derived = (uint64_t)format->sample_rate * format->block_align;
    return derived > UINT32_MAX ? UINT32_MAX : (uint32_t)derived;
}

// fmt payload: tag@0 channels@2 sample_rate@4 byte_rate@8 block_align@12 bits@14 | cbSize@16 | valid_bits@18 channel_mask@20 guid@24
static void wav__encode_fmt(uint8_t *b, const wav_format *format) {
    bool extensible = wav__needs_extensible(format);
    uint16_t tag = wav__plain_tag(format);
    wav__write_u16_le(b, extensible ? WAV__TAG_EXTENSIBLE : tag);
    wav__write_u16_le(b + 2, format->channels);
    wav__write_u32_le(b + 4, format->sample_rate);
    wav__write_u32_le(b + 8, wav__effective_byte_rate(format));
    wav__write_u16_le(b + 12, format->block_align);
    wav__write_u16_le(b + 14, format->bits_per_sample);
    if(wav__is_pcm(format) && !extensible) {
        return;
    }
    wav__write_u16_le(b + 16, extensible ? 22 : 0);
    if(!extensible) {
        return;
    }
    wav__write_u16_le(b + 18, format->valid_bits_per_sample ? format->valid_bits_per_sample : format->bits_per_sample);
    wav__write_u32_le(b + 20, format->channel_mask);
    if(wav__has_guid(format)) {
        for(size_t i = 0; i < sizeof(format->subformat_guid); i++) {
            b[24 + i] = format->subformat_guid[i];
        }
        return;
    }
    wav__write_u32_le(b + 24, tag);
    for(size_t i = 0; i < sizeof(wav__guid_suffix); i++) {
        b[28 + i] = wav__guid_suffix[i];
    }
}

static wav_result wav__parse_fmt(const wav_io *io, const wav_chunk *chunk, wav_format *format, wav_error *error) {
    if(chunk->size < WAV__FMT_SIZE_PCM) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk->offset, WAV_STRVIEW("fmt chunk too small"));
    }
    uint8_t b[WAV__FMT_SIZE_EXTENSIBLE] = {0};
    size_t n = chunk->size < sizeof(b) ? (size_t)chunk->size : sizeof(b);
    wav_result res = wav__read_exact(io, chunk->offset, b, n, error);
    if(res == WAV_EOF) {
        return wav__set_error(error, WAV_ERROR_UNEXPECTED_EOF, chunk->offset, WAV_STRVIEW("Truncated fmt chunk"));
    }
    if(res != WAV_OK) {
        return res;
    }

    uint16_t tag = wav__read_u16_le(b);
    *format = (wav_format){0};
    format->format_tag = tag;
    format->channels = wav__read_u16_le(b + 2);
    format->sample_rate = wav__read_u32_le(b + 4);
    format->byte_rate = wav__read_u32_le(b + 8);
    format->block_align = wav__read_u16_le(b + 12);
    format->bits_per_sample = wav__read_u16_le(b + 14);
    format->valid_bits_per_sample = format->bits_per_sample;
    format->encoding = wav__encoding_from_tag(tag);

    if(tag == WAV__TAG_EXTENSIBLE) {
        if(chunk->size < WAV__FMT_SIZE_EXTENSIBLE || wav__read_u16_le(b + 16) < 22) {
            return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk->offset, WAV_STRVIEW("Malformed WAVE_FORMAT_EXTENSIBLE fmt chunk"));
        }
        format->valid_bits_per_sample = wav__read_u16_le(b + 18);
        format->channel_mask = wav__read_u32_le(b + 20);
        for(size_t i = 0; i < 16; i++) {
            format->subformat_guid[i] = b[24 + i];
        }
        bool standard_guid = true;
        for(size_t i = 0; i < sizeof(wav__guid_suffix); i++) {
            standard_guid = standard_guid && (b[28 + i] == wav__guid_suffix[i]);
        }
        format->encoding = standard_guid ? wav__encoding_from_tag(wav__read_u32_le(b + 24)) : WAV_ENCODING_UNKNOWN;
    }

    if(format->channels == 0 || format->block_align == 0 || format->bits_per_sample == 0) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk->offset, WAV_STRVIEW("fmt chunk has zero channels, block_align or bits_per_sample"));
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_parse_stream_info(const wav_io *io, uint64_t file_size, wav_stream_info *stream_info, wav_error *error) {
    if(!stream_info) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid stream_info argument"));
    }

    wav_container container;
    wav_result res = wav_read_container(io, &container, error);
    if(res != WAV_OK) {
        return res;
    }

    bool have_fmt = false;
    bool have_ds64 = false;
    wav_format format = {0};
    uint64_t ds64_data_size = WAV_UNKNOWN_SIZE;
    uint64_t offset = WAV_CONTAINER_HEADER_SIZE;
    wav_chunk chunk;

    while(true) {
        res = wav_read_chunk_header(io, &offset, &chunk, error);
        if(res == WAV_EOF) {
            return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, offset, WAV_STRVIEW("Missing data chunk"));
        }
        if(res != WAV_OK) {
            return res;
        }

        if(chunk.id == WAV_FOURCC_DS64) {
            uint8_t b[16];
            if(chunk.size < WAV_DS64_PAYLOAD_SIZE) {
                return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk.offset, WAV_STRVIEW("ds64 chunk too small"));
            }
            res = wav__read_exact(io, chunk.offset, b, sizeof(b), error);
            if(res == WAV_EOF) {
                return wav__set_error(error, WAV_ERROR_UNEXPECTED_EOF, chunk.offset, WAV_STRVIEW("Truncated ds64 chunk"));
            }
            if(res != WAV_OK) {
                return res;
            }
            ds64_data_size = wav__read_u64_le(b + 8);
            have_ds64 = true;
        }
        else if(chunk.id == WAV_FOURCC_FMT) {
            res = wav__parse_fmt(io, &chunk, &format, error);
            if(res != WAV_OK) {
                return res;
            }
            have_fmt = true;
        }
        else if(chunk.id == WAV_FOURCC_DATA) {
            break;
        }
    }

    if(!have_fmt) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk.offset, WAV_STRVIEW("data chunk before fmt chunk"));
    }
    if(container == WAV_CONTAINER_RF64 && !have_ds64) {
        return wav__set_error(error, WAV_ERROR_INVALID_FORMAT, chunk.offset, WAV_STRVIEW("RF64 without ds64 chunk"));
    }

    uint64_t data_size = chunk.size;
    if(container == WAV_CONTAINER_RF64) {
        data_size = ds64_data_size;
    }
    else if(chunk.size == WAV_SIZE32_SENTINEL) {
        data_size = WAV_UNKNOWN_SIZE;
    }

    bool data_truncated = false;
    if(file_size != WAV_UNKNOWN_SIZE) {
        uint64_t available = (file_size > chunk.offset) ? (file_size - chunk.offset) : 0;
        if(data_size == WAV_UNKNOWN_SIZE) {
            data_size = available;
        }
        else if(data_size > available) {
            data_size = available;
            data_truncated = true;
        }
    }

    stream_info->container = container;
    stream_info->format = format;
    stream_info->data_offset = chunk.offset;
    stream_info->data_size = data_size;
    stream_info->n_frames = (data_size == WAV_UNKNOWN_SIZE) ? WAV_UNKNOWN_FRAMES : data_size / format.block_align;
    stream_info->data_truncated = data_truncated;
    return WAV_OK;
}

WAV_IFC wav_result wav_read_data_frames(const wav_io *io, const wav_stream_info *stream_info, uint64_t start_frame, void *buffer, uint64_t n_frames, wav_error *error) {
    if(!stream_info || stream_info->format.block_align == 0 || (!buffer && n_frames > 0)) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, 0, WAV_STRVIEW("Invalid argument"));
    }
    if(n_frames == 0) {
        return WAV_OK;
    }

    uint64_t block = stream_info->format.block_align;
    uint64_t known = stream_info->n_frames;
    if(known != WAV_UNKNOWN_FRAMES && (start_frame > known || n_frames > known - start_frame)) {
        return wav__set_error(error, WAV_ERROR_OUT_OF_BOUNDS, stream_info->data_offset, WAV_STRVIEW("Read past the end of the data chunk"));
    }
    if(start_frame > (UINT64_MAX - stream_info->data_offset) / block || n_frames > (uint64_t)SIZE_MAX / block) {
        return wav__set_error(error, WAV_ERROR_OUT_OF_BOUNDS, stream_info->data_offset, WAV_STRVIEW("Read range overflows"));
    }

    uint64_t byte_offset = stream_info->data_offset + start_frame * block;
    wav_result res = wav__read_exact(io, byte_offset, buffer, (size_t)(n_frames * block), error);
    if(res == WAV_EOF) {
        return wav__set_error(error, WAV_ERROR_UNEXPECTED_EOF, byte_offset, WAV_STRVIEW("Data ends before the requested frames"));
    }
    return res;
}

WAV_IFC uint64_t wav_header_size(wav_write_mode mode, const wav_format *format) {
    if(mode != WAV_WRITE_RIFF && mode != WAV_WRITE_RF64 && mode != WAV_WRITE_AUTO) {
        return 0;
    }
    if(wav__check_format(format, 0, NULL) != WAV_OK) {
        return 0;
    }
    return wav__header_size(mode != WAV_WRITE_RIFF, format);
}

static wav_result wav__write_chunk(const wav_io *io, uint64_t *at, uint32_t id, uint64_t declared_size, const void *payload, size_t payload_size, wav_error *error) {
    wav_result res = wav_write_chunk_header(io, *at, id, declared_size, error);
    if(res != WAV_OK) {
        return res;
    }
    *at += WAV_CHUNK_HEADER_SIZE;
    res = wav__write_exact(io, *at, payload, payload_size, error);
    if(res != WAV_OK) {
        return res;
    }
    *at += payload_size;
    return WAV_OK;
}

WAV_IFC wav_result wav_write_header(const wav_io *io, uint64_t offset, wav_write_mode mode, const wav_format *format, uint64_t n_frames, uint64_t *bytes_written, wav_error *error) {
    if(mode != WAV_WRITE_RIFF && mode != WAV_WRITE_RF64 && mode != WAV_WRITE_AUTO) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid write mode"));
    }
    wav_result res = wav__check_format(format, offset, error);
    if(res != WAV_OK) {
        return res;
    }

    bool ds64_slot = (mode != WAV_WRITE_RIFF);
    uint64_t header_size = wav__header_size(ds64_slot, format);
    bool streaming = (n_frames == WAV_UNKNOWN_FRAMES);
    uint64_t data_size = WAV_UNKNOWN_SIZE;
    uint64_t file_size = WAV_UNKNOWN_SIZE;

    if(!streaming) {
        uint64_t block = format->block_align;
        if(n_frames > (UINT64_MAX - header_size - 1) / block) {
            return wav__set_error(error, WAV_ERROR_OUT_OF_BOUNDS, offset, WAV_STRVIEW("Frame count overflows 64-bit file size"));
        }
        data_size = n_frames * block;
        file_size = header_size + data_size + (data_size & 1);
    }

    wav_container container = WAV_CONTAINER_RIFF;
    if(mode == WAV_WRITE_RF64) {
        container = WAV_CONTAINER_RF64;
    }
    else if(mode == WAV_WRITE_AUTO) {
        bool too_big_for_riff = !streaming && (wav__riff_size(file_size) >= WAV_SIZE32_SENTINEL);
        container = (streaming || too_big_for_riff) ? WAV_CONTAINER_RF64 : WAV_CONTAINER_RIFF;
    }

    uint64_t at = offset;
    res = wav_write_container(io, at, container, file_size, error);
    if(res != WAV_OK) {
        return res;
    }
    at += WAV_CONTAINER_HEADER_SIZE;

    if(ds64_slot) {
        uint8_t ds64[WAV_DS64_PAYLOAD_SIZE] = {0};
        uint32_t id = WAV_FOURCC_JUNK;
        if(container == WAV_CONTAINER_RF64) {
            id = WAV_FOURCC_DS64;
            wav__write_u64_le(ds64, streaming ? WAV_UNKNOWN_SIZE : wav__riff_size(file_size));
            wav__write_u64_le(ds64 + 8, data_size);
            wav__write_u64_le(ds64 + 16, n_frames);
        }
        res = wav__write_chunk(io, &at, id, sizeof(ds64), ds64, sizeof(ds64), error);
        if(res != WAV_OK) {
            return res;
        }
    }

    uint8_t fmt[WAV__FMT_SIZE_EXTENSIBLE] = {0};
    uint32_t fmt_size = wav__fmt_payload_size(format);
    wav__encode_fmt(fmt, format);
    res = wav__write_chunk(io, &at, WAV_FOURCC_FMT, fmt_size, fmt, fmt_size, error);
    if(res != WAV_OK) {
        return res;
    }

    if(!wav__is_pcm(format)) {
        uint8_t fact[WAV__FACT_PAYLOAD_SIZE];
        wav__write_u32_le(fact, (streaming || n_frames >= WAV_SIZE32_SENTINEL) ? WAV_SIZE32_SENTINEL : (uint32_t)n_frames);
        res = wav__write_chunk(io, &at, WAV_FOURCC_FACT, sizeof(fact), fact, sizeof(fact), error);
        if(res != WAV_OK) {
            return res;
        }
    }

    uint64_t data_chunk_size = (container == WAV_CONTAINER_RF64) ? WAV_UNKNOWN_SIZE : data_size;
    res = wav__write_chunk(io, &at, WAV_FOURCC_DATA, data_chunk_size, NULL, 0, error);
    if(res != WAV_OK) {
        return res;
    }

    if(bytes_written) {
        *bytes_written = header_size;
    }
    return WAV_OK;
}

WAV_IFC wav_result wav_write_data_frames(const wav_io *io, uint64_t offset, const wav_format *format, const void *buffer, uint64_t n_frames, wav_error *error) {
    if(!format || format->block_align == 0 || (!buffer && n_frames > 0)) {
        return wav__set_error(error, WAV_ERROR_INVALID_ARGUMENT, offset, WAV_STRVIEW("Invalid argument"));
    }
    if(n_frames == 0) {
        return WAV_OK;
    }
    uint64_t block = format->block_align;
    if(n_frames > (uint64_t)SIZE_MAX / block || offset > UINT64_MAX - n_frames * block) {
        return wav__set_error(error, WAV_ERROR_OUT_OF_BOUNDS, offset, WAV_STRVIEW("Write range overflows"));
    }
    return wav__write_exact(io, offset, buffer, (size_t)(n_frames * block), error);
}

#endif // WAV_IMPLEMENTATION
