#include "permastream.h"
#include "toml.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int keys_allowed(toml_table_t *table, const char *const *allowed)
{
    for (int i = 0; ; i++) {
        const char *key = toml_key_in(table, i);
        if (!key)
            return 0;
        bool found = false;
        for (int j = 0; allowed[j]; j++)
            if (!strcmp(key, allowed[j]))
                found = true;
        if (!found) {
            fprintf(stderr, "Unknown configuration key: %s\n", key);
            return -1;
        }
    }
}

static int number(toml_table_t *table, const char *key, double *value,
                  double minimum, double maximum)
{
    if (!toml_key_exists(table, key))
        return 0;
    toml_datum_t d = toml_double_in(table, key);
    if (!d.ok) {
        toml_datum_t i = toml_int_in(table, key);
        if (i.ok) {
            d.ok = 1;
            d.u.d = (double)i.u.i;
        }
    }
    if (!d.ok || !isfinite(d.u.d) || d.u.d < minimum || d.u.d > maximum) {
        fprintf(stderr, "%s must be a number between %.2f and %.2f\n",
                key, minimum, maximum);
        return -1;
    }
    *value = d.u.d;
    return 0;
}

void config_free(Config *config)
{
    free(config->listen);
    for (int i = 0; i < config->source_count; i++)
        free(config->urls[i]);
    for (int i = 0; i < config->output_count; i++)
        free(config->outputs[i].path);
    memset(config, 0, sizeof(*config));
}

int config_load(const char *path, Config *config)
{
    *config = (Config){ .buffer_seconds = 10, .timeout_seconds = 10,
        .recovery_seconds = 15, .retry_seconds = 1, .retry_max_seconds = 30,
        .client_buffer_seconds = 5, .max_clients = 64 };
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1;
    }
    char error[256];
    toml_table_t *root = toml_parse_file(file, error, sizeof(error));
    fclose(file);
    if (!root) {
        fprintf(stderr, "Invalid TOML: %s\n", error);
        return -1;
    }
    static const char *const keys[] = { "listen", "streams", "outputs",
        "buffer_seconds", "timeout_seconds", "recovery_seconds",
        "retry_seconds", "retry_max_seconds", "client_buffer_seconds",
        "max_clients", NULL };
    if (keys_allowed(root, keys))
        goto fail;
    toml_datum_t listen = toml_string_in(root, "listen");
    if (toml_key_exists(root, "listen") && !listen.ok) {
        fprintf(stderr, "listen must be a string\n");
        goto fail;
    }
    config->listen = listen.ok ? listen.u.s : strdup("0.0.0.0:8642");
    if (!config->listen || !*config->listen)
        goto fail;
    const char *port = strrchr(config->listen, ':');
    if (!port || port == config->listen || !port[1] ||
        strspn(port + 1, "0123456789") != strlen(port + 1) ||
        strlen(port + 1) > 5 || atoi(port + 1) < 1 || atoi(port + 1) > 65535 ||
        strpbrk(config->listen, "\r\n\t /") ||
        (config->listen[0] == '[' ? (port - config->listen < 3 || port[-1] != ']') :
         strchr(config->listen, ':') != port)) {
        fprintf(stderr, "listen must be host:port or [IPv6]:port, with port 1..65535\n");
        goto fail;
    }
    if (number(root, "buffer_seconds", &config->buffer_seconds, 0.1, 120) ||
        number(root, "timeout_seconds", &config->timeout_seconds, 0.1, 300) ||
        number(root, "recovery_seconds", &config->recovery_seconds, 0, 600) ||
        number(root, "retry_seconds", &config->retry_seconds, 0.1, 300) ||
        number(root, "retry_max_seconds", &config->retry_max_seconds, 0.1, 600) ||
        number(root, "client_buffer_seconds", &config->client_buffer_seconds, 0.1, 60))
        goto fail;
    if (config->retry_max_seconds < config->retry_seconds) {
        fprintf(stderr, "retry_max_seconds must be at least retry_seconds\n");
        goto fail;
    }
    if (toml_key_exists(root, "max_clients")) {
        toml_datum_t n = toml_int_in(root, "max_clients");
        if (!n.ok || n.u.i < 1 || n.u.i > 4096) {
            fprintf(stderr, "max_clients must be an integer from 1 to 4096\n");
            goto fail;
        }
        config->max_clients = (int)n.u.i;
    }
    toml_array_t *streams = toml_array_in(root, "streams");
    int count = streams ? toml_array_nelem(streams) : 0;
    if (count < 1 || count > MAX_SOURCES) {
        fprintf(stderr, "streams must contain 1 to %d HTTP(S) URLs or local PLS paths\n", MAX_SOURCES);
        goto fail;
    }
    for (int i = 0; i < count; i++) {
        toml_datum_t url = toml_string_at(streams, i);
        if (!url.ok) {
            fprintf(stderr, "streams entries must be strings\n");
            goto fail;
        }
        config->urls[config->source_count++] = url.u.s;
        bool remote = !strncmp(url.u.s, "http://", 7) || !strncmp(url.u.s, "https://", 8);
        size_t length = strlen(url.u.s);
        bool local_pls = !strchr(url.u.s, ':') && length > 4 &&
            !strcasecmp(url.u.s + length - 4, ".pls");
        if ((!remote && !local_pls) || strpbrk(url.u.s, "\r\n\t") ||
            (remote && strchr(url.u.s, ' '))) {
            fprintf(stderr, "Stream %d needs an HTTP(S) URL or local .pls path\n", i + 1);
            goto fail;
        }
        /* Local PLS paths are relative to the config file, not the shell cwd. */
        if (local_pls && url.u.s[0] != '/') {
            const char *slash = strrchr(path, '/');
            if (slash) {
                char *resolved = NULL;
                if (asprintf(&resolved, "%.*s/%s", (int)(slash - path), path, url.u.s) < 0)
                    goto fail;
                free(config->urls[i]);
                config->urls[i] = resolved;
            }
        }
    }
    toml_array_t *outputs = toml_array_in(root, "outputs");
    count = outputs ? toml_array_nelem(outputs) : 0;
    if (!toml_key_exists(root, "outputs")) {
        config->output_count = 2;
        config->outputs[0] = (OutputConfig){ strdup("/local.mp3"), 320 };
        config->outputs[1] = (OutputConfig){ strdup("/remote.mp3"), 64 };
        if (!config->outputs[0].path || !config->outputs[1].path)
            goto fail;
    } else {
        if (count < 1 || count > MAX_OUTPUTS) {
            fprintf(stderr, "Use 1 to %d [[outputs]] tables\n", MAX_OUTPUTS);
            goto fail;
        }
        for (int i = 0; i < count; i++) {
            toml_table_t *table = toml_table_at(outputs, i);
            static const char *const output_keys[] = { "path", "bitrate", NULL };
            if (!table || keys_allowed(table, output_keys))
                goto fail;
            toml_datum_t p = toml_string_in(table, "path");
            toml_datum_t b = toml_int_in(table, "bitrate");
            if (!p.ok) {
                fprintf(stderr, "Each output needs a path string\n");
                goto fail;
            }
            OutputConfig *out = &config->outputs[config->output_count++];
            out->path = p.u.s;
            if (out->path[0] != '/' || strlen(out->path) > 255 ||
                strspn(out->path, "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(out->path)) {
                fprintf(stderr, "Output paths must start with / and contain only letters, digits, / . _ -\n");
                goto fail;
            }
            static const int rates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
            bool valid = false;
            for (size_t j = 0; j < sizeof(rates) / sizeof(rates[0]); j++)
                if (b.ok && b.u.i == rates[j])
                    valid = true;
            if (!valid) {
                fprintf(stderr, "Each output needs a valid MP3 bitrate in kbps (32..320)\n");
                goto fail;
            }
            out->bitrate = (int)b.u.i;
            for (int j = 0; j < i; j++)
                if (!strcmp(out->path, config->outputs[j].path)) {
                    fprintf(stderr, "Duplicate output path: %s\n", out->path);
                    goto fail;
                }
        }
    }
    toml_free(root);
    return 0;
fail:
    toml_free(root);
    config_free(config);
    return -1;
}
