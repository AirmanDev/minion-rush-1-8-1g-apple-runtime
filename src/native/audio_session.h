#ifndef MR_AUDIO_SESSION_H
#define MR_AUDIO_SESSION_H

int mr_audio_session_begin(void); // Returns zero on success.
void mr_audio_session_end(void);

enum {
    MR_AUDIO_SESSION_MEDIA_RESET = 1u << 0,
};

unsigned mr_audio_session_poll(void);

#endif
