#ifndef PERMASTREAM_FFMPEG_COMPAT_H
#define PERMASTREAM_FFMPEG_COMPAT_H

#include "permastream.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>

/* AVChannelLayout arrived in libavutil 57.24.100 (released in FFmpeg 5.1).
 * Ubuntu 22.04's FFmpeg 4.4 uses channel counts and channel-layout bit masks.
 * Keep legacy fields out of newer builds, where they are deprecated/removed.
 */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
typedef AVChannelLayout AudioLayout;

static inline int audio_parameter_channels(const AVCodecParameters *parameters)
{
    return parameters->ch_layout.nb_channels;
}

static inline int audio_layout_from_frame(AudioLayout *layout, const AVFrame *frame)
{
    if (frame->ch_layout.nb_channels < 1)
        return AVERROR_INVALIDDATA;
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(layout, frame->ch_layout.nb_channels);
        return 0;
    }
    return av_channel_layout_copy(layout, &frame->ch_layout);
}

static inline void audio_layout_uninit(AudioLayout *layout)
{
    av_channel_layout_uninit(layout);
}

static inline int audio_layout_copy(AudioLayout *dst, const AudioLayout *src)
{
    return av_channel_layout_copy(dst, src);
}

static inline int audio_layout_compare(const AudioLayout *a, const AudioLayout *b)
{
    return av_channel_layout_compare(a, b);
}

static inline int audio_resampler_alloc(SwrContext **resampler, AudioLayout *input,
                                        enum AVSampleFormat format, int rate)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    return swr_alloc_set_opts2(resampler, &stereo, AV_SAMPLE_FMT_FLT, SAMPLE_RATE,
                              input, format, rate, 0, NULL);
}

static inline void audio_encoder_stereo(AVCodecContext *codec)
{
    av_channel_layout_default(&codec->ch_layout, CHANNELS);
}

static inline int audio_frame_layout(AVFrame *frame, const AVCodecContext *codec)
{
    return av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout);
}
#else
typedef uint64_t AudioLayout;

static inline int audio_parameter_channels(const AVCodecParameters *parameters)
{
    return parameters->channels;
}

static inline int audio_layout_from_frame(AudioLayout *layout, const AVFrame *frame)
{
    if (frame->channels < 1)
        return AVERROR_INVALIDDATA;
    *layout = frame->channel_layout;
    if (!*layout)
        *layout = (uint64_t)av_get_default_channel_layout(frame->channels);
    if (!*layout || av_get_channel_layout_nb_channels(*layout) != frame->channels)
        return AVERROR_INVALIDDATA;
    return 0;
}

static inline void audio_layout_uninit(AudioLayout *layout)
{
    *layout = 0;
}

static inline int audio_layout_copy(AudioLayout *dst, const AudioLayout *src)
{
    *dst = *src;
    return 0;
}

static inline int audio_layout_compare(const AudioLayout *a, const AudioLayout *b)
{
    return *a != *b;
}

static inline int audio_resampler_alloc(SwrContext **resampler, AudioLayout *input,
                                        enum AVSampleFormat format, int rate)
{
    *resampler = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT,
                                   SAMPLE_RATE, (int64_t)*input, format, rate, 0, NULL);
    return *resampler ? 0 : AVERROR(ENOMEM);
}

static inline void audio_encoder_stereo(AVCodecContext *codec)
{
    codec->channel_layout = AV_CH_LAYOUT_STEREO;
    codec->channels = CHANNELS;
}

static inline int audio_frame_layout(AVFrame *frame, const AVCodecContext *codec)
{
    frame->channel_layout = codec->channel_layout;
    frame->channels = codec->channels;
    return 0;
}
#endif

#endif
