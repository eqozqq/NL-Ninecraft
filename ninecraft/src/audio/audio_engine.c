#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define AUDIO_ENGINE_MAX_STREAMS 64

#define PCM_S 1
#define PCM_U 2
#define PCM_F 3
#define PCM_BE 1
#define PCM_LE 2

typedef struct {
    bool active;
    uint8_t *buffer;
    uint32_t buffer_size;
    uint32_t num_channels;
    uint32_t bits_per_sample;
    uint32_t freq;
    uint32_t format;
    uint32_t endianess;
    float gain;
    float pitch;
    uint32_t frame_size;
    uint32_t frame_count;
    float sample_pos;
    float rate_ratio;
} audio_engine_stream_t;

static audio_engine_stream_t audio_engine_streams[AUDIO_ENGINE_MAX_STREAMS];
static SDL_AudioDeviceID audio_engine_device = 0;
static SDL_AudioSpec audio_engine_audio_spec;

static bool audio_engine_initialized = false;

static float audio_engine_decode_sample(uint8_t *sample_data, uint32_t sample_size, uint32_t format) {
    float sample = 0;
    if (sample_size == 1 && format == PCM_S) {
        int8_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample /= 128.0;
    } else if (sample_size == 2 && format == PCM_S) {
        int16_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample /= 32768.0;
    } else if (sample_size == 4 && format == PCM_S) {
        int32_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample /= 2147483648.0;
    } else if (sample_size == 1 && format == PCM_U) {
        uint8_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample -= 128.0;
        sample /= 128.0;
    } else if (sample_size == 2 && format == PCM_U) {
        uint16_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample -= 32768.0;
        sample /= 32768.0;
    } else if (sample_size == 4 && format == PCM_U) {
        uint32_t tmp;
        memcpy(&tmp, sample_data, sizeof(tmp));
        sample = (float)tmp;
        sample -= 2147483648.0;
        sample /= 2147483648.0;
    } else if (sample_size == 4 && format == PCM_F) {
        memcpy(&sample, sample_data, sizeof(sample));
    }
    return sample;
}

static float *audio_engine_mix_buffer = NULL;
static int audio_engine_mix_buffer_size = 0;

static int audio_engine_try_open(SDL_AudioSpec *desired_spec) {
    memset(&audio_engine_audio_spec, 0, sizeof(audio_engine_audio_spec));
    audio_engine_device = SDL_OpenAudioDevice(NULL, 0, desired_spec, &audio_engine_audio_spec, SDL_AUDIO_ALLOW_ANY_CHANGE);

    if (audio_engine_device) {
        int supported = 0;
        switch (audio_engine_audio_spec.format) {
            case AUDIO_S16LSB:
            case AUDIO_S16MSB:
            case AUDIO_S32LSB:
            case AUDIO_S32MSB:
            case AUDIO_F32LSB:
            case AUDIO_F32MSB:
            case AUDIO_U8:
            case AUDIO_S8:
                supported = 1;
                break;
        }
        if (!supported) {
            printf("Audio: unsupported format 0x%x\n", audio_engine_audio_spec.format);
            SDL_CloseAudioDevice(audio_engine_device);
            audio_engine_device = 0;
            return 0;
        }
        return 1;
    }
    return 0;
}

static void audio_engine_write_output(float *mix_buf, Uint8 *out_stream, int sample_count) {
    switch (audio_engine_audio_spec.format) {
        case AUDIO_F32LSB:
        case AUDIO_F32MSB:
            memcpy(out_stream, mix_buf, sample_count * sizeof(float));
            break;
        case AUDIO_S16LSB:
        case AUDIO_S16MSB: {
            int16_t *out = (int16_t *)out_stream;
            for (int i = 0; i < sample_count; ++i) {
                float s = mix_buf[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i] = (int16_t)(s * 32767.0f);
            }
            break;
        }
        case AUDIO_S32LSB:
        case AUDIO_S32MSB: {
            int32_t *out = (int32_t *)out_stream;
            for (int i = 0; i < sample_count; ++i) {
                float s = mix_buf[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i] = (int32_t)(s * 2147483647.0f);
            }
            break;
        }
        case AUDIO_U8: {
            uint8_t *out = out_stream;
            for (int i = 0; i < sample_count; ++i) {
                float s = mix_buf[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i] = (uint8_t)((s * 0.5f + 0.5f) * 255.0f);
            }
            break;
        }
        case AUDIO_S8: {
            int8_t *out = (int8_t *)out_stream;
            for (int i = 0; i < sample_count; ++i) {
                float s = mix_buf[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i] = (int8_t)(s * 127.0f);
            }
            break;
        }
    }
}

static void audio_engine_mix_to_float(audio_engine_stream_t *stream, float *out, int sample_count) {
    int sample_size = stream->bits_per_sample / 8;
    for (int i = 0; i < sample_count; i += audio_engine_audio_spec.channels) {
        size_t frame_pos = (size_t)stream->sample_pos;
        if (frame_pos >= stream->frame_count - 1) {
            stream->active = false;
            break;
        }
        float frac = stream->sample_pos - (float)frame_pos;
        uint8_t *frame1 = &stream->buffer[frame_pos * stream->frame_size];
        uint8_t *frame2 = &stream->buffer[(frame_pos + 1) * stream->frame_size];
        uint32_t num_channels = (stream->num_channels < audio_engine_audio_spec.channels) ? audio_engine_audio_spec.channels : stream->num_channels;

        for (int ch = 0; ch < (int)num_channels; ++ch) {
            size_t idx1 = i + (ch % audio_engine_audio_spec.channels);
            size_t idx2 = (ch % stream->num_channels) * sample_size;
            float sample1 = audio_engine_decode_sample(&frame1[idx2], sample_size, stream->format);
            float sample2 = audio_engine_decode_sample(&frame2[idx2], sample_size, stream->format);
            float interpolated = sample1 + (sample2 - sample1) * frac;

            interpolated *= stream->gain;
            out[idx1] += interpolated;

            if (out[idx1] > 1.0f) {
                out[idx1] = 1.0f;
            }
            if (out[idx1] < -1.0f) {
                out[idx1] = -1.0f;
            }
        }
        stream->sample_pos += stream->rate_ratio;
    }
    if ((uint32_t)stream->sample_pos >= stream->frame_count) {
        stream->active = false;
    }
}

static void SDLCALL audio_engine_audio_callback(void *userdata, Uint8 *stream, int len) {
    int bytes_per_sample = SDL_AUDIO_BITSIZE(audio_engine_audio_spec.format) / 8;
    int sample_count = len / bytes_per_sample;

    if (sample_count > audio_engine_mix_buffer_size) {
        audio_engine_mix_buffer = (float *)realloc(audio_engine_mix_buffer, sample_count * sizeof(float));
        audio_engine_mix_buffer_size = sample_count;
    }

    memset(audio_engine_mix_buffer, 0, sample_count * sizeof(float));
    for (int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i) {
        audio_engine_stream_t *audio_engine_stream = &audio_engine_streams[i];
        if (audio_engine_stream->active) {
            audio_engine_mix_to_float(audio_engine_stream, audio_engine_mix_buffer, sample_count);
        }
    }

    memset(stream, 0, len);
    audio_engine_write_output(audio_engine_mix_buffer, stream, sample_count);
}

void audio_engine_init() {
    if (!audio_engine_initialized) {
        SDL_AudioSpec desired_spec;
        const char *fallback_drivers[] = {
            NULL, "pipewire", "pulseaudio", "alsa", "jack", "oss", NULL
        };
        int opened = 0;

        printf("Audio: available SDL drivers:");
        int num_drivers = SDL_GetNumAudioDrivers();
        for (int i = 0; i < num_drivers; ++i) {
            printf(" %s", SDL_GetAudioDriver(i));
        }
        printf("\n");

        memset(&desired_spec, 0, sizeof(desired_spec));
        desired_spec.freq = 44100;
        desired_spec.format = AUDIO_F32LSB;
        desired_spec.channels = 2;
        desired_spec.samples = 4096;
        desired_spec.callback = audio_engine_audio_callback;

        for (int i = 0; fallback_drivers[i] != NULL || i == 0; ++i) {
            if (fallback_drivers[i] != NULL) {
                SDL_setenv("SDL_AUDIODRIVER", fallback_drivers[i], 1);
            }

            if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
                printf("Audio: driver '%s' init failed: %s\n",
                       fallback_drivers[i] ? fallback_drivers[i] : "default",
                       SDL_GetError());
                continue;
            }

            if (audio_engine_try_open(&desired_spec)) {
                printf("Audio: using driver '%s'\n",
                       SDL_GetCurrentAudioDriver());
                opened = 1;
                break;
            }

            printf("Audio: driver '%s' device open failed: %s\n",
                   SDL_GetCurrentAudioDriver(),
                   SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }

        if (!opened) {
            printf("Audio: failed to open any audio device\n");
            return;
        }

        printf("Audio: device opened (%s, %dHz, %dch, %d samples, fmt=0x%x)\n",
               SDL_GetCurrentAudioDriver(),
               audio_engine_audio_spec.freq,
               audio_engine_audio_spec.channels,
               audio_engine_audio_spec.samples,
               audio_engine_audio_spec.format);

        SDL_PauseAudioDevice(audio_engine_device, 0);
        memset(audio_engine_streams, 0, sizeof(audio_engine_streams));
        audio_engine_initialized = true;
    }
}

void audio_engine_destroy() {
    if (audio_engine_initialized) {
        if (audio_engine_device) {
            SDL_CloseAudioDevice(audio_engine_device);
            audio_engine_device = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_engine_initialized = false;
    }
}

void audio_engine_tick() {} // this is a stub and should not be modified

void audio_engine_play(uint8_t *buffer, uint32_t buffer_size, uint32_t num_channels, uint32_t bits_per_sample, uint32_t freq, uint32_t format, uint32_t endianess, float gain, float pitch) {
    if (audio_engine_initialized && buffer && buffer_size) {
        SDL_LockAudioDevice(audio_engine_device);
        for (int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i) {
            audio_engine_stream_t *audio_engine_stream = &audio_engine_streams[i];
            if (!audio_engine_stream->active) {
                audio_engine_stream->active = true;
                audio_engine_stream->buffer = buffer;
                audio_engine_stream->buffer_size = buffer_size;
                audio_engine_stream->num_channels = num_channels;
                audio_engine_stream->bits_per_sample = bits_per_sample;
                audio_engine_stream->freq = freq;
                audio_engine_stream->format = format;
                audio_engine_stream->endianess = endianess;
                audio_engine_stream->gain = gain;
                audio_engine_stream->pitch = pitch;
                audio_engine_stream->frame_size = (bits_per_sample / 8) * num_channels;
                audio_engine_stream->frame_count = buffer_size / audio_engine_stream->frame_size;
                audio_engine_stream->sample_pos = 0;
                audio_engine_stream->rate_ratio = ((float)freq * pitch) / (float)audio_engine_audio_spec.freq;
                break;
            }
        }
        SDL_UnlockAudioDevice(audio_engine_device);
    }
}