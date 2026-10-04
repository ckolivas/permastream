#include "permastream.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

int output_init(Output *out, const OutputConfig *config, double buffer_seconds)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_init(&out->mutex, NULL);
    out->config = config;
    out->capacity = (size_t)(config->bitrate * 125 * buffer_seconds) + 4096;
    out->bytes = malloc(out->capacity);
    const AVCodec *codec = avcodec_find_encoder_by_name("libmp3lame");
    if (!codec) {
        log_message("FFmpeg needs the libmp3lame encoder");
        return -1;
    }
    out->codec = avcodec_alloc_context3(codec);
    out->frame = av_frame_alloc();
    out->packet = av_packet_alloc();
    if (!out->bytes || !out->codec || !out->frame || !out->packet)
        return -1;
    out->codec->sample_rate = SAMPLE_RATE;
    out->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    out->codec->bit_rate = config->bitrate * 1000;
    out->codec->time_base = (AVRational){1, SAMPLE_RATE};
    av_channel_layout_default(&out->codec->ch_layout, CHANNELS);
    /* Independent MP3 frames let a new listener join the live stream cleanly. */
    if (av_opt_set_int(out->codec->priv_data, "reservoir", 0, 0) < 0 ||
        avcodec_open2(out->codec, codec, NULL) < 0 ||
        out->codec->frame_size != AUDIO_FRAMES)
        return -1;
    out->frame->format = out->codec->sample_fmt;
    out->frame->sample_rate = SAMPLE_RATE;
    out->frame->nb_samples = AUDIO_FRAMES;
    if (av_channel_layout_copy(&out->frame->ch_layout, &out->codec->ch_layout) < 0 ||
        av_frame_get_buffer(out->frame, 0) < 0)
        return -1;
    return 0;
}

int output_encode(Output *out, const float *pcm)
{
    if (av_frame_make_writable(out->frame) < 0)
        return -1;
    for (int c = 0; c < CHANNELS; c++) {
        float *dst = (float *)out->frame->data[c];
        for (int i = 0; i < AUDIO_FRAMES; i++)
            dst[i] = pcm[i * CHANNELS + c];
    }
    out->frame->pts = out->pts;
    out->pts += AUDIO_FRAMES;
    if (avcodec_send_frame(out->codec, out->frame) < 0)
        return -1;
    int ret;
    while ((ret = avcodec_receive_packet(out->codec, out->packet)) >= 0) {
        size_t size = (size_t)out->packet->size;
        if (size > out->capacity) {
            av_packet_unref(out->packet);
            return -1;
        }
        pthread_mutex_lock(&out->mutex);
        size_t pos = (size_t)(out->end % out->capacity);
        size_t first = out->capacity - pos;
        if (first > size)
            first = size;
        memcpy(out->bytes + pos, out->packet->data, first);
        memcpy(out->bytes, out->packet->data + first, size - first);
        out->end += size;
        pthread_mutex_unlock(&out->mutex);
        av_packet_unref(out->packet);
    }
    return ret == AVERROR(EAGAIN) ? 0 : -1;
}

void output_destroy(Output *out)
{
    av_packet_free(&out->packet);
    av_frame_free(&out->frame);
    avcodec_free_context(&out->codec);
    free(out->bytes);
    pthread_mutex_destroy(&out->mutex);
}
