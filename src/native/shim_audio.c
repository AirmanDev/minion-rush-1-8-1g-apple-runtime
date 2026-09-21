
#include "shim_audio.h"
#include "audio_session.h"
#include "shim_libc.h"
#include "guest_threads.h"
#include "host_time.h"

#include <TargetConditionals.h>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "the audio ring requires lock-free 64-bit atomics");

#define SL_RESULT_SUCCESS 0x00000000u
#define SL_RESULT_PARAMETER_INVALID 0x00000002u
#define SL_RESULT_BUFFER_INSUFFICIENT 0x00000007u
#define SL_RESULT_FEATURE_UNSUPPORTED 0x0000000Cu

#define SL_PLAYSTATE_STOPPED 0x00000001u
#define SL_PLAYSTATE_PLAYING 0x00000003u

#define SL_DATAFORMAT_PCM 0x00000002u

#define SL_OBJECT_STATE_REALIZED 2u

#define OBJ_SLOTS 16
#define ENG_SLOTS 24
#define PLAY_SLOTS 16
#define BQ_SLOTS 8

#define MAX_PLAYERS 8u
#define RING_BYTES 65536u // Power of two; about 0.5 seconds at 32 kHz.
#define QUEUE_SLOTS 64u   // Power of two.
#define SLICE_FRAMES 4096u
#define SLICE_BYTES (SLICE_FRAMES * 4u) // At most two 16-bit channels.
#define AUDIO_RETRY_NS UINT64_C(1000000000)

typedef struct {
    int used;
    uint32_t obj, play_itf, bq_itf; // Preallocated guest addresses.
    uint32_t cb, cb_ctx;
    uint32_t cb_tid; // Serial internal OpenSL callback thread.
    uint32_t channels, rate, bits, frame_bytes;
    uint32_t state;

    unsigned char pcm[RING_BYTES];
    _Atomic uint64_t write, read;
    uint32_t qsize[QUEUE_SLOTS];
    _Atomic uint64_t q_write, q_read;
    _Atomic uint64_t played; // Buffers consumed by the audio thread.
    uint64_t fired;          // Guest callbacks dispatched so far.
    uint32_t q_left;

    AudioUnit unit;
    int running;
    _Atomic int active;      // CoreAudio currently expects valid samples.
    _Atomic int starving;    // Counts one event per continuous silent interval.
    int silent;              // Drain locally when audio output is unavailable.
    uint64_t retry_after_ns; // Retry time after a transient CoreAudio failure.
} player;

static player PLAYERS[MAX_PLAYERS];
static _Atomic unsigned UNDERRUNS;
static _Atomic uint64_t SILENCE_US;
static _Atomic uint64_t RENDERED_SAMPLES;
static _Atomic uint64_t SIGNAL_SAMPLES;
static _Atomic uint64_t RENDER_CALLBACKS;
static _Atomic uint64_t SIGNAL_CALLBACKS;
static _Atomic unsigned PEAK_AMPLITUDE;
static unsigned ENQUEUES;
static int DIAGNOSTICS;

static uint32_t ENGINE_OBJ, ENGINE_ITF, MIX_OBJ;
static uint32_t IID_ENGINE, IID_PLAY, IID_BUFQ;

// Helpers.

#define A(i) mr_guest_arg32(c, (unsigned)(i))
#define RET(x) (c->r[0] = (uint32_t)(x))

static player *player_by(uint32_t itf) {
    for (unsigned i = 0; i < MAX_PLAYERS; i++) {
        player *p = &PLAYERS[i];
        if (p->used && (p->obj == itf || p->play_itf == itf || p->bq_itf == itf)) return p;
    }
    return NULL;
}

static void audio_peak_update(unsigned peak) {
    unsigned old = atomic_load_explicit(&PEAK_AMPLITUDE, memory_order_relaxed);
    while (old < peak &&
           !atomic_compare_exchange_weak_explicit(&PEAK_AMPLITUDE, &old, peak, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static void audio_retry_later(player *p) {
    p->silent = 1;
    p->retry_after_ns = mr_mono_ns() + AUDIO_RETRY_NS;
}

static uint32_t queued_bytes(const player *p) {
    uint64_t r = atomic_load_explicit(&p->read, memory_order_acquire);
    uint64_t w = atomic_load_explicit(&p->write, memory_order_acquire);
    return (uint32_t)(w - r);
}

// PCM ring.

static int ring_push(player *p, const unsigned char *src, uint32_t n) {
    uint64_t w = atomic_load_explicit(&p->write, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&p->read, memory_order_acquire);
    uint64_t qw = atomic_load_explicit(&p->q_write, memory_order_relaxed);
    uint64_t qr = atomic_load_explicit(&p->q_read, memory_order_acquire);

    if (n > RING_BYTES - (uint32_t)(w - r) || qw - qr >= QUEUE_SLOTS) return 0;

    uint32_t off = (uint32_t)(w & (RING_BYTES - 1));
    uint32_t first = RING_BYTES - off < n ? RING_BYTES - off : n;
    memcpy(p->pcm + off, src, first);
    memcpy(p->pcm, src + first, n - first);
    atomic_store_explicit(&p->write, w + n, memory_order_release);

    p->qsize[qw & (QUEUE_SLOTS - 1)] = n;
    atomic_store_explicit(&p->q_write, qw + 1, memory_order_release);
    return 1;
}

// Audio-thread consumer. Counting completed buffers limits callback prefetch.
static uint32_t ring_pull(player *p, unsigned char *dst, uint32_t want) {
    uint64_t qw = atomic_load_explicit(&p->q_write, memory_order_acquire);
    uint64_t qr = atomic_load_explicit(&p->q_read, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&p->read, memory_order_relaxed);
    uint64_t done = atomic_load_explicit(&p->played, memory_order_relaxed);
    uint32_t got = 0;

    while (got < want) {
        if (!p->q_left) {
            if (qr == qw) break;
            p->q_left = p->qsize[qr & (QUEUE_SLOTS - 1)];
        }
        uint32_t n = want - got;
        if (n > p->q_left) n = p->q_left;

        uint32_t off = (uint32_t)(rd & (RING_BYTES - 1));
        uint32_t first = RING_BYTES - off < n ? RING_BYTES - off : n;
        memcpy(dst + got, p->pcm + off, first);
        memcpy(dst + got + first, p->pcm, n - first);

        rd += n;
        got += n;
        p->q_left -= n;
        if (!p->q_left) {
            qr++;
            done++;
        }
    }

    atomic_store_explicit(&p->read, rd, memory_order_release);
    atomic_store_explicit(&p->q_read, qr, memory_order_release);
    atomic_store_explicit(&p->played, done, memory_order_release);
    return got;
}

// CoreAudio.

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                       UInt32 bus, UInt32 frames, AudioBufferList *out) {
    (void)flags;
    (void)time;
    (void)bus;
    player *p = (player *)ref;
    float *dst = (float *)out->mBuffers[0].mData;
    uint32_t total = frames * p->channels;

    unsigned char pcm[SLICE_BYTES];
    uint32_t want = frames * p->frame_bytes;
    if (want > sizeof pcm) want = (uint32_t)sizeof pcm;
    uint32_t got = ring_pull(p, pcm, want);

    if (got < want && atomic_load_explicit(&p->active, memory_order_relaxed) &&
        atomic_load_explicit(&p->q_write, memory_order_relaxed)) {
        if (!atomic_exchange_explicit(&p->starving, 1, memory_order_relaxed))
            atomic_fetch_add_explicit(&UNDERRUNS, 1u, memory_order_relaxed);
        uint32_t missing_frames = (want - got) / p->frame_bytes;
        uint64_t missing_us = (uint64_t)missing_frames * UINT64_C(1000000) / p->rate;
        atomic_fetch_add_explicit(&SILENCE_US, missing_us, memory_order_relaxed);
    } else if (got == want) {
        atomic_store_explicit(&p->starving, 0, memory_order_relaxed);
    }

    uint32_t n = got / (p->bits / 8u);
    if (n > total) n = total;
    uint64_t signal = 0;
    unsigned peak = 0;
    if (p->bits == 16) {
        const int16_t *s = (const int16_t *)pcm;
        if (DIAGNOSTICS) {
            for (uint32_t i = 0; i < n; i++) {
                int sample = s[i];
                unsigned amplitude = sample < 0 ? (unsigned)(-sample) : (unsigned)sample;
                if (amplitude) signal++;
                if (amplitude > peak) peak = amplitude;
                dst[i] = (float)sample * (1.0f / 32768.0f);
            }
        } else {
            for (uint32_t i = 0; i < n; i++)
                dst[i] = (float)s[i] * (1.0f / 32768.0f);
        }
    } else if (DIAGNOSTICS) {
        for (uint32_t i = 0; i < n; i++) {
            int sample = (int)pcm[i] - 128;
            unsigned amplitude = sample < 0 ? (unsigned)(-sample) : (unsigned)sample;
            if (amplitude) signal++;
            if (amplitude > peak) peak = amplitude;
            dst[i] = (float)sample * (1.0f / 128.0f);
        }
    } else {
        for (uint32_t i = 0; i < n; i++)
            dst[i] = (float)((int)pcm[i] - 128) * (1.0f / 128.0f);
    }
    for (uint32_t i = n; i < total; i++)
        dst[i] = 0.0f;

    if (DIAGNOSTICS) {
        atomic_fetch_add_explicit(&RENDER_CALLBACKS, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&RENDERED_SAMPLES, n, memory_order_relaxed);
        if (signal) {
            atomic_fetch_add_explicit(&SIGNAL_CALLBACKS, 1u, memory_order_relaxed);
            atomic_fetch_add_explicit(&SIGNAL_SAMPLES, signal, memory_order_relaxed);
            audio_peak_update(peak);
        }
    }
    if (!got || (DIAGNOSTICS && !signal)) {
        if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    return noErr;
}

static int audio_open(player *p) {
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
        .componentSubType = kAudioUnitSubType_RemoteIO,
#else
        .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &p->unit) != noErr) return 0;

    AudioStreamBasicDescription fmt = {
        .mSampleRate = (Float64)p->rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mChannelsPerFrame = p->channels,
        .mBitsPerChannel = 32,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = p->channels * 4u,
        .mBytesPerPacket = p->channels * 4u,
    };
    AURenderCallbackStruct cb = {render, p};

    OSStatus rc = AudioUnitSetProperty(p->unit, kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
    if (rc == noErr)
        rc = AudioUnitSetProperty(p->unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (rc == noErr) rc = AudioUnitInitialize(p->unit);
    if (rc != noErr) {
        AudioComponentInstanceDispose(p->unit);
        p->unit = NULL;
        audio_retry_later(p);
        if (DIAGNOSTICS) fprintf(stderr, "[audio] no output (OSStatus %d)\n", (int)rc);
        return 0;
    }
    p->silent = 0;
    p->retry_after_ns = 0;
    if (DIAGNOSTICS)
        fprintf(stderr, "[audio] output: %u Hz, %u channels, %u bits\n", p->rate, p->channels,
                p->bits);
    return 1;
}

static void audio_close(player *p);

static void audio_sync(player *p) {
    if (p->state != SL_PLAYSTATE_PLAYING) {
        atomic_store_explicit(&p->active, 0, memory_order_release);
        if (p->unit && p->running) AudioOutputUnitStop(p->unit);
        p->running = 0;
        atomic_store_explicit(&p->starving, 0, memory_order_relaxed);
        return;
    }
    if (p->running) return;
    if (p->silent) {
        if (mr_mono_ns() < p->retry_after_ns) return;
        p->silent = 0;
    }
    if (!p->unit && !audio_open(p)) return;

    OSStatus rc = AudioOutputUnitStart(p->unit);
    if (rc == noErr) {
        p->running = 1;
        atomic_store_explicit(&p->active, 1, memory_order_release);
        atomic_store_explicit(&p->starving, 0, memory_order_relaxed);
        p->silent = 0;
        p->retry_after_ns = 0;
        return;
    }

    if (DIAGNOSTICS) fprintf(stderr, "[audio] output cannot start (OSStatus %d)\n", (int)rc);
    audio_close(p);
    audio_retry_later(p);
}

static void audio_close(player *p) {
    atomic_store_explicit(&p->active, 0, memory_order_release);
    atomic_store_explicit(&p->starving, 0, memory_order_relaxed);
    if (!p->unit) return;
    AudioOutputUnitStop(p->unit);
    AudioUnitUninitialize(p->unit);
    AudioComponentInstanceDispose(p->unit);
    p->unit = NULL;
    p->running = 0;
}

// SLObjectItf.

static void t_obj_ok(mr_cpu *c) {
    RET(SL_RESULT_SUCCESS);
}
static void t_unsupported(mr_cpu *c) {
    RET(SL_RESULT_FEATURE_UNSUPPORTED);
}

static void t_obj_getstate(mr_cpu *c) {
    if (A(1)) mr_st32(c, A(1), SL_OBJECT_STATE_REALIZED);
    RET(SL_RESULT_SUCCESS);
}

static void t_obj_destroy(mr_cpu *c) {
    player *p = player_by(A(0));
    if (p) {
        if (p->cb_tid) mr_sched_cancel_callback(c, p->cb_tid);
        audio_close(p);
        uint32_t obj = p->obj, play = p->play_itf, bq = p->bq_itf;
        uint32_t cb_tid = p->cb_tid;
        memset(p, 0, sizeof *p);
        p->obj = obj;
        p->play_itf = play;
        p->bq_itf = bq;
        p->cb_tid = cb_tid;
    }
    RET(SL_RESULT_SUCCESS);
}

// GetInterface(self, iid, ppItf). iid is the value of our data import.
static void t_obj_getinterface(mr_cpu *c) {
    uint32_t self = A(0), iid = A(1), out = A(2);
    if (!out) {
        RET(SL_RESULT_PARAMETER_INVALID);
        return;
    }

    if (self == ENGINE_OBJ && iid == IID_ENGINE) {
        mr_st32(c, out, ENGINE_ITF);
        RET(SL_RESULT_SUCCESS);
        return;
    }
    player *p = player_by(self);
    if (p && iid == IID_PLAY) {
        mr_st32(c, out, p->play_itf);
        RET(SL_RESULT_SUCCESS);
        return;
    }
    if (p && iid == IID_BUFQ) {
        mr_st32(c, out, p->bq_itf);
        RET(SL_RESULT_SUCCESS);
        return;
    }

    mr_st32(c, out, 0);
    RET(SL_RESULT_FEATURE_UNSUPPORTED);
}

// SLEngineItf.

static void t_eng_createoutputmix(mr_cpu *c) {
    if (A(1)) mr_st32(c, A(1), MIX_OBJ);
    RET(SL_RESULT_SUCCESS);
}

static void t_eng_createaudioplayer(mr_cpu *c) {
    uint32_t out = A(1), src = A(2);
    if (!out) {
        RET(SL_RESULT_PARAMETER_INVALID);
        return;
    }

    player *p = NULL;
    for (unsigned i = 0; i < MAX_PLAYERS; i++)
        if (!PLAYERS[i].used) {
            p = &PLAYERS[i];
            break;
        }
    if (!p) {
        mr_st32(c, out, 0);
        RET(SL_RESULT_FEATURE_UNSUPPORTED);
        return;
    }

    uint32_t obj = p->obj, play = p->play_itf, bq = p->bq_itf;
    uint32_t cb_tid = p->cb_tid;
    memset(p, 0, sizeof *p);
    p->obj = obj;
    p->play_itf = play;
    p->bq_itf = bq;
    p->cb_tid = cb_tid;
    p->used = 1;
    p->channels = 2;
    p->rate = 44100;
    p->bits = 16;
    p->state = SL_PLAYSTATE_STOPPED;

    if (src && mr_mem_ok(c, src, 8)) {
        uint32_t fmt = mr_ld32(c, src + 4);
        if (fmt && mr_mem_ok(c, fmt, 16) && mr_ld32(c, fmt) == SL_DATAFORMAT_PCM) {
            p->channels = mr_ld32(c, fmt + 4);
            p->rate = mr_ld32(c, fmt + 8) / 1000u; // Millihertz to hertz.
            p->bits = mr_ld32(c, fmt + 12);
        }
    }
    if (!p->channels || p->channels > 2) p->channels = 2;
    if (p->rate < 8000 || p->rate > 48000) p->rate = 44100;
    if (p->bits != 8 && p->bits != 16) p->bits = 16;
    p->frame_bytes = p->channels * (p->bits / 8u);
    mr_st32(c, out, p->obj);
    if (DIAGNOSTICS)
        fprintf(stderr, "[audio] player: %u Hz, %u channels, %u bits\n", p->rate, p->channels,
                p->bits);
    RET(SL_RESULT_SUCCESS);
}

// SLPlayItf.

static void t_play_setstate(mr_cpu *c) {
    player *p = player_by(A(0));
    if (!p) {
        RET(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (DIAGNOSTICS && p->state != A(1))
        fprintf(stderr, "[audio] player state: %u -> %u\n", p->state, A(1));
    p->state = A(1);
    audio_sync(p);
    RET(SL_RESULT_SUCCESS);
}

static void t_play_getstate(mr_cpu *c) {
    player *p = player_by(A(0));
    if (A(1)) mr_st32(c, A(1), p ? p->state : SL_PLAYSTATE_STOPPED);
    RET(SL_RESULT_SUCCESS);
}

// SLBufferQueueItf.

static void t_bq_enqueue(mr_cpu *c) {
    player *p = player_by(A(0));
    uint32_t data = A(1), size = A(2);
    if (!p || !data || !size || !mr_mem_ok(c, data, size)) {
        RET(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    if (!ring_push(p, (const unsigned char *)mr_mem(c, data), size)) {
        RET(SL_RESULT_BUFFER_INSUFFICIENT);
        return;
    }
    ENQUEUES++;
    audio_sync(p);
    RET(SL_RESULT_SUCCESS);
}

static void t_bq_clear(mr_cpu *c) {
    player *p = player_by(A(0));
    if (p) {
        if (p->cb_tid) mr_sched_cancel_callback(c, p->cb_tid);
        int was_running = p->running;
        atomic_store_explicit(&p->active, 0, memory_order_release);
        atomic_store_explicit(&p->starving, 0, memory_order_relaxed);
        if (p->unit) {
            AudioOutputUnitStop(p->unit);
            p->running = 0;
        }
        atomic_store(&p->write, 0);
        atomic_store(&p->read, 0);
        atomic_store(&p->q_write, 0);
        atomic_store(&p->q_read, 0);
        atomic_store(&p->played, 0);
        p->q_left = 0;
        p->fired = 0;
        if (was_running) audio_sync(p);
    }
    RET(SL_RESULT_SUCCESS);
}

// SLBufferQueueState { count, playIndex }
static void t_bq_getstate(mr_cpu *c) {
    player *p = player_by(A(0));
    uint32_t out = A(1);
    if (out && mr_mem_ok(c, out, 8)) {
        uint64_t qr = p ? atomic_load_explicit(&p->q_read, memory_order_acquire) : 0;
        uint64_t qw = p ? atomic_load_explicit(&p->q_write, memory_order_acquire) : 0;
        mr_st32(c, out, (uint32_t)(qw - qr));
        mr_st32(c, out + 4, (uint32_t)qr);
    }
    RET(SL_RESULT_SUCCESS);
}

static void t_bq_regcb(mr_cpu *c) {
    player *p = player_by(A(0));
    if (p) {
        if (p->cb_tid && A(1) != p->cb) {
            mr_sched_cancel_callback(c, p->cb_tid);
            p->fired = atomic_load(&p->played);
        }
        p->cb = A(1);
        p->cb_ctx = A(2);
        if (DIAGNOSTICS)
            fprintf(stderr, "[audio] buffer-queue callback: cb=0x%08x, ctx=0x%08x\n", p->cb,
                    p->cb_ctx);
    }
    RET(SL_RESULT_SUCCESS);
}

static void t_slCreateEngine(mr_cpu *c) {
    if (A(0)) mr_st32(c, A(0), ENGINE_OBJ);
    RET(SL_RESULT_SUCCESS);
}

static uint32_t NEXT;

static uint32_t put_table(mr_cpu *c, const uint32_t *slots, int n) {
    uint32_t table = NEXT;
    for (int i = 0; i < n; i++)
        mr_st32(c, NEXT + (uint32_t)i * 4, slots[i]);
    NEXT += (uint32_t)n * 4;
    return table;
}

static uint32_t put_itf(mr_cpu *c, uint32_t table) {
    uint32_t itf = NEXT;
    mr_st32(c, itf, table);
    NEXT += 4;
    return itf;
}

uint32_t mr_audio_init(mr_cpu *cpu, uint32_t base,
                       uint32_t (*reg)(const char *, mr_thunk_fn, void *), void *user) {
    NEXT = (base + 7u) & ~7u;
    DIAGNOSTICS = getenv("MR_DIAGNOSTICS") ? 1 : 0;
    if (mr_audio_session_begin() != 0) fprintf(stderr, "[audio] session cannot be opened\n");
    atomic_store_explicit(&UNDERRUNS, 0u, memory_order_relaxed);
    atomic_store_explicit(&SILENCE_US, 0u, memory_order_relaxed);
    atomic_store_explicit(&RENDERED_SAMPLES, 0u, memory_order_relaxed);
    atomic_store_explicit(&SIGNAL_SAMPLES, 0u, memory_order_relaxed);
    atomic_store_explicit(&RENDER_CALLBACKS, 0u, memory_order_relaxed);
    atomic_store_explicit(&SIGNAL_CALLBACKS, 0u, memory_order_relaxed);
    atomic_store_explicit(&PEAK_AMPLITUDE, 0u, memory_order_relaxed);
    ENQUEUES = 0;

    uint32_t unsup = reg("SL::unsupported", t_unsupported, user);

    uint32_t obj[OBJ_SLOTS];
    for (int i = 0; i < OBJ_SLOTS; i++)
        obj[i] = unsup;
    obj[0] = reg("SLObject::Realize", t_obj_ok, user);
    obj[1] = reg("SLObject::Resume", t_obj_ok, user);
    obj[2] = reg("SLObject::GetState", t_obj_getstate, user);
    obj[3] = reg("SLObject::GetInterface", t_obj_getinterface, user);
    obj[4] = reg("SLObject::RegisterCallback", t_obj_ok, user);
    obj[5] = reg("SLObject::AbortAsyncOperation", t_obj_ok, user);
    obj[6] = reg("SLObject::Destroy", t_obj_destroy, user);
    obj[7] = reg("SLObject::SetPriority", t_obj_ok, user);
    uint32_t obj_tbl = put_table(cpu, obj, OBJ_SLOTS);

    uint32_t eng[ENG_SLOTS];
    for (int i = 0; i < ENG_SLOTS; i++)
        eng[i] = unsup;
    eng[2] = reg("SLEngine::CreateAudioPlayer", t_eng_createaudioplayer, user);
    eng[7] = reg("SLEngine::CreateOutputMix", t_eng_createoutputmix, user);
    uint32_t eng_tbl = put_table(cpu, eng, ENG_SLOTS);

    uint32_t play[PLAY_SLOTS];
    for (int i = 0; i < PLAY_SLOTS; i++)
        play[i] = unsup;
    play[0] = reg("SLPlay::SetPlayState", t_play_setstate, user);
    play[1] = reg("SLPlay::GetPlayState", t_play_getstate, user);
    play[4] = reg("SLPlay::RegisterCallback", t_obj_ok, user);
    play[5] = reg("SLPlay::SetCallbackEventsMask", t_obj_ok, user);
    play[10] = reg("SLPlay::SetPositionUpdatePeriod", t_obj_ok, user);
    uint32_t play_tbl = put_table(cpu, play, PLAY_SLOTS);

    uint32_t bq[BQ_SLOTS];
    for (int i = 0; i < BQ_SLOTS; i++)
        bq[i] = unsup;
    bq[0] = reg("SLBufferQueue::Enqueue", t_bq_enqueue, user);
    bq[1] = reg("SLBufferQueue::Clear", t_bq_clear, user);
    bq[2] = reg("SLBufferQueue::GetState", t_bq_getstate, user);
    bq[3] = reg("SLBufferQueue::RegisterCallback", t_bq_regcb, user);
    uint32_t bq_tbl = put_table(cpu, bq, BQ_SLOTS);

    ENGINE_OBJ = put_itf(cpu, obj_tbl);
    ENGINE_ITF = put_itf(cpu, eng_tbl);
    MIX_OBJ = put_itf(cpu, obj_tbl);

    for (unsigned i = 0; i < MAX_PLAYERS; i++) {
        PLAYERS[i].obj = put_itf(cpu, obj_tbl);
        PLAYERS[i].play_itf = put_itf(cpu, play_tbl);
        PLAYERS[i].bq_itf = put_itf(cpu, bq_tbl);
    }

    uint32_t slot;
    if ((slot = mr_shim_data_lookup("SL_IID_ENGINE"))) IID_ENGINE = mr_ld32(cpu, slot);
    if ((slot = mr_shim_data_lookup("SL_IID_PLAY"))) IID_PLAY = mr_ld32(cpu, slot);
    if ((slot = mr_shim_data_lookup("SL_IID_BUFFERQUEUE"))) IID_BUFQ = mr_ld32(cpu, slot);

    return NEXT;
}

mr_thunk_fn mr_audio_lookup(const char *name) {
    return strcmp(name, "slCreateEngine") == 0 ? t_slCreateEngine : NULL;
}

void mr_audio_pump(mr_cpu *cpu) {
    unsigned session_events = mr_audio_session_poll();
    if (session_events & MR_AUDIO_SESSION_MEDIA_RESET) {
        for (unsigned i = 0; i < MAX_PLAYERS; i++) {
            player *p = &PLAYERS[i];
            if (!p->used) continue;
            audio_close(p);
            p->silent = 0;
            p->retry_after_ns = 0;
            audio_sync(p);
        }
    }

    for (unsigned i = 0; i < MAX_PLAYERS; i++) {
        player *p = &PLAYERS[i];
        if (!p->used || !p->cb) continue;

        if (p->silent) {
            uint64_t qw = atomic_load(&p->q_write);
            atomic_store(&p->q_read, qw);
            atomic_store(&p->read, atomic_load(&p->write));
            atomic_store(&p->played, qw);
        }

        if (p->state != SL_PLAYSTATE_PLAYING) continue;
        if (p->fired >= atomic_load(&p->played)) continue;

        uint32_t argv[2] = {p->bq_itf, p->cb_ctx};
        int posted = mr_sched_post_callback(cpu, &p->cb_tid, p->cb, 2, argv);
        if (posted > 0)
            p->fired++;
        else if (posted < 0 && DIAGNOSTICS)
            fprintf(stderr, "[audio] callback thread cannot start\n");
    }
}

mr_audio_stats mr_audio_stats_get(void) {
    mr_audio_stats s = {
        .buffers_played = 0,
        .underruns = atomic_load_explicit(&UNDERRUNS, memory_order_relaxed),
        .enqueues = ENQUEUES,
        .latency_ms = 0,
        .silence_ms = (unsigned)(atomic_load_explicit(&SILENCE_US, memory_order_relaxed) / 1000u),
        .callback_backlog = 0,
        .rendered_samples = atomic_load_explicit(&RENDERED_SAMPLES, memory_order_relaxed),
        .signal_samples = atomic_load_explicit(&SIGNAL_SAMPLES, memory_order_relaxed),
        .render_callbacks = atomic_load_explicit(&RENDER_CALLBACKS, memory_order_relaxed),
        .signal_callbacks = atomic_load_explicit(&SIGNAL_CALLBACKS, memory_order_relaxed),
        .peak_amplitude = atomic_load_explicit(&PEAK_AMPLITUDE, memory_order_relaxed),
    };
    for (unsigned i = 0; i < MAX_PLAYERS; i++) {
        const player *p = &PLAYERS[i];
        if (!p->used || !p->rate) continue;
        uint64_t played = atomic_load_explicit(&p->played, memory_order_acquire);
        s.buffers_played += (unsigned)played;
        if (played > p->fired) s.callback_backlog += (unsigned)(played - p->fired);
        unsigned ms = queued_bytes(p) * 1000u / (p->rate * p->frame_bytes);
        if (ms > s.latency_ms) s.latency_ms = ms;
    }
    return s;
}

void mr_audio_shutdown(void) {
    for (unsigned i = 0; i < MAX_PLAYERS; i++)
        audio_close(&PLAYERS[i]);
    mr_audio_session_end();
}
