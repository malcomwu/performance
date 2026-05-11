// jam_ref.c - Online music jam reference
// Build: gcc jam_ref.c -o jam -lopus -lspeexdsp -lpthread -lm -D_GNU_SOURCE
// Run:./jam 127.0.0.1 9000 9001
// Args: <peer_ip> <udp_port> <tcp_port>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <stdatomic.h>
#include <opus/opus.h>
#include <speex/speex_resampler.h>

#define SAMPLE_RATE 48000
#define FRAME_SIZE 960 // 20ms @ 48k
#define CHANNELS 1
#define MAX_PACKET 1500
#define RING_SIZE 256 // power of 2
#define XFADE_SAMPLES 144 // 3ms @ 48k
#define TRIM_PERCENT 5
#define BASE_BUF_FRAMES 3 // 60ms base latency

typedef struct {
    float data[FRAME_SIZE];
    uint32_t ts;
    int samples;
} AudioFrame;

typedef struct {
    AudioFrame buf[RING_SIZE];
    atomic_uint w;
    atomic_uint r;
} RingBuf;

typedef struct {
    int udp_sock, tcp_sock;
    struct sockaddr_in peer_udp;
    OpusEncoder *enc;
    OpusDecoder *dec;
    SpeexResamplerState *rs;
    RingBuf rx_ring;
    atomic_int running;
    double avg_drift;
    uint64_t samples_played;
    float prev_tail[XFADE_SAMPLES];
    int need_trim;
} JamCtx;

static int ring_push(RingBuf *rb, AudioFrame *f) {
    uint w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    uint r = atomic_load_explicit(&rb->r, memory_order_acquire);
    if (w - r >= RING_SIZE) return 0;
    rb->buf[w & (RING_SIZE-1)] = *f;
    atomic_store_explicit(&rb->w, w + 1, memory_order_release);
    return 1;
}

static int ring_pop(RingBuf *rb, AudioFrame *f) {
    uint r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    uint w = atomic_load_explicit(&rb->w, memory_order_acquire);
    if (r == w) return 0;
    *f = rb->buf[r & (RING_SIZE-1)];
    atomic_store_explicit(&rb->r, r + 1, memory_order_release);
    return 1;
}

static void smooth_trim(float *out, float *old, float *new, int n) {
    int m = n < XFADE_SAMPLES? n : XFADE_SAMPLES;
    for (int i = 0; i < m; i++) {
        float t = 0.5f * (1.0f - cosf(M_PI * i / m));
        out[i] = old[i] * (1.0f - t) + new[i] * t;
    }
    for (int i = m; i < n; i++) out[i] = new[i];
}

static void *net_thread(void *arg) {
    JamCtx *ctx = arg;
    fd_set fds;
    uint8_t pkt[MAX_PACKET];
    uint32_t local_ts = 0;

    while (atomic_load(&ctx->running)) {
        FD_ZERO(&fds);
        FD_SET(ctx->udp_sock, &fds);
        FD_SET(ctx->tcp_sock, &fds);
        int maxfd = (ctx->udp_sock > ctx->tcp_sock? ctx->udp_sock : ctx->tcp_sock) + 1;
        struct timeval tv = {0, 10000}; // 10ms

        if (select(maxfd, &fds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(ctx->udp_sock, &fds)) {
                struct sockaddr_in from;
                socklen_t sl = sizeof(from);
                int n = recvfrom(ctx->udp_sock, pkt, MAX_PACKET, 0,
                                (struct sockaddr*)&from, &sl);
                if (n > 4) {
                    AudioFrame f;
                    uint32_t remote_ts;
                    memcpy(&remote_ts, pkt, 4);
                    int decoded = opus_decode_float(ctx->dec, pkt+4, n-4,
                                                   f.data, FRAME_SIZE, 0);
                    if (decoded > 0) {
                        f.samples = decoded;
                        f.ts = remote_ts;
                        int32_t drift = (int32_t)(remote_ts - local_ts);
                        ctx->avg_drift = 0.01*drift + 0.99*ctx->avg_drift;
                        if (!ring_push(&ctx->rx_ring, &f)) ctx->need_trim = 1;
                    }
                }
            }
            if (FD_ISSET(ctx->tcp_sock, &fds)) {
                int client = accept(ctx->tcp_sock, NULL, NULL);
                if (client >= 0) {
                    char cmd[64];
                    int r = recv(client, cmd, 63, 0);
                    if (r > 0) {
                        cmd[r] = 0;
                        if (!strncmp(cmd, "quit", 4)) atomic_store(&ctx->running, 0);
                    }
                    close(client);
                }
            }
        }

        // Send: generate silence for demo, replace with mic input
        float in[FRAME_SIZE] = {0};
        uint8_t out_pkt[MAX_PACKET];
        memcpy(out_pkt, &local_ts, 4);
        int nb = opus_encode_float(ctx->enc, in, FRAME_SIZE, out_pkt+4, MAX_PACKET-4);
        if (nb > 0) sendto(ctx->udp_sock, out_pkt, nb+4, 0,
                          (struct sockaddr*)&ctx->peer_udp, sizeof(ctx->peer_udp));
        local_ts += FRAME_SIZE;
        usleep(20000);
    }
    return NULL;
}

static void audio_callback(JamCtx *ctx, float *out, int nframes) {
    uint r = atomic_load(&ctx->rx_ring.r);
    uint w = atomic_load(&ctx->rx_ring.w);
    int avail = w - r;
    int target = BASE_BUF_FRAMES + (int)(ctx->avg_drift / FRAME_SIZE);
    if (target < 1) target = 1;

    AudioFrame f = {0};
    int got = 0;

    if (avail > target + 2 || ctx->need_trim) {
        // Trim head: skip frames
        int trim = (avail * TRIM_PERCENT) / 100;
        if (trim < 1) trim = 1;
        atomic_fetch_add(&ctx->rx_ring.r, trim);
        ctx->need_trim = 0;
        got = ring_pop(&ctx->rx_ring, &f);
        if (got) { // Smooth from prev_tail
            float tmp[FRAME_SIZE];
            smooth_trim(tmp, ctx->prev_tail, f.data, nframes);
            memcpy(out, tmp, nframes * sizeof(float));
        }
    } else if (avail >= target) {
        got = ring_pop(&ctx->rx_ring, &f);
        memcpy(out, f.data, nframes * sizeof(float));
    } else {
        // PLC: starved
        opus_decode_float(ctx->dec, NULL, 0, out, nframes, 0);
    }

    // Drift comp via resample
    if (got && fabs(ctx->avg_drift) > FRAME_SIZE) {
        double ratio = 1.0 + (ctx->avg_drift / (ctx->samples_played + 1));
        uint32_t in_len = nframes, out_len = nframes;
        float rs_out[FRAME_SIZE*2];
        speex_resampler_process_float(ctx->rs, 0, out, &in_len, rs_out, &out_len);
        if (out_len <= FRAME_SIZE) memcpy(out, rs_out, out_len * sizeof(float));
    }

    memcpy(ctx->prev_tail, &out[nframes - XFADE_SAMPLES], XFADE_SAMPLES * sizeof(float));
    ctx->samples_played += nframes;
}

int main(int argc, char **argv) {
    if (argc!= 4) {
        printf("Use: %s <peer_ip> <udp_port> <tcp_port>\n", argv[0]);
        return 1;
    }
    JamCtx ctx = {0};
    atomic_store(&ctx.running, 1);

    int err;
    ctx.enc = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    ctx.dec = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    opus_encoder_ctl(ctx.enc, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(ctx.enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(ctx.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    ctx.rs = speex_resampler_init(CHANNELS, SAMPLE_RATE, SAMPLE_RATE,
                                  SPEEX_RESAMPLER_QUALITY_DEFAULT, &err);

    ctx.udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ctx.tcp_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(ctx.udp_sock, (struct sockaddr*)&addr, sizeof(addr));

    addr.sin_port = htons(atoi(argv[3]));
    bind(ctx.tcp_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(ctx.tcp_sock, 1);

    ctx.peer_udp.sin_family = AF_INET;
    ctx.peer_udp.sin_port = htons(atoi(argv[2]));
    ctx.peer_udp.sin_addr.s_addr = inet_addr(argv[1]);

    pthread_t net_tid;
    pthread_create(&net_tid, NULL, net_thread, &ctx);

    printf("Running. Connect TCP to port %s and send 'quit' to stop.\n", argv[3]);
    printf("Audio callback simulated. Replace with PortAudio/JACK for real I/O.\n");

    // Demo loop instead of real audio callback
    float dummy[FRAME_SIZE];
    while (atomic_load(&ctx.running)) {
        audio_callback(&ctx, dummy, FRAME_SIZE);
        usleep(20000); // 20ms
    }

    pthread_join(net_tid, NULL);
    opus_encoder_destroy(ctx.enc);
    opus_decoder_destroy(ctx.dec);
    speex_resampler_destroy(ctx.rs);
    close(ctx.udp_sock);
    close(ctx.tcp_sock);
    return 0;
}
