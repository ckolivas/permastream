#include "playlist.h"

#include <libavutil/opt.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOCUMENT_LIMIT (256 * 1024)
#define URL_LIMIT 8192
#define PROBE_SIZE 4096

static int interrupted(void *opaque)
{
    Source *source = opaque;
    return stopping() || monotonic_ns() >= source->deadline;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1]))
        text[--n] = '\0';
    return text;
}

static bool remote_url(const char *url)
{
    return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
}

/* RFC 3986 dot-segment removal, preserving repeated slashes and query bytes. */
static void normalize_path(char *path)
{
    char *in = path, *out = path;
    while (*in) {
        if (!strncmp(in, "../", 3))
            in += 3;
        else if (!strncmp(in, "./", 2))
            in += 2;
        else if (!strncmp(in, "/./", 3))
            in += 2;
        else if (!strcmp(in, "/.")) {
            *out++ = '/';
            break;
        } else if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
            bool last = !strcmp(in, "/..");
            in += 3;
            while (out > path && out[-1] != '/')
                out--;
            if (out > path)
                out--;
            if (last) {
                *out++ = '/';
                break;
            }
        } else if (!strcmp(in, ".") || !strcmp(in, ".."))
            break;
        else {
            if (*in == '/')
                *out++ = *in++;
            while (*in && *in != '/')
                *out++ = *in++;
        }
    }
    *out = '\0';
}

static char *resolve_url(const char *base, const char *reference)
{
    size_t length = strcspn(reference, "#");
    if (!length || length > URL_LIMIT)
        return NULL;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)reference[i] <= 32 || (unsigned char)reference[i] == 127)
            return NULL;
    char *ref = strndup(reference, length);
    if (!ref)
        return NULL;
    if (remote_url(ref))
        return ref;
    /* Relative references are meaningful only inside a remote playlist. */
    if (!remote_url(base)) {
        free(ref);
        return NULL;
    }
    char *scheme = strstr(base, "://");
    const char *authority_end = scheme + 3 + strcspn(scheme + 3, "/?#");
    size_t origin_length = (size_t)(authority_end - base);
    char *result = NULL;
    if (!strncmp(ref, "//", 2)) {
        if (asprintf(&result, "%.*s%s", (int)(scheme + 1 - base), base, ref) < 0)
            result = NULL;
    } else {
        /* Disallow file:, data:, and other scheme-bearing references. */
        const char *colon = strchr(ref, ':');
        if (colon && colon < ref + strcspn(ref, "/?")) {
            free(ref);
            return NULL;
        }
        size_t base_path_length = strcspn(authority_end, "?#");
        char *path = NULL;
        if (*ref == '/')
            path = strdup(ref);
        else if (*ref == '?') {
            if (asprintf(&path, "%.*s%s%s", (int)base_path_length, authority_end,
                         base_path_length ? "" : "/", ref) < 0)
                path = NULL;
        } else {
            while (base_path_length && authority_end[base_path_length - 1] != '/')
                base_path_length--;
            if (asprintf(&path, "%.*s%s%s", (int)base_path_length, authority_end,
                         base_path_length ? "" : "/", ref) < 0)
                path = NULL;
        }
        if (path) {
            char *query = strchr(path, '?');
            char *saved_query = query ? strdup(query) : NULL;
            if (!query || saved_query) {
                if (query)
                    *query = '\0';
                normalize_path(path);
                if (asprintf(&result, "%.*s%s%s", (int)origin_length, base, path,
                             saved_query ? saved_query : "") < 0)
                    result = NULL;
            }
            free(saved_query);
            free(path);
        }
    }
    free(ref);
    if (result && strlen(result) > URL_LIMIT) {
        free(result);
        result = NULL;
    }
    return result;
}

static int parse_playlist(PlaylistInput *input, bool local)
{
    if (memchr(input->prefix, 0, input->length))
        return AVERROR_INVALIDDATA;
    char *text = (char *)input->prefix;
    if (input->length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3))
        text += 3;
    text = trim(text);
    bool pls = !strncasecmp(text, "[playlist]", 10);
    if (local && !pls)
        return AVERROR_INVALIDDATA;
    /* HLS is a media protocol, not a list of alternative radio endpoints. */
    if (!pls && strstr(text, "#EXT-X-")) {
        input->hls = true;
        return 0;
    }
    char *slots[PLAYLIST_ENTRIES] = {0};
    char *save = NULL;
    int result = 0, sequential = 0;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        line = trim(line);
        if (!*line || *line == '#' || *line == ';')
            continue;
        int index;
        if (pls) {
            if (strncasecmp(line, "File", 4))
                continue;
            char *end;
            errno = 0;
            long number = strtol(line + 4, &end, 10);
            if (end == line + 4)
                continue;
            end = trim(end);
            if (*end != '=' || errno || number < 1 || number > PLAYLIST_ENTRIES) {
                result = AVERROR_INVALIDDATA;
                break;
            }
            index = (int)number - 1;
            line = trim(end + 1);
        } else {
            if (sequential == PLAYLIST_ENTRIES) {
                result = AVERROR(E2BIG);
                break;
            }
            index = sequential++;
        }
        if (slots[index]) {
            result = AVERROR_INVALIDDATA;
            break;
        }
        slots[index] = resolve_url(input->url, line);
        /* Unsupported entries (e.g. file://) must not block later radio URLs. */
    }
    for (int i = 0; i < PLAYLIST_ENTRIES; i++)
        if (slots[i])
            input->entries[input->count++] = slots[i];
    return result < 0 ? result : input->count ? 0 : AVERROR_INVALIDDATA;
}

static int replay_read(void *opaque, uint8_t *buffer, int size)
{
    PlaylistInput *input = opaque;
    if (input->position < input->length) {
        size_t n = input->length - input->position;
        if (n > (size_t)size)
            n = (size_t)size;
        memcpy(buffer, input->prefix + input->position, n);
        input->position += n;
        return (int)n;
    }
    int ret = avio_read_partial(input->transport, buffer, size);
    return ret ? ret : AVERROR_EOF;
}

int playlist_open(PlaylistInput *input, const char *url, Source *source)
{
    memset(input, 0, sizeof(*input));
    input->url = strdup(url);
    input->prefix = av_malloc(DOCUMENT_LIMIT + 1);
    if (!input->url || !input->prefix)
        return AVERROR(ENOMEM);
    if (!remote_url(url)) {
        /* Only explicitly configured local PLS files reach this branch. */
        int fd = open(url, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return AVERROR(errno);
        struct stat status;
        int ret = fstat(fd, &status);
        if (ret || !S_ISREG(status.st_mode) || status.st_size > DOCUMENT_LIMIT) {
            close(fd);
            return AVERROR_INVALIDDATA;
        }
        FILE *file = fdopen(fd, "r");
        if (!file) {
            int error = errno;
            close(fd);
            return AVERROR(error);
        }
        input->length = fread(input->prefix, 1, DOCUMENT_LIMIT + 1, file);
        ret = ferror(file) ? AVERROR(EIO) : 0;
        fclose(file);
        if (ret < 0 || input->length > DOCUMENT_LIMIT)
            return ret < 0 ? ret : AVERROR(E2BIG);
        input->prefix[input->length] = 0;
        return parse_playlist(input, true);
    }
    source->deadline = monotonic_ns() + (int64_t)(source->config->timeout_seconds * 1e9);
    AVIOInterruptCB callback = { interrupted, source };
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls", 0);
    av_dict_set(&options, "user_agent", "permastream/" VERSION, 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    av_dict_set_int(&options, "rw_timeout", (int64_t)(source->config->timeout_seconds * 1e6), 0);
    int ret = avio_open2(&input->transport, url, AVIO_FLAG_READ, &callback, &options);
    av_dict_free(&options);
    if (ret < 0)
        return ret;
    uint8_t *effective = NULL;
    if (av_opt_get(input->transport, "location", AV_OPT_SEARCH_CHILDREN, &effective) >= 0 &&
        effective && remote_url((char *)effective)) {
        char *copy = strdup((char *)effective);
        if (!copy) {
            av_free(effective);
            return AVERROR(ENOMEM);
        }
        free(input->url);
        input->url = copy;
    }
    av_free(effective);
    ret = avio_read(input->transport, input->prefix, PROBE_SIZE);
    if (ret <= 0)
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    input->length = (size_t)ret;
    input->prefix[input->length] = 0;
    char *text = (char *)input->prefix;
    if (input->length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3))
        text += 3;
    while (isspace((unsigned char)*text))
        text++;
    uint8_t *mime = NULL;
    av_opt_get(input->transport, "mime_type", AV_OPT_SEARCH_CHILDREN, &mime);
    size_t path_length = strcspn(input->url, "?#");
    bool named_playlist = (path_length >= 4 &&
        (!strncasecmp(input->url + path_length - 4, ".pls", 4) ||
         !strncasecmp(input->url + path_length - 4, ".m3u", 4))) ||
        (path_length >= 5 && !strncasecmp(input->url + path_length - 5, ".m3u8", 5));
    bool playlist = !strncasecmp(text, "[playlist]", 10) || !strncmp(text, "#EXTM3U", 7) ||
        !strncmp(text, "http://", 7) || !strncmp(text, "https://", 8) ||
        named_playlist || (mime && (strstr((char *)mime, "mpegurl") || strstr((char *)mime, "scpls")));
    av_free(mime);
    if (playlist) {
        while (input->length <= DOCUMENT_LIMIT) {
            ret = avio_read_partial(input->transport, input->prefix + input->length,
                                    (int)(DOCUMENT_LIMIT + 1 - input->length));
            if (ret == AVERROR_EOF || ret == 0)
                break;
            if (ret < 0)
                return ret;
            input->length += (size_t)ret;
            if (interrupted(source))
                return AVERROR_EXIT;
        }
        if (input->length > DOCUMENT_LIMIT)
            return AVERROR(E2BIG);
        input->prefix[input->length] = 0;
        avio_closep(&input->transport);
        return parse_playlist(input, false);
    }
    uint8_t *buffer = av_malloc(32768);
    if (!buffer)
        return AVERROR(ENOMEM);
    input->replay = avio_alloc_context(buffer, 32768, 0, input, replay_read, NULL, NULL);
    if (!input->replay) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    return 0;
}

void playlist_close(PlaylistInput *input)
{
    if (input->replay) {
        av_freep(&input->replay->buffer);
        avio_context_free(&input->replay);
    }
    avio_closep(&input->transport);
    av_free(input->prefix);
    free(input->url);
    for (int i = 0; i < input->count; i++)
        free(input->entries[i]);
}
