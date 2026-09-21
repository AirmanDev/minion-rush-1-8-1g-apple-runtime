// AVAudioSession lifecycle for the RemoteIO output.

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "audio_session.h"
#include "host_time.h"

#include <stdatomic.h>
#include <stdio.h>

#define RETRY_NS UINT64_C(500000000)

static atomic_int NEEDS_ACTIVATION;
static atomic_int MEDIA_RESET;
static atomic_uint_fast64_t NEXT_TRY_NS;
static NSMutableArray *OBSERVERS;

static void request_activation(const char *reason) {
    printf("[audio] session reactivation requested: %s\n", reason);
    atomic_store_explicit(&NEXT_TRY_NS, 0, memory_order_relaxed);
    atomic_store_explicit(&NEEDS_ACTIVATION, 1, memory_order_release);
}

static void observe(NSNotificationName name, void (^block)(NSNotification *)) {
    id token = [NSNotificationCenter.defaultCenter addObserverForName:name
                                                               object:nil
                                                                queue:NSOperationQueue.mainQueue
                                                           usingBlock:block];
    [OBSERVERS addObject:token];
}

static int configure_session(void) {
    NSError *error = nil;
    if ([AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback
                                              mode:AVAudioSessionModeDefault
                                           options:0
                                             error:&error])
        return 0;
    fprintf(stderr, "[audio] session category cannot be set: %s\n",
            error.localizedDescription.UTF8String);
    return -1;
}

int mr_audio_session_begin(void) {
    if (OBSERVERS) mr_audio_session_end();
    atomic_store_explicit(&NEEDS_ACTIVATION, 0, memory_order_relaxed);
    atomic_store_explicit(&MEDIA_RESET, 0, memory_order_relaxed);
    atomic_store_explicit(&NEXT_TRY_NS, 0, memory_order_relaxed);

    if (configure_session() != 0) return -1;

    OBSERVERS = [NSMutableArray array];

    observe(AVAudioSessionInterruptionNotification, ^(NSNotification *note) {
      NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
      if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded)
          request_activation("interruption ended");
    });

    observe(AVAudioSessionRouteChangeNotification, ^(NSNotification *note) {
      request_activation("route changed");
    });

    observe(AVAudioSessionMediaServicesWereResetNotification, ^(NSNotification *note) {
      atomic_store_explicit(&MEDIA_RESET, 1, memory_order_release);
      request_activation("media services restarted");
    });

    observe(UIApplicationDidBecomeActiveNotification, ^(NSNotification *note) {
      request_activation("application became active");
    });

    request_activation("startup");
    return 0;
}

unsigned mr_audio_session_poll(void) {
    unsigned events = 0;
    if (atomic_exchange_explicit(&MEDIA_RESET, 0, memory_order_acq_rel)) {
        events |= MR_AUDIO_SESSION_MEDIA_RESET;
        if (configure_session() != 0)
            atomic_store_explicit(&NEEDS_ACTIVATION, 1, memory_order_release);
    }

    if (!atomic_load_explicit(&NEEDS_ACTIVATION, memory_order_acquire)) return events;

    uint64_t now = mr_mono_ns();
    uint64_t next = atomic_load_explicit(&NEXT_TRY_NS, memory_order_relaxed);
    if (now < next) return events;
    atomic_store_explicit(&NEXT_TRY_NS, now + RETRY_NS, memory_order_relaxed);

    NSError *error = nil;
    if (![AVAudioSession.sharedInstance setActive:YES error:&error]) {
        fprintf(stderr, "[audio] session cannot be activated: %s\n",
                error.localizedDescription.UTF8String);
        return events;
    }
    atomic_store_explicit(&NEEDS_ACTIVATION, 0, memory_order_release);
    printf("[audio] session active (%s, %.0f Hz)\n",
           AVAudioSession.sharedInstance.category.UTF8String,
           AVAudioSession.sharedInstance.sampleRate);
    return events;
}

void mr_audio_session_end(void) {
    for (id token in OBSERVERS)
        [NSNotificationCenter.defaultCenter removeObserver:token];
    OBSERVERS = nil;
    [AVAudioSession.sharedInstance setActive:NO
                                 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                       error:nil];
    atomic_store_explicit(&NEEDS_ACTIVATION, 0, memory_order_relaxed);
    atomic_store_explicit(&MEDIA_RESET, 0, memory_order_relaxed);
    atomic_store_explicit(&NEXT_TRY_NS, 0, memory_order_relaxed);
}
