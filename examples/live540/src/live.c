#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uvg266.h>

enum { WIDTH = 540, HEIGHT = 960, PAD_WIDTH = 544, FPS = 30, CAPACITY = 8 };
static const int64_t second = 1000000000;

typedef struct {
    uvg_picture *picture;
    int64_t due, arrival;
    unsigned depth;
} Frame;

typedef struct {
    FILE *input;
    const uvg_api *api;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    Frame slots[CAPACITY];
    unsigned head, count;
    bool done, failed, stop;
    int64_t start;
} Source;

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (int64_t)ts.tv_sec * second + ts.tv_nsec;
}

static void wait_until(int64_t deadline)
{
    for (;;) {
        int64_t left = deadline - now_ns();
        if (left <= 0) return;
        struct timespec delay = { left / second, left % second };
        if (nanosleep(&delay, NULL) && errno != EINTR) { perror("nanosleep"); exit(1); }
    }
}

// uvg266's 8-bit 4:2:0 input uses an 8-pixel conformance grid.
static int read_picture(FILE *input, uvg_picture *pic)
{
    for (int p = 0; p < 3; ++p) {
        int w = p ? WIDTH / 2 : WIDTH;
        int h = p ? HEIGHT / 2 : HEIGHT;
        int stride = p ? pic->stride / 2 : pic->stride;
        for (int y = 0; y < h; ++y) {
            uint8_t *row = pic->data[p] + y * stride;
            size_t n = fread(row, 1, w, input);
            if (n != (size_t)w) {
                if (!n && !p && !y && feof(input) && !ferror(input)) return 0;
                fprintf(stderr, "Incomplete I420 frame or input read error\n");
                return -1;
            }
            memset(row + w, row[w - 1], stride - w);
        }
    }
    return 1;
}

static void *feed(void *arg)
{
    Source *s = arg;
    bool failed = false;
    for (int64_t id = 0;; ++id) {
        uvg_picture *pic = s->api->picture_alloc(PAD_WIDTH, HEIGHT);
        if (!pic) { fprintf(stderr, "Picture allocation failed\n"); failed = true; break; }
        int result = read_picture(s->input, pic);
        if (result != 1) {
            s->api->picture_free(pic);
            failed = result < 0;
            break;
        }
        pic->pts = id;
        if (!id) s->start = now_ns();
        int64_t due = s->start + (id / FPS) * second + (id % FPS) * second / FPS;
        wait_until(due);
        pthread_mutex_lock(&s->mutex);
        if (s->stop || s->count == CAPACITY) {
            failed = !s->stop;
            if (failed) fprintf(stderr, "Input queue full at PTS %" PRId64 "\n", id);
            pthread_mutex_unlock(&s->mutex);
            s->api->picture_free(pic);
            break;
        }
        unsigned tail = (s->head + s->count) % CAPACITY;
        s->slots[tail] = (Frame){pic, due, now_ns(), s->count};
        ++s->count;
        pthread_cond_signal(&s->changed);
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_lock(&s->mutex);
    s->done = true;
    s->failed = failed;
    pthread_cond_signal(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static bool receive(Source *s, Frame *frame)
{
    pthread_mutex_lock(&s->mutex);
    while (!s->count && !s->done) pthread_cond_wait(&s->changed, &s->mutex);
    bool available = s->count != 0;
    if (available) {
        *frame = s->slots[s->head];
        s->head = (s->head + 1) % CAPACITY;
        --s->count;
    }
    pthread_mutex_unlock(&s->mutex);
    return available;
}

static uvg_encoder *open_encoder(const uvg_api *api)
{
    uvg_config *cfg = api->config_alloc();
    uvg_encoder *enc = NULL;
    if (cfg && api->config_init(cfg) && api->config_parse(cfg, "preset", "ultrafast")
        && api->config_parse(cfg, "gop", "lp-g4d3t1")) {
        cfg->width = WIDTH;
        cfg->height = HEIGHT;
        cfg->framerate_num = FPS;
        cfg->framerate_denom = 1;
        cfg->intra_period = 60;
        cfg->vps_period = 1;
        cfg->open_gop = 0;
        cfg->threads = 4;
        cfg->owf = 0;
        cfg->wpp = 1;
        cfg->qp = 32;
        cfg->calc_psnr = 0;
        enc = api->encoder_open(cfg);
    }
    if (cfg) api->config_destroy(cfg);
    return enc;
}

static bool write_au(FILE *out, const uvg_data_chunk *chunk, uint32_t expected)
{
    size_t written = 0;
    for (; chunk; chunk = chunk->next) {
        if (fwrite(chunk->data, 1, chunk->len, out) != chunk->len) return false;
        written += chunk->len;
    }
    return written == expected;
}

static bool encode(Source *s, uvg_encoder *enc, FILE *out, FILE *trace)
{
    if (fprintf(trace, "pts,nal_type,slice_type,bytes,queue_ahead,due_ns,arrival_ns,submit_ns,ready_ns,latency_ms,submit_to_ready_ms,gap_ms\n") < 0)
        return false;
    int64_t previous = 0;
    int64_t frames = 0;
    for (;;) {
        Frame f = {0};
        bool have_input = receive(s, &f);
        uvg_data_chunk *chunks = NULL;
        uvg_picture *source = NULL;
        uint32_t bytes = 0;
        uvg_frame_info info = {0};
        int64_t submit = now_ns();
        bool ok = s->api->encoder_encode(enc, f.picture, &chunks, &bytes, NULL, &source, &info);
        int64_t ready = now_ns();
        // OWF=0 and a past-only GOP require the same input PTS back in this call.
        // Fail explicitly if a future configuration introduces pipeline buffering.
        if (have_input) {
            ok = ok && source && chunks && bytes && source->pts == f.picture->pts;
            if (ok) {
                char gap[48] = "";
                if (frames) snprintf(gap, sizeof gap, "%.6f", (ready - previous) / 1e6);
                ok = write_au(out, chunks, bytes) &&
                    fprintf(trace, "%" PRId64 ",%d,%d,%u,%u,%" PRId64 ",%" PRId64
                            ",%" PRId64 ",%" PRId64 ",%.6f,%.6f,%s\n",
                            source->pts, info.nal_unit_type, info.slice_type, bytes, f.depth,
                            f.due - s->start, f.arrival - s->start, submit - s->start,
                            ready - s->start, (ready - f.arrival) / 1e6,
                            (ready - submit) / 1e6, gap) > 0;
                previous = ready;
                ++frames;
            }
        } else {
            ok = ok && !chunks && !source && !bytes && frames > 0 && !s->failed;
        }
        s->api->picture_free(source);
        s->api->picture_free(f.picture);
        s->api->chunk_free(chunks);
        if (!ok) fprintf(stderr, "Invalid input, AU/PTS mismatch or output failure after %" PRId64 " frames\n", frames);
        if (!ok || !have_input) return ok;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s input.i420 output.vvc frames.csv\n", argv[0]);
        return 2;
    }
    Source s = { .api = uvg_api_get(8), .mutex = PTHREAD_MUTEX_INITIALIZER,
                 .changed = PTHREAD_COND_INITIALIZER };
    s.input = fopen(argv[1], "rb");
    if (!s.input) { perror(argv[1]); return 1; }
    FILE *out = fopen(argv[2], "wbx");
    FILE *trace = out ? fopen(argv[3], "wx") : NULL;
    if (!out || !trace) perror(!out ? argv[2] : argv[3]);
    uvg_encoder *enc = trace && s.api ? open_encoder(s.api) : NULL;
    bool ok = false;
    if (enc) {
        pthread_t thread;
        int err = pthread_create(&thread, NULL, feed, &s);
        if (err) fprintf(stderr, "Starting source: %s\n", strerror(err));
        else {
            ok = encode(&s, enc, out, trace);
            pthread_mutex_lock(&s.mutex);
            s.stop = true;
            pthread_mutex_unlock(&s.mutex);
            pthread_join(thread, NULL);
            Frame f;
            while (receive(&s, &f)) s.api->picture_free(f.picture);
        }
        s.api->encoder_close(enc);
    }
    if (out && fclose(out)) { perror(argv[2]); ok = false; }
    if (trace && fclose(trace)) { perror(argv[3]); ok = false; }
    fclose(s.input);
    pthread_cond_destroy(&s.changed);
    pthread_mutex_destroy(&s.mutex);
    return ok ? 0 : 1;
}
