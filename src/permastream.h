#ifndef PERMASTREAM_H
#define PERMASTREAM_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define AUDIO_FRAMES 1152
#define MAX_SOURCES 32
#define MAX_OUTPUTS 8
#define VERSION "0.1.0"

typedef struct {
    char *path;
    int bitrate;
} OutputConfig;

typedef struct {
    char *listen;
    char *urls[MAX_SOURCES];
    int source_count;
    OutputConfig outputs[MAX_OUTPUTS];
    int output_count;
    double buffer_seconds, timeout_seconds, recovery_seconds;
    double retry_seconds, retry_max_seconds, client_buffer_seconds;
    int max_clients;
} Config;

typedef struct {
    pthread_mutex_t mutex;
    float *samples;
    size_t capacity, head, count, target;
    bool ready, online;
    int64_t healthy_since;
    pthread_t thread;
    bool started;
    int index;
    int64_t deadline;
    const Config *config;
} Source;

typedef struct {
    pthread_mutex_t mutex;
    unsigned char *bytes;
    size_t capacity;
    uint64_t end;
    const OutputConfig *config;
    struct AVCodecContext *codec;
    struct AVFrame *frame;
    struct AVPacket *packet;
    int64_t pts;
} Output;

extern volatile sig_atomic_t stop_signal;
extern atomic_bool stop_requested;
extern atomic_int selected_source;

bool stopping(void);
int64_t monotonic_ns(void);
void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
void sleep_until(int64_t deadline);
int config_load(const char *path, Config *config);
void config_free(Config *config);
int sources_start(Source *sources, const Config *config);
void sources_stop(Source *sources, int count);
int sources_read(Source *sources, const Config *config, float *pcm, int current);
int output_init(Output *output, const OutputConfig *config, double buffer_seconds);
int output_encode(Output *output, const float *pcm);
void output_destroy(Output *output);
int server_open(const char *listen_address);
void server_run(int listener, Output *outputs, const Config *config);

#endif
