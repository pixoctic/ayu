#include <jni.h>
#include <ogg/ogg.h>
#include <stdio.h>
#include <opus.h>
#include <stdlib.h>
#include <time.h>
#include <opusfile.h>
#include <math.h>
#include "c_utils.h"
#include "../third_party/pffft/src/pffft.h"

typedef struct {
    int version;
    int channels; /* Number of channels: 1..255 */
    int preskip;
    ogg_uint32_t input_sample_rate;
    int gain; /* in dB S7.8 should be zero whenever possible */
    int channel_mapping;
    /* The rest is only used if channel_mapping != 0 */
    int nb_streams;
    int nb_coupled;
    unsigned char stream_map[255];
} OpusHeader;

typedef struct {
    unsigned char *data;
    int maxlen;
    int pos;
} Packet;

typedef struct {
    const unsigned char *data;
    int maxlen;
    int pos;
} ROPacket;

typedef struct {
    void *readdata;
    opus_int64 total_samples_per_channel;
    int rawmode;
    int channels;
    long rate;
    int gain;
    int samplesize;
    int endianness;
    char *infilename;
    int ignorelength;
    int skip;
    int extraout;
    char *comments;
    int comments_length;
    int copy_comments;
} oe_enc_opt;
void cleanupVoiceChanging(void);

static int write_uint32(Packet *p, ogg_uint32_t val) {
    if (p->pos > p->maxlen - 4) {
        return 0;
    }
    p->data[p->pos  ] = (val    ) & 0xFF;
    p->data[p->pos+1] = (val>> 8) & 0xFF;
    p->data[p->pos+2] = (val>>16) & 0xFF;
    p->data[p->pos+3] = (val>>24) & 0xFF;
    p->pos += 4;
    return 1;
}

static int write_uint16(Packet *p, ogg_uint16_t val) {
    if (p->pos > p->maxlen-2) {
        return 0;
    }
    p->data[p->pos  ] = (val    ) & 0xFF;
    p->data[p->pos+1] = (val>> 8) & 0xFF;
    p->pos += 2;
    return 1;
}

static int write_chars(Packet *p, const unsigned char *str, int nb_chars)
{
    int i;
    if (p->pos>p->maxlen-nb_chars)
        return 0;
    for (i=0;i<nb_chars;i++)
        p->data[p->pos++] = str[i];
    return 1;
}

static int read_uint32(ROPacket *p, ogg_uint32_t *val)
{
    if (p->pos>p->maxlen-4)
        return 0;
    *val =  (ogg_uint32_t)p->data[p->pos  ];
    *val |= (ogg_uint32_t)p->data[p->pos+1]<< 8;
    *val |= (ogg_uint32_t)p->data[p->pos+2]<<16;
    *val |= (ogg_uint32_t)p->data[p->pos+3]<<24;
    p->pos += 4;
    return 1;
}

static int read_uint16(ROPacket *p, ogg_uint16_t *val)
{
    if (p->pos>p->maxlen-2)
        return 0;
    *val =  (ogg_uint16_t)p->data[p->pos  ];
    *val |= (ogg_uint16_t)p->data[p->pos+1]<<8;
    p->pos += 2;
    return 1;
}

static int read_chars(ROPacket *p, unsigned char *str, int nb_chars)
{
    int i;
    if (p->pos>p->maxlen-nb_chars)
        return 0;
    for (i=0;i<nb_chars;i++)
        str[i] = p->data[p->pos++];
    return 1;
}

int opus_header_to_packet(const OpusHeader *h, unsigned char *packet, int len) {
    int i;
    Packet p;
    unsigned char ch;
    
    p.data = packet;
    p.maxlen = len;
    p.pos = 0;
    if (len < 19) {
        return 0;
    }
    if (!write_chars(&p, (const unsigned char *)"OpusHead", 8)) {
        return 0;
    }

    ch = 1;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }
    
    ch = h->channels;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }
    
    if (!write_uint16(&p, h->preskip)) {
        return 0;
    }
    
    if (!write_uint32(&p, h->input_sample_rate)) {
        return 0;
    }
    
    if (!write_uint16(&p, h->gain)) {
        return 0;
    }
    
    ch = h->channel_mapping;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }
    
    if (h->channel_mapping != 0) {
        ch = h->nb_streams;
        if (!write_chars(&p, &ch, 1)) {
            return 0;
        }
        
        ch = h->nb_coupled;
        if (!write_chars(&p, &ch, 1)) {
            return 0;
        }
        
        /* Multi-stream support */
        for (i = 0; i < h->channels; i++) {
            if (!write_chars(&p, &h->stream_map[i], 1)) {
                return 0;
            }
        }
    }
    
    return p.pos;
}

#define writeint(buf, base, val) do { buf[base + 3] = ((val) >> 24) & 0xff; \
buf[base + 2]=((val) >> 16) & 0xff; \
buf[base + 1]=((val) >> 8) & 0xff; \
buf[base] = (val) & 0xff; \
} while(0)

static void comment_init(char **comments, int *length, const char *vendor_string) {
    // The 'vendor' field should be the actual encoding library used
    size_t vendor_length = strlen(vendor_string);
    int user_comment_list_length = 0;
    size_t len = 8 + 4 + vendor_length + 4;
    char *p = (char *)malloc(len);
    memcpy(p, "OpusTags", 8);
    writeint(p, 8, vendor_length);
    memcpy(p + 12, vendor_string, vendor_length);
    writeint(p, 12 + vendor_length, user_comment_list_length);
    *length = len;
    *comments = p;
}

static void comment_pad(char **comments, int* length, size_t amount) {
    if (amount > 0) {
        char *p = *comments;
        // Make sure there is at least amount worth of padding free, and round up to the maximum that fits in the current ogg segments
        size_t newlen = (*length + amount + 255) / 255 * 255 - 1;
        p = realloc(p, newlen);
        for (int32_t i = *length; i < newlen; i++) {
            p[i] = 0;
        }
        *comments = p;
        *length = newlen;
    }
}

static int writeOggPage(ogg_page *page, FILE *os) {
    int written = fwrite(page->header, sizeof(unsigned char), (size_t) page->header_len, os);
    written += fwrite(page->body, sizeof(unsigned char), (size_t) page->body_len, os);
    return written;
}

const opus_int32 bitrate = OPUS_BITRATE_MAX;
int frame_size = 960;
const int with_cvbr = 1;
const int max_ogg_delay = 0;
const int comment_padding = 512;

opus_int32 rate = 48000;
opus_int32 coding_rate = 48000;

ogg_int32_t _packetId;
OpusEncoder *_encoder = 0;
uint8_t *_packet = 0;
ogg_stream_state os;
FILE *_fileOs = 0;
oe_enc_opt inopt;
OpusHeader header;
opus_int32 min_bytes;
int max_frame_bytes;
ogg_packet op;
ogg_page og;
opus_int64 bytes_written;
opus_int64 pages_out;
opus_int64 total_samples;
ogg_int64_t enc_granulepos;
ogg_int64_t last_granulepos;
int size_segments;
int last_segments;

// Voice changing state
int voiceChangeEnabled = 0;
float pitchShiftSemitones = 4.0f; // +4.0 semitones
float formantScale = 1.20f; // 1.20 formant scaling
int sampleRate = 48000; // Assuming 48kHz sample rate
PFFFT_Setup *pffftSetup = NULL;
float *fftWorkBuffer = NULL;
float *inputBuffer = NULL;
float *outputBuffer = NULL;
float *magnitudeBuffer = NULL;
float *phaseBuffer = NULL;
uint8_t *pcm_padded_buffer = NULL; // Pre-allocated buffer for PCM frame padding
uint8_t *paddedFrameBuffer = NULL;

// Filter coefficients for high-pass filter (110 Hz cutoff)
float hp_a0, hp_a1, hp_a2, hp_b1, hp_b2;
float hp_x1 = 0.0f, hp_x2 = 0.0f;
float hp_y1 = 0.0f, hp_y2 = 0.0f;

// Filter coefficients for presence boost (peak EQ at 3.2 kHz, +2dB)
float pb_a0, pb_a1, pb_a2, pb_b1, pb_b2;
float pb_x1 = 0.0f, pb_x2 = 0.0f;
float pb_y1 = 0.0f, pb_y2 = 0.0f;

void cleanupRecorder() {
    
    ogg_stream_flush(&os, &og);
    
    if (_encoder) {
        opus_encoder_destroy(_encoder);
        _encoder = 0;
    }
    
    ogg_stream_clear(&os);
    
    if (_packet) {
        free(_packet);
        _packet = 0;
    }
    
    if (_fileOs) {
        fclose(_fileOs);
        _fileOs = 0;
    }
    
    _packetId = -1;
    bytes_written = 0;
    pages_out = 0;
    total_samples = 0;
    enc_granulepos = 0;
    size_segments = 0;
    last_segments = 0;
    last_granulepos = 0;
    memset(&os, 0, sizeof(ogg_stream_state));
    memset(&inopt, 0, sizeof(oe_enc_opt));
    memset(&header, 0, sizeof(OpusHeader));
    memset(&op, 0, sizeof(ogg_packet));
    memset(&og, 0, sizeof(ogg_page));

    // Cleanup voice changing components
    cleanupVoiceChanging();
}

// Filter coefficient calculation functions
void updateHPCoefficients(float cutoffFreq, float sampleRate) {
    // Safety check for sample rate
    if (sampleRate <= 0.0f) {
        sampleRate = 48000.0f; // Default to 48kHz if invalid
    }

    float w0 = 2.0f * M_PI * cutoffFreq / sampleRate;
    float cosW0 = cosf(w0);
    float sinW0 = sinf(w0);
    float alpha = sinW0 / 2.0f; // Q = sqrt(2)/2 for Butterworth

    hp_a0 = 1.0f + alpha;
    hp_a1 = -2.0f * cosW0;
    hp_a2 = 1.0f - alpha;
    hp_b1 = -2.0f * cosW0;
    hp_b2 = 1.0f - alpha;

    // Normalize
    hp_a0 = 1.0f / hp_a0;
    hp_a1 *= hp_a0;
    hp_a2 *= hp_a0;
    hp_b1 *= hp_a0;
    hp_b2 *= hp_a0;
}

void updatePBNormalCoefficients(float centerFreq, float gainDB, float sampleRate) {
    // Safety check for sample rate
    if (sampleRate <= 0.0f) {
        sampleRate = 48000.0f; // Default to 48kHz if invalid
    }

    float w0 = 2.0f * M_PI * centerFreq / sampleRate;
    float cosW0 = cosf(w0);
    float sinW0 = sinf(w0);
    float A = powf(10.0f, gainDB / 4.0f);
    float alpha = sinW0 / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/0.707f - 1.0f) + 2.0f); // Q = 0.707

    pb_a0 = 1.0f + alpha * A;
    pb_a1 = -2.0f * cosW0;
    pb_a2 = 1.0f - alpha * A;
    pb_b1 = -2.0f * cosW0;
    pb_b2 = 1.0f - alpha / A;

    // Normalize
    float norm = 1.0f / (1.0f + alpha/A);
    pb_a0 *= norm;
    pb_a1 *= norm;
    pb_a2 *= norm;
    pb_b1 *= norm;
    pb_b2 *= norm;
}

// Voice changing initialization and cleanup
void initializeVoiceChanging(int sample_rate) {
    // Store sample rate
    voiceChangeEnabled = 0; // Start disabled by default
    sampleRate = sample_rate;

    // Calculate frame size (20ms frames)
    frame_size = (sampleRate * 20) / 1000;
    if (frame_size < 64) frame_size = 64; // Minimum reasonable size
    if (frame_size > 1024) frame_size = 1024; // Maximum reasonable size

    // Allocate PCM padded buffer (max size for safety)
    if (pcm_padded_buffer) {
        free(pcm_padded_buffer);
    }
    pcm_padded_buffer = (uint8_t*)malloc(frame_size * 2); // 16-bit PCM = 2 bytes per sample

    // Initialize PFFFT for pitch shifting
    pffftSetup = pffft_new_setup(frame_size, PFFFT_REAL);
    if (pffftSetup) {
        fftWorkBuffer = (float*)pffft_aligned_malloc(frame_size * sizeof(float));
        inputBuffer = (float*)pffft_aligned_malloc(frame_size * sizeof(float));
        outputBuffer = (float*)pffft_aligned_malloc(frame_size * sizeof(float));
        magnitudeBuffer = (float*)pffft_aligned_malloc((frame_size/2 + 1) * sizeof(float));
        phaseBuffer = (float*)pffft_aligned_malloc((frame_size/2 + 1) * sizeof(float));
    }

    // Initialize filter coefficients
    updateHPCoefficients(110.0f, sampleRate); // 110 Hz high-pass
    updatePBNormalCoefficients(3200.0f, 2.0f, sampleRate); // 3.2 kHz peak, +2dB
}

void cleanupVoiceChanging() {
    voiceChangeEnabled = 0;

    if (pffftSetup) {
        pffft_destroy_setup(pffftSetup);
        pffftSetup = NULL;
    }

    if (fftWorkBuffer) {
        pffft_aligned_free(fftWorkBuffer);
        fftWorkBuffer = NULL;
    }

    if (inputBuffer) {
        pffft_aligned_free(inputBuffer);
        inputBuffer = NULL;
    }

    if (outputBuffer) {
        pffft_aligned_free(outputBuffer);
        outputBuffer = NULL;
    }

    if (magnitudeBuffer) {
        pffft_aligned_free(magnitudeBuffer);
        magnitudeBuffer = NULL;
    }

    if (phaseBuffer) {
        pffft_aligned_free(phaseBuffer);
        phaseBuffer = NULL;
    }

    if (paddedFrameBuffer) {
        free(paddedFrameBuffer);
        paddedFrameBuffer = NULL;
    }
}

// JNI method to enable/disable voice changing
JNIEXPORT void Java_org_telegram_messenger_MediaController_setVoiceChangeEnabled(JNIEnv *env, jclass class, jboolean enabled) {
    voiceChangeEnabled = enabled ? 1 : 0;

    // Log the change for debugging
    if (voiceChangeEnabled) {
        LOGI("Voice changing enabled");
    } else {
        LOGI("Voice changing disabled");
    }
}

int initRecorder(const char *path, opus_int32 sampleRate) {
    cleanupRecorder();

    coding_rate = sampleRate;
    rate = sampleRate;

    if (!path) {
        LOGE("path is null");
        return 0;
    }
    
    _fileOs = fopen(path, "w");
    if (!_fileOs) {
        LOGE("error cannot open file: %s", path);
        return 0;
    }
    
    inopt.rate = rate;
    inopt.gain = 0;
    inopt.endianness = 0;
    inopt.copy_comments = 0;
    inopt.rawmode = 0;
    inopt.ignorelength = 0;
    inopt.samplesize = 16;
    inopt.channels = 1;
    inopt.skip = 0;
    
    comment_init(&inopt.comments, &inopt.comments_length, opus_get_version_string());
    
    if (rate != coding_rate) {
        LOGE("Invalid rate");
        return 0;
    }
    
    header.channels = 1;
    header.channel_mapping = 0;
    header.input_sample_rate = rate;
    header.gain = inopt.gain;
    header.nb_streams = 1;
    
    int result = OPUS_OK;
    _encoder = opus_encoder_create(coding_rate, 1, OPUS_APPLICATION_VOIP, &result);
    if (result != OPUS_OK) {
        LOGE("Error cannot create encoder: %s", opus_strerror(result));
        return 0;
    }
    
    min_bytes = max_frame_bytes = (1275 * 3 + 7) * header.nb_streams;
    _packet = malloc(max_frame_bytes);
    
    result = opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(bitrate));
    //result = opus_encoder_ctl(_encoder, OPUS_SET_COMPLEXITY(10));
    if (result != OPUS_OK) {
        LOGE("Error OPUS_SET_BITRATE returned: %s", opus_strerror(result));
        return 0;
    }
    
#ifdef OPUS_SET_LSB_DEPTH
    result = opus_encoder_ctl(_encoder, OPUS_SET_LSB_DEPTH(MAX(8, MIN(24, inopt.samplesize))));
    if (result != OPUS_OK) {
        LOGE("Warning OPUS_SET_LSB_DEPTH returned: %s", opus_strerror(result));
    }
#endif
    
    opus_int32 lookahead;
    result = opus_encoder_ctl(_encoder, OPUS_GET_LOOKAHEAD(&lookahead));
    if (result != OPUS_OK) {
        LOGE("Error OPUS_GET_LOOKAHEAD returned: %s", opus_strerror(result));
        return 0;
    }
    
    inopt.skip += lookahead;
    header.preskip = (int)(inopt.skip * (48000.0 / coding_rate));
    inopt.extraout = (int)(header.preskip * (rate / 48000.0));
    
    if (ogg_stream_init(&os, rand()) == -1) {
        LOGE("Error: stream init failed");
        return 0;
    }
    
    unsigned char header_data[100];
    int packet_size = opus_header_to_packet(&header, header_data, 100);
    op.packet = header_data;
    op.bytes = packet_size;
    op.b_o_s = 1;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = 0;
    ogg_stream_packetin(&os, &op);
    
    while ((result = ogg_stream_flush(&os, &og))) {
        if (!result) {
            break;
        }
        
        int pageBytesWritten = writeOggPage(&og, _fileOs);
        if (pageBytesWritten != og.header_len + og.body_len) {
            LOGE("Error: failed writing header to output stream");
            return 0;
        }
        bytes_written += pageBytesWritten;
        pages_out++;
    }
    
    comment_pad(&inopt.comments, &inopt.comments_length, comment_padding);
    op.packet = (unsigned char *)inopt.comments;
    op.bytes = inopt.comments_length;
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = 1;
    ogg_stream_packetin(&os, &op);
    
    while ((result = ogg_stream_flush(&os, &og))) {
        if (result == 0) {
            break;
        }
        
        int writtenPageBytes = writeOggPage(&og, _fileOs);
        if (writtenPageBytes != og.header_len + og.body_len) {
            LOGE("Error: failed writing header to output stream");
            return 0;
        }
        
        bytes_written += writtenPageBytes;
        pages_out++;
    }
    
    free(inopt.comments);

    // Initialize voice changing components
    initializeVoiceChanging(sampleRate);

    return 1;
}
int writeFrame(uint8_t *framePcmBytes, uint32_t frameByteCount) {
    size_t cur_frame_size = frame_size;
    _packetId++;
    
    opus_int32 nb_samples = frameByteCount / 2;
    total_samples += nb_samples;

    // Process audio for voice changing if enabled
    if (voiceChangeEnabled && nb_samples > 0 && pffftSetup != NULL) {
        // Convert PCM to float array
        for (int i = 0; i < nb_samples; i++) {
            int16_t sample = ((int16_t*)framePcmBytes)[i];
            inputBuffer[i] = (float)sample / 32768.0f; // Normalize to [-1.0, 1.0]
        }

        // Apply high-pass filter
        for (int i = 0; i < nb_samples; i++) {
            float sample = inputBuffer[i];
            float out = hp_a0 * sample + hp_a1 * hp_x1 + hp_a2 * hp_x2 - hp_b1 * hp_y1 - hp_b2 * hp_y2;
            hp_x2 = hp_x1;
            hp_x1 = sample;
            hp_y2 = hp_y1;
            hp_y1 = out;
            inputBuffer[i] = out;
        }

        // Apply presence boost (peak EQ)
        for (int i = 0; i < nb_samples; i++) {
            float sample = inputBuffer[i];
            float out = pb_a0 * sample + pb_a1 * pb_x1 + pb_a2 * pb_x2 - pb_b1 * pb_y1 - pb_b2 * pb_y2;
            pb_x2 = pb_x1;
            pb_x1 = sample;
            pb_y2 = pb_y1;
            pb_y1 = out;
            inputBuffer[i] = out;
        }

        // Pitch shifting using FFT-based approach
        // Zero-pad input to frame_size if needed
        for (int i = 0; i < frame_size; i++) {
            if (i < nb_samples) {
                // Already normalized above
            } else {
                inputBuffer[i] = 0.0f;
            }
        }

        // Forward FFT
        pffft_transform(pffftSetup, inputBuffer, outputBuffer, fftWorkBuffer, PFFFT_FORWARD);

        // Convert to magnitude/phase
        for (int i = 0; i <= frame_size/2; i++) {
            float re = outputBuffer[i*2];
            float im = outputBuffer[i*2+1];
            magnitudeBuffer[i] = sqrtf(re*re + im*im);
            phaseBuffer[i] = atan2f(im, re);
        }

        // Apply pitch shifting: shift frequency components up by 4 semitones
        // 4 semitones = 2^(4/12) = 2^(1/3) ≈ 1.2599 frequency multiplier
        float pitchFactor = powf(2.0f, 4.0f/12.0f); // 2^(4/12)

        // Shift magnitudes (formant preservation would be more complex)
        // For simplicity, we're shifting both pitch and formants
        // A more advanced implementation would separate them
        memset(outputBuffer, 0, frame_size * 2 * sizeof(float)); // Clear output

        for (int i = 0; i <= frame_size/2; i++) {
            int newIdx = (int)(i * pitchFactor);
            if (newIdx <= frame_size/2) {
                float re = magnitudeBuffer[i] * cosf(phaseBuffer[i]);
                float im = magnitudeBuffer[i] * sinf(phaseBuffer[i]);
                outputBuffer[newIdx*2] = re;
                outputBuffer[newIdx*2+1] = im;
            }
        }

        // Inverse FFT
        pffft_transform(pffftSetup, outputBuffer, inputBuffer, fftWorkBuffer, PFFFT_BACKWARD);

        // Normalize the IFFT result (PFFFT doesn't scale)
        for (int i = 0; i < frame_size; i++) {
            inputBuffer[i] /= frame_size;
        }

        // Apply formant scaling (simplified - adjust frequencies)
        // This is a simplified approach - a true formant shifter would be more complex
        for (int i = 0; i < nb_samples; i++) {
            // Apply a gentle high-shelf boost to simulate formant shifting
            float sample = inputBuffer[i];
            // Simple approximation: boost higher frequencies slightly
            if (sample > 0) {
                inputBuffer[i] = sample * (1.0f + (formantScale - 1.0f) * (sample * sample));
            } else {
                inputBuffer[i] = sample * (1.0f - (formantScale - 1.0f) * (sample * sample));
            }
        }

        // Apply output scaling to prevent clipping (-1.5 dB ≈ 0.841 linear)
        for (int i = 0; i < nb_samples; i++) {
            inputBuffer[i] *= 0.841f;
        }

        // Convert back to PCM
        for (int i = 0; i < nb_samples; i++) {
            // Clamp to [-1.0, 1.0] and convert back to 16-bit
            float clamped = fminf(1.0f, fmaxf(-1.0f, inputBuffer[i]));
            ((int16_t*)framePcmBytes)[i] = (int16_t)(clamped * 32767.0f);
        }
    }

    if (nb_samples < frame_size) {
        op.e_o_s = 1;
    } else {
        op.e_o_s = 0;
    }
    
    int nbBytes = 0;
    
    if (nb_samples != 0) {
        uint8_t *paddedFrameBytes = framePcmBytes;
        int freePaddedFrameBytes = 0;

        if (nb_samples < cur_frame_size) {
            // Use pre-allocated buffer for padding
            memcpy(pcm_padded_buffer, framePcmBytes, frameByteCount);
            memset(pcm_padded_buffer + nb_samples * 2, 0, (cur_frame_size - nb_samples) * 2);
            paddedFrameBytes = pcm_padded_buffer;
            // Don't free pcm_padded_buffer - it's pre-allocated and reused
        }

        nbBytes = opus_encode(_encoder, (opus_int16 *)paddedFrameBytes, cur_frame_size, _packet, max_frame_bytes / 10);
        if (freePaddedFrameBytes) {
            free(paddedFrameBytes);
        }

        if (nbBytes < 0) {
            LOGE("Encoding failed: %s. Aborting.", opus_strerror(nbBytes));
            return 0;
        }
        
        enc_granulepos += cur_frame_size * 48000 / coding_rate;
        size_segments = (nbBytes + 255) / 255;
        min_bytes = MIN(nbBytes, min_bytes);
    }
    
    while ((((size_segments <= 255) && (last_segments + size_segments > 255)) || (enc_granulepos - last_granulepos > max_ogg_delay)) && ogg_stream_flush_fill(&os, &og, 255 * 255)) {
        if (ogg_page_packets(&og) != 0) {
            last_granulepos = ogg_page_granulepos(&og);
        }
        
        last_segments -= og.header[26];
        int writtenPageBytes = writeOggPage(&og, _fileOs);
        if (writtenPageBytes != og.header_len + og.body_len) {
            LOGE("Error: failed writing data to output stream");
            return 0;
        }
        bytes_written += writtenPageBytes;
        pages_out++;
    }
    
    op.packet = _packet;
    op.bytes = nbBytes;
    op.b_o_s = 0;
    op.granulepos = enc_granulepos;
    if (op.e_o_s) {
        op.granulepos = ((total_samples * 48000 + rate - 1) / rate) + header.preskip;
    }
    op.packetno = 2 + _packetId;
    ogg_stream_packetin(&os, &op);
    last_segments += size_segments;
    
    while ((op.e_o_s || (enc_granulepos + (frame_size * 48000 / coding_rate) - last_granulepos > max_ogg_delay) || (last_segments >= 255)) ? ogg_stream_flush_fill(&os, &og, 255 * 255) : ogg_stream_pageout_fill(&os, &og, 255 * 255)) {
        if (ogg_page_packets(&og) != 0) {
            last_granulepos = ogg_page_granulepos(&og);
        }
        last_segments -= og.header[26];
        int writtenPageBytes = writeOggPage(&og, _fileOs);
        if (writtenPageBytes != og.header_len + og.body_len) {
            LOGE("Error: failed writing data to output stream");
            return 0;
        }
        bytes_written += writtenPageBytes;
        pages_out++;
    }
    
    return 1;
}

JNIEXPORT jint Java_org_telegram_messenger_MediaController_startRecord(JNIEnv *env, jclass class, jstring path, jint sampleRate) {
    const char *pathStr = (*env)->GetStringUTFChars(env, path, 0);

    int32_t result = initRecorder(pathStr, sampleRate);
    
    if (pathStr != 0) {
        (*env)->ReleaseStringUTFChars(env, path, pathStr);
    }
    
    return result;
}

JNIEXPORT jint Java_org_telegram_messenger_MediaController_writeFrame(JNIEnv *env, jclass class, jobject frame, jint len) {
    jbyte *frameBytes = (*env)->GetDirectBufferAddress(env, frame);
    return writeFrame((uint8_t *) frameBytes, (uint32_t) len);
}

JNIEXPORT void Java_org_telegram_messenger_MediaController_stopRecord(JNIEnv *env, jclass class) {
    cleanupRecorder();
}

JNIEXPORT jint Java_org_telegram_messenger_MediaController_isOpusFile(JNIEnv *env, jclass class, jstring path) {
    const char *pathStr = (*env)->GetStringUTFChars(env, path, 0);
    
    int32_t result = 0;
    
    int32_t error = OPUS_OK;
    OggOpusFile *file = op_test_file(pathStr, &error);
    if (file != NULL) {
        error = op_test_open(file);
        op_free(file);
        result = error == OPUS_OK;
    }
    
    if (pathStr != 0) {
        (*env)->ReleaseStringUTFChars(env, path, pathStr);
    }
    
    return result;
}

static inline void set_bits(uint8_t *bytes, int32_t bitOffset, int32_t value) {
    bytes += bitOffset / 8;
    bitOffset %= 8;
    *((int32_t *) bytes) |= (value << bitOffset);
}

JNIEXPORT jbyteArray Java_org_telegram_messenger_MediaController_getWaveform2(JNIEnv *env, jclass class, jshortArray array, jint length) {

    jshort *sampleBuffer = (*env)->GetShortArrayElements(env, array, 0);

    const int32_t resultSamples = 100;
    uint16_t *samples = malloc(100 * 2);
    uint64_t sampleIndex = 0;
    uint16_t peakSample = 0;
    int32_t sampleRate = (int32_t) MAX(1, length / resultSamples);
    int32_t index = 0;

    for (int32_t i = 0; i < length; i++) {
        uint16_t sample = (uint16_t) abs(sampleBuffer[i]);
        if (sample > peakSample) {
            peakSample = sample;
        }
        if (sampleIndex++ % sampleRate == 0) {
            if (index < resultSamples) {
                samples[index++] = peakSample;
            }
            peakSample = 0;
        }
    }

    int64_t sumSamples = 0;
    for (int32_t i = 0; i < resultSamples; i++) {
        sumSamples += samples[i];
    }
    uint16_t peak = (uint16_t) (sumSamples * 1.8f / resultSamples);
    if (peak < 2500) {
        peak = 2500;
    }

    for (int32_t i = 0; i < resultSamples; i++) {
        uint16_t sample = (uint16_t) ((int64_t) samples[i]);
        if (sample > peak) {
            samples[i] = peak;
        }
    }

    (*env)->ReleaseShortArrayElements(env, array, sampleBuffer, 0);

    uint32_t bitstreamLength = resultSamples * 5 / 8 + 1;
    jbyteArray *result = (*env)->NewByteArray(env, bitstreamLength);
    if (result) {
        uint8_t *bytes = malloc(bitstreamLength + 4);
        memset(bytes, 0, bitstreamLength + 4);
        for (int32_t i = 0; i < resultSamples; i++) {
            int32_t value = MIN(31, abs((int32_t) samples[i]) * 31 / peak);
            set_bits(bytes, i * 5, value & 31);
        }
        (*env)->SetByteArrayRegion(env, result, 0, bitstreamLength, (jbyte *) bytes);
    }
    free(samples);
    
    return result;
}

int16_t *sampleBuffer = NULL;

JNIEXPORT jbyteArray Java_org_telegram_messenger_MediaController_getWaveform(JNIEnv *env, jclass class, jstring path) {
    const char *pathStr = (*env)->GetStringUTFChars(env, path, 0);
    jbyteArray result = 0;
    
    int error = OPUS_OK;
    OggOpusFile *opusFile = op_open_file(pathStr, &error);
    if (opusFile != NULL && error == OPUS_OK) {
        int64_t totalSamples = op_pcm_total(opusFile, -1);
        const uint32_t resultSamples = 100;
        int32_t sampleRate = MAX(1, (int32_t) (totalSamples / resultSamples));

        uint16_t *samples = malloc(100 * 2);

        size_t bufferSize = 1024 * 128;
        if (sampleBuffer == NULL) {
            sampleBuffer = malloc(bufferSize);
        }
        uint64_t sampleIndex = 0;
        uint16_t peakSample = 0;

        int32_t index = 0;

        while (1) {
            int readSamples = op_read(opusFile, sampleBuffer, bufferSize / 2, NULL);
            for (int32_t i = 0; i < readSamples; i++) {
                uint16_t sample = (uint16_t) abs(sampleBuffer[i]);
                if (sample > peakSample) {
                    peakSample = sample;
                }
                if (sampleIndex++ % sampleRate == 0) {
                    if (index < resultSamples) {
                        samples[index++] = peakSample;
                    }
                    peakSample = 0;
                }
            }
            if (readSamples == 0) {
                break;
            }
        }

        int64_t sumSamples = 0;
        for (int32_t i = 0; i < resultSamples; i++) {
            sumSamples += samples[i];
        }
        uint16_t peak = (uint16_t) (sumSamples * 1.8f / resultSamples);
        if (peak < 2500) {
            peak = 2500;
        }

        for (int32_t i = 0; i < resultSamples; i++) {
            uint16_t sample = (uint16_t) ((int64_t) samples[i]);
            if (sample > peak) {
                samples[i] = peak;
            }
        }

        //free(sampleBuffer);
        op_free(opusFile);

        uint32_t bitstreamLength = (resultSamples * 5) / 8 + 1;
        result = (*env)->NewByteArray(env, bitstreamLength);
        if (result) {
            uint8_t *bytes = malloc(bitstreamLength + 4);
            memset(bytes, 0, bitstreamLength + 4);

            for (int32_t i = 0; i < resultSamples; i++) {
                int32_t value = MIN(31, abs((int32_t) samples[i]) * 31 / peak);
                set_bits(bytes, i * 5, value & 31);
            }

            (*env)->SetByteArrayRegion(env, result, 0, bitstreamLength, (jbyte *) bytes);
        }
        free(samples);
    }
    
    if (pathStr != 0) {
        (*env)->ReleaseStringUTFChars(env, path, pathStr);
    }
    
    return result;
}
