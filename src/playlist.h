#ifndef PERMASTREAM_PLAYLIST_H
#define PERMASTREAM_PLAYLIST_H

#include "permastream.h"
#include <libavformat/avformat.h>

#define PLAYLIST_ENTRIES 64
#define PLAYLIST_DEPTH 4

typedef struct {
    char *url;
    char *entries[PLAYLIST_ENTRIES];
    int count;
    bool hls;
    AVIOContext *transport, *replay;
    uint8_t *prefix;
    size_t length, position;
} PlaylistInput;

/* A direct stream keeps its original connection and all inspected bytes.
 * A radio playlist supplies ordered URLs. HLS is left to FFmpeg's demuxer. */
int playlist_open(PlaylistInput *input, const char *url, Source *source);
void playlist_close(PlaylistInput *input);

#endif
