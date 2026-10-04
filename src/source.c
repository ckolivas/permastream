#include "permastream.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int interrupted(void *opaque)
{
    Source *source = opaque;
    return stopping() || monotonic_ns() >= source->deadline;
}

static int open_input_io(AVFormatContext *format, AVIOContext **io, const char *url,
                         int flags, AVDictionary **options)
{
    Source *source = format->interrupt_callback.opaque;
    /* Reapply these to nested HLS playlists, segments and encryption keys too. */
    av_dict_set(options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
    av_dict_set(options, "tls_verify", "1", 0);
    av_dict_set_int(options, "rw_timeout",
                    (int64_t)(source->config->timeout_seconds * 1e6), 0);
    return avio_open2(io, url, flags, &format->interrupt_callback, options);
}

static void buffer_audio(Source *source, const float *pcm, size_t frames)
{
    pthread_mutex_lock(&source->mutex);
    if (!source->online) {
        source->online = true;
        source->healthy_since = monotonic_ns();
        /* A standby must not retain old audio across a reconnection. */
        if (atomic_load(&selected_source) != source->index) {
            source->head = source->count = 0;
            source->ready = false;
        }
    }
    if (frames > source->capacity) {
        pcm += (frames - source->capacity) * CHANNELS;
        frames = source->capacity;
    }
    if (source->count + frames > source->capacity) {
        size_t drop = source->count + frames - source->capacity;
        source->head = (source->head + drop) % source->capacity;
        source->count -= drop;
    }
    size_t pos = (source->head + source->count) % source->capacity;
    size_t first = source->capacity - pos;
    if (first > frames)
        first = frames;
    memcpy(source->samples + pos * CHANNELS, pcm, first * CHANNELS * sizeof(float));
    memcpy(source->samples, pcm + first * CHANNELS,
           (frames - first) * CHANNELS * sizeof(float));
    source->count += frames;
    if (source->count >= source->target)
        source->ready = true;
    pthread_mutex_unlock(&source->mutex);
}

static int receive_audio(Source *source, AVCodecContext *decoder, AVFrame *frame,
                         SwrContext **resampler, AVChannelLayout *previous_layout,
                         int *previous_rate, int *previous_format,
                         uint8_t **pcm, unsigned int *pcm_capacity)
{
    int ret;
    while ((ret = avcodec_receive_frame(decoder, frame)) >= 0) {
        AVChannelLayout layout = {0};
        if (frame->ch_layout.nb_channels < 1 || frame->sample_rate < 1 ||
            frame->nb_samples < 1 || frame->nb_samples > 1048576) {
            av_frame_unref(frame);
            return AVERROR_INVALIDDATA;
        }
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
        else if (av_channel_layout_copy(&layout, &frame->ch_layout) < 0)
            return AVERROR(ENOMEM);
        if (!*resampler || *previous_rate != frame->sample_rate ||
            *previous_format != frame->format ||
            av_channel_layout_compare(previous_layout, &layout)) {
            swr_free(resampler);
            av_channel_layout_uninit(previous_layout);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            ret = swr_alloc_set_opts2(resampler, &stereo, AV_SAMPLE_FMT_FLT, SAMPLE_RATE,
                                      &layout, frame->format, frame->sample_rate, 0, NULL);
            if (ret >= 0)
                ret = swr_init(*resampler);
            if (ret >= 0)
                ret = av_channel_layout_copy(previous_layout, &layout);
            *previous_rate = frame->sample_rate;
            *previous_format = frame->format;
            if (ret < 0) {
                av_channel_layout_uninit(&layout);
                return ret;
            }
        }
        av_channel_layout_uninit(&layout);
        int64_t needed = av_rescale_rnd(swr_get_delay(*resampler, frame->sample_rate) +
                                        frame->nb_samples, SAMPLE_RATE,
                                        frame->sample_rate, AV_ROUND_UP);
        if (needed < 1 || needed > 4194304)
            return AVERROR_INVALIDDATA;
        av_fast_malloc(pcm, pcm_capacity, (size_t)needed * CHANNELS * sizeof(float));
        if (!*pcm)
            return AVERROR(ENOMEM);
        int converted = swr_convert(*resampler, pcm, (int)needed,
                                    (const uint8_t **)frame->extended_data, frame->nb_samples);
        av_frame_unref(frame);
        if (converted < 0)
            return converted;
        if (converted) {
            buffer_audio(source, (float *)*pcm, (size_t)converted);
            source->deadline = monotonic_ns() +
                (int64_t)(source->config->timeout_seconds * 1e9);
        }
        if (stopping())
            return AVERROR_EXIT;
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

static int play_connection(Source *source)
{
    AVFormatContext *format = avformat_alloc_context();
    AVCodecContext *decoder = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwrContext *resampler = NULL;
    AVChannelLayout previous_layout = {0};
    int previous_rate = 0, previous_format = -1;
    uint8_t *pcm = NULL;
    unsigned int pcm_capacity = 0;
    int ret = AVERROR(ENOMEM);
    if (!format || !packet || !frame)
        goto done;
    source->deadline = monotonic_ns() + (int64_t)(source->config->timeout_seconds * 1e9);
    format->interrupt_callback = (AVIOInterruptCB){ interrupted, source };
    format->io_open = open_input_io;
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
    av_dict_set(&options, "user_agent", "permastream/" VERSION, 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    av_dict_set_int(&options, "rw_timeout",
                    (int64_t)(source->config->timeout_seconds * 1e6), 0);
    av_dict_set_int(&options, "analyzeduration", 1000000, 0);
    av_dict_set_int(&options, "probesize", 131072, 0);
    ret = avformat_open_input(&format, source->config->urls[source->index], NULL, &options);
    av_dict_free(&options);
    if (ret < 0)
        goto done;
    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0)
        goto done;
    const AVCodec *codec = NULL;
    int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (audio < 0) {
        ret = audio;
        goto done;
    }
    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    ret = avcodec_parameters_to_context(decoder, format->streams[audio]->codecpar);
    if (ret < 0)
        goto done;
    decoder->thread_count = 1;
    ret = avcodec_open2(decoder, codec, NULL);
    if (ret < 0)
        goto done;
    while (!interrupted(source)) {
        ret = av_read_frame(format, packet);
        if (ret < 0)
            break;
        if (packet->stream_index == audio) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret >= 0)
                ret = receive_audio(source, decoder, frame, &resampler, &previous_layout,
                                     &previous_rate, &previous_format, &pcm, &pcm_capacity);
        }
        av_packet_unref(packet);
        if (ret < 0)
            break;
    }
    if (ret >= 0)
        ret = AVERROR_EXIT;
done:
    if (ret < 0 && source->deadline && monotonic_ns() >= source->deadline && !stopping())
        ret = AVERROR(ETIMEDOUT);
    av_free(pcm);
    av_channel_layout_uninit(&previous_layout);
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return ret;
}

static void *source_worker(void *opaque)
{
    Source *source = opaque;
    double retry = source->config->retry_seconds;
    while (!stopping()) {
        log_message("Source %d: connecting", source->index + 1);
        int ret = play_connection(source);
        pthread_mutex_lock(&source->mutex);
        bool stable = source->online && monotonic_ns() - source->healthy_since >=
            (int64_t)(source->config->recovery_seconds * 1e9);
        source->online = false;
        source->healthy_since = 0;
        pthread_mutex_unlock(&source->mutex);
        if (stopping())
            break;
        if (stable)
            retry = source->config->retry_seconds;
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error, sizeof(error));
        log_message("Source %d: %s; retry in %.1fs", source->index + 1, error, retry);
        sleep_until(monotonic_ns() + (int64_t)(retry * 1e9));
        retry = fmin(retry * 2, source->config->retry_max_seconds);
    }
    return NULL;
}

int sources_start(Source *sources, const Config *config)
{
    for (int i = 0; i < config->source_count; i++) {
        Source *source = &sources[i];
        pthread_mutex_init(&source->mutex, NULL);
        source->config = config;
        source->index = i;
        source->target = (size_t)(config->buffer_seconds * SAMPLE_RATE);
        source->capacity = source->target + SAMPLE_RATE;
    }
    for (int i = 0; i < config->source_count; i++) {
        Source *source = &sources[i];
        source->samples = calloc(source->capacity * CHANNELS, sizeof(float));
        if (!source->samples || pthread_create(&source->thread, NULL, source_worker, source))
            return -1;
        source->started = true;
    }
    return 0;
}

void sources_stop(Source *sources, int count)
{
    for (int i = 0; i < count; i++) {
        if (sources[i].started)
            pthread_join(sources[i].thread, NULL);
        free(sources[i].samples);
        pthread_mutex_destroy(&sources[i].mutex);
    }
}

int sources_read(Source *sources, const Config *config, float *pcm, int current)
{
    bool usable[MAX_SOURCES] = {0}, online[MAX_SOURCES] = {0};
    bool recovered[MAX_SOURCES] = {0};
    int64_t now = monotonic_ns();
    for (int i = 0; i < config->source_count; i++) {
        Source *source = &sources[i];
        pthread_mutex_lock(&source->mutex);
        usable[i] = source->ready && source->count >= AUDIO_FRAMES;
        online[i] = source->online;
        recovered[i] = source->online && now - source->healthy_since >=
            (int64_t)(config->recovery_seconds * 1e9);
        pthread_mutex_unlock(&source->mutex);
    }
    int selected = current;
    if (selected < 0 || !usable[selected]) {
        selected = -1;
        for (int i = 0; i < config->source_count; i++)
            if (usable[i] && online[i]) {
                selected = i;
                break;
            }
    } else {
        for (int i = 0; i < selected; i++)
            if (usable[i] && recovered[i]) {
                selected = i;
                break;
            }
    }
    memset(pcm, 0, AUDIO_FRAMES * CHANNELS * sizeof(float));
    atomic_store(&selected_source, selected);
    if (selected >= 0) {
        Source *source = &sources[selected];
        pthread_mutex_lock(&source->mutex);
        /* Reconnection may reset a standby between selection and this lock. */
        if (source->count >= AUDIO_FRAMES && source->ready) {
            size_t first = source->capacity - source->head;
            if (first > AUDIO_FRAMES)
                first = AUDIO_FRAMES;
            memcpy(pcm, source->samples + source->head * CHANNELS,
                   first * CHANNELS * sizeof(float));
            memcpy(pcm + first * CHANNELS, source->samples,
                   (AUDIO_FRAMES - first) * CHANNELS * sizeof(float));
            source->head = (source->head + AUDIO_FRAMES) % source->capacity;
            source->count -= AUDIO_FRAMES;
        } else {
            selected = -1;
        }
        if (source->count < AUDIO_FRAMES)
            source->ready = false;
        pthread_mutex_unlock(&source->mutex);
    }
    /* An exhausted source must refill before it can be selected again. */
    if (current >= 0 && !usable[current]) {
        pthread_mutex_lock(&sources[current].mutex);
        if (sources[current].count < AUDIO_FRAMES)
            sources[current].ready = false;
        pthread_mutex_unlock(&sources[current].mutex);
    }
    atomic_store(&selected_source, selected);
    return selected;
}
