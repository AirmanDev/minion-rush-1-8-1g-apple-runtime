#ifndef MR_SHIM_AUDIO_H
#define MR_SHIM_AUDIO_H

#include "cpu.h"

uint32_t mr_audio_init(mr_cpu *cpu, uint32_t base,
                       uint32_t (*reg)(const char *, mr_thunk_fn, void *), void *user);

mr_thunk_fn mr_audio_lookup(const char *name);

void mr_audio_pump(mr_cpu *cpu);

void mr_audio_shutdown(void);

typedef struct {
    unsigned buffers_played;
    unsigned underruns; // Continuous audio-starvation intervals.
    unsigned enqueues;
    unsigned latency_ms; // Duration of queued, unplayed audio.
    unsigned silence_ms; // Output time filled with zero samples.
    unsigned callback_backlog;
    uint64_t rendered_samples; // Samples written to CoreAudio output.
    uint64_t signal_samples;   // Samples differing from digital silence.
    uint64_t render_callbacks;
    uint64_t signal_callbacks;
    unsigned peak_amplitude; // Peak distance from the PCM center value.
} mr_audio_stats;

mr_audio_stats mr_audio_stats_get(void);

#endif
