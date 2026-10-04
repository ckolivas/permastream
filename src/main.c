#include "permastream.h"

#include <libavformat/avformat.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stop_signal;
atomic_bool stop_requested;
atomic_int selected_source = -1;

bool stopping(void)
{
    return stop_signal || atomic_load(&stop_requested);
}

int64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

void sleep_until(int64_t deadline)
{
    while (!stopping()) {
        int64_t now = monotonic_ns();
        if (now >= deadline)
            return;
        int64_t next = deadline - now > 100000000 ? now + 100000000 : deadline;
        struct timespec ts = { .tv_sec = (time_t)(next / 1000000000),
                               .tv_nsec = (long)(next % 1000000000) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
}

void log_message(const char *format, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    flockfile(stderr);
    fprintf(stderr, "%s ", stamp);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    funlockfile(stderr);
}

static void handle_signal(int signal_number)
{
    stop_signal = signal_number;
}

typedef struct {
    int listener;
    Output *outputs;
    const Config *config;
} ServerArgs;

static void *http_worker(void *opaque)
{
    ServerArgs *args = opaque;
    server_run(args->listener, args->outputs, args->config);
    return NULL;
}

static void usage(FILE *file)
{
    fprintf(file, "permastream " VERSION " - continuous priority radio relay\n"
            "Usage: permastream [--check] [CONFIG]\n"
            "       permastream --help | --version\n"
            "CONFIG defaults to ./permastream.toml. --check validates without connecting.\n");
}

int main(int argc, char **argv)
{
    bool check = false;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts("permastream " VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--check"))
            check = true;
        else if (argv[i][0] == '-' || path) {
            usage(stderr);
            return 1;
        } else
            path = argv[i];
    }
    Config config;
    if (config_load(path ? path : "permastream.toml", &config))
        return 1;
    if (check) {
        printf("Configuration valid: %d sources, %d outputs\n",
               config.source_count, config.output_count);
        config_free(&config);
        return 0;
    }
    struct sigaction action = { .sa_handler = handle_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    /* Source errors are logged by index without exposing credentials in URLs. */
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();
    Output outputs[MAX_OUTPUTS] = {0};
    Source sources[MAX_SOURCES] = {0};
    int output_count = 0, result = 1;
    bool sources_initialized = false, server_started = false;
    pthread_t server;
    int listener = -1;
    for (int i = 0; i < config.output_count; i++) {
        output_count++;
        if (output_init(&outputs[i], &config.outputs[i], config.client_buffer_seconds)) {
            log_message("Cannot initialize encoder for %s", config.outputs[i].path);
            goto cleanup;
        }
    }
    listener = server_open(config.listen);
    if (listener < 0)
        goto cleanup;
    sources_initialized = true;
    if (sources_start(sources, &config)) {
        log_message("Cannot start source workers");
        goto cleanup;
    }
    ServerArgs args = { listener, outputs, &config };
    if (pthread_create(&server, NULL, http_worker, &args)) {
        log_message("Cannot start HTTP server");
        goto cleanup;
    }
    server_started = true;
    for (int i = 0; i < config.output_count; i++)
        log_message("Serving http://%s%s (%d kbps MP3)", config.listen,
                    config.outputs[i].path, config.outputs[i].bitrate);
    log_message("Output: silence until a source is buffered");
    float pcm[AUDIO_FRAMES * CHANNELS];
    float pending[AUDIO_FRAMES * CHANNELS] = {0};
    int current = -1;
    int64_t origin = monotonic_ns(), frames = 0;
    result = 0;
    while (!stopping()) {
        int next = sources_read(sources, &config, pcm, current);
        for (int i = 0; i < AUDIO_FRAMES * CHANNELS; i++)
            pcm[i] = isfinite(pcm[i]) ? fmaxf(-1, fminf(1, pcm[i])) : 0;
        if (next != current) {
            /* One block of lookahead permits a 10ms fade out and fade in. */
            const int fade = SAMPLE_RATE / 100;
            for (int i = 0; i < fade; i++)
                for (int c = 0; c < CHANNELS; c++) {
                    pending[(AUDIO_FRAMES - fade + i) * CHANNELS + c] *=
                        (float)(fade - 1 - i) / (float)(fade - 1);
                    pcm[i * CHANNELS + c] *= (float)i / (float)(fade - 1);
                }
            if (next < 0)
                log_message("Output: silence; waiting for upstream audio");
            else
                log_message("Output: source %d", next + 1);
            current = next;
        }
        for (int i = 0; i < config.output_count; i++)
            if (output_encode(&outputs[i], pending)) {
                log_message("Encoder failed for %s", config.outputs[i].path);
                result = 1;
                atomic_store(&stop_requested, true);
                break;
            }
        memcpy(pending, pcm, sizeof(pending));
        frames += AUDIO_FRAMES;
        int64_t deadline = origin + (frames / SAMPLE_RATE) * INT64_C(1000000000) +
            (frames % SAMPLE_RATE) * INT64_C(1000000000) / SAMPLE_RATE;
        int64_t now = monotonic_ns();
        if (now - deadline > 250000000) {
            log_message("Output clock delayed; resynchronizing");
            origin = now;
            frames = 0;
            deadline = now;
        }
        sleep_until(deadline);
    }
    if (atomic_load(&stop_requested))
        result = 1;
cleanup:
    atomic_store(&stop_requested, true);
    if (server_started)
        pthread_join(server, NULL);
    if (listener >= 0)
        close(listener);
    if (sources_initialized)
        sources_stop(sources, config.source_count);
    for (int i = 0; i < output_count; i++)
        output_destroy(&outputs[i]);
    config_free(&config);
    avformat_network_deinit();
    log_message("Stopped");
    return result;
}
