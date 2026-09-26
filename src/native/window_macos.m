
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <QuartzCore/QuartzCore.h>

#include "platform_window.h"
#include "localization.h"
#include "shim_gl.h"
#include "host_time.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static NSWindow *WIN;
static NSOpenGLView *VIEW;
static NSOpenGLContext *CTX;
static CADisplayLink *DISPLAY_LINK;
static NSObject *LANGUAGE_TARGET;
static int CLOSED;
static NSSize LAST_BACKING_SIZE;
static uint64_t DISPLAY_TICK;
static uint64_t PRESENT_COUNT;
static double COPY_MS, SWAP_MS;
static double DISPLAY_TIMESTAMP;
static double DISPLAY_TARGET_TIMESTAMP;
static double LAST_DELIVERED_TARGET;
static double TARGET_REFRESH_HZ;
static double START_REFRESH_HZ; // Maximum rate to which cadence may recover.
static double MAX_REFRESH_HZ;
static uint64_t CADENCE_DROPS;
static uint64_t CADENCE_RECOVERIES;
static uint64_t CADENCE_WINDOW_FRAMES;
static uint64_t CADENCE_WINDOW_LATE;
static uint64_t CADENCE_CLEAN_WINDOWS;
static uint64_t CADENCE_LATE_WINDOWS;

#define CADENCE_WINDOW 240u    // Engine updates per decision window.
#define CADENCE_LATE_LIMIT 12u // More than 5 percent late is a bad window.
#define CADENCE_LATE_WINDOWS_NEEDED 2u
#define CADENCE_CLEAN_LIMIT 1u // Fewer late frames makes a clean window.
#define CADENCE_CLEAN_WINDOWS_NEEDED 3u
#define CADENCE_MIN_HZ 30.0
#define CADENCE_GAP_PERIODS 8.0

static void swap_drawable(void) {
    [CTX flushBuffer];
}

static void apply_frame_rate(double hz) {
    TARGET_REFRESH_HZ = hz;
    if (!DISPLAY_LINK) return;
    float target = (float)hz;
    [DISPLAY_LINK setPreferredFrameRateRange:CAFrameRateRangeMake(target, target, target)];
}

static void destroy_window(void) {
    if (DISPLAY_LINK) {
        [DISPLAY_LINK invalidate];
        [DISPLAY_LINK release];
        DISPLAY_LINK = nil;
    }
    if (VIEW) [VIEW clearGLContext];
    if (WIN) {
        [WIN close];
        [WIN release];
    }
    CTX = nil;
    VIEW = nil;
    WIN = nil;
    [LANGUAGE_TARGET release];
    LANGUAGE_TARGET = nil;
}

static void configure_language_menu(void);

@interface MRLanguageTarget : NSObject
- (void)selectLanguage:(NSMenuItem *)sender;
@end

@implementation MRLanguageTarget
- (void)selectLanguage:(NSMenuItem *)sender {
    NSNumber *value = sender.representedObject;
    if (!value) return;
    mr_localization_request(value.intValue);
    for (NSMenuItem *item in sender.menu.itemArray)
        item.state = NSControlStateValueOff;
    sender.state = NSControlStateValueOn;
}
@end

static void configure_language_menu(void) {
    if (mr_localization_count() == 0) return;
    if (!LANGUAGE_TARGET) LANGUAGE_TARGET = [[MRLanguageTarget alloc] init];

    NSMenu *menu_bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *application_item = [[[NSMenuItem alloc] initWithTitle:@""
                                                               action:nil
                                                        keyEquivalent:@""] autorelease];
    NSMenu *application_menu = [[[NSMenu alloc] initWithTitle:@"Minion Rush"] autorelease];
    NSString *quit_title = [@"Quit " stringByAppendingString:NSProcessInfo.processInfo.processName];
    [application_menu addItemWithTitle:quit_title action:@selector(terminate:) keyEquivalent:@"q"];
    application_item.submenu = application_menu;
    [menu_bar addItem:application_item];

    NSMenuItem *language_item = [[[NSMenuItem alloc] initWithTitle:@"Language"
                                                            action:nil
                                                     keyEquivalent:@""] autorelease];
    NSMenu *language_menu = [[[NSMenu alloc] initWithTitle:@"Language"] autorelease];
    int selected = mr_localization_current();
    NSMenuItem *original = [language_menu addItemWithTitle:@"English"
                                                    action:@selector(selectLanguage:)
                                             keyEquivalent:@""];
    original.target = LANGUAGE_TARGET;
    original.representedObject = @(MR_LOCALIZATION_ORIGINAL);
    original.state =
        selected == MR_LOCALIZATION_ORIGINAL ? NSControlStateValueOn : NSControlStateValueOff;
    [language_menu addItem:NSMenuItem.separatorItem];
    for (unsigned index = 0; index < mr_localization_count(); index++) {
        const mr_localization_definition *definition = mr_localization_get(index);
        NSMenuItem *item =
            [language_menu addItemWithTitle:[NSString stringWithUTF8String:definition->display_name]
                                     action:@selector(selectLanguage:)
                              keyEquivalent:@""];
        item.target = LANGUAGE_TARGET;
        item.representedObject = @((int)index);
        item.state = selected == (int)index ? NSControlStateValueOn : NSControlStateValueOff;
    }
    language_item.submenu = language_menu;
    [menu_bar addItem:language_item];
    NSApp.mainMenu = menu_bar;
}

@interface MRGameView : NSOpenGLView
@end

@implementation MRGameView
- (void)displayTick:(CADisplayLink *)link {
    DISPLAY_TIMESTAMP = link.timestamp;
    DISPLAY_TARGET_TIMESTAMP = link.targetTimestamp;
    DISPLAY_TICK++;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}
@end

static uint32_t SURFACE_W = 1, SURFACE_H = 1;

#define TOUCH_QUEUE 64
static mr_touch TOUCHES[TOUCH_QUEUE];
static unsigned TOUCH_HEAD, TOUCH_TAIL;
static int INPUT_ENABLED;
static int TRACKING; // The press started inside the content surface.
static int TILT_LEFT, TILT_RIGHT;

static unsigned DROPPED;

static unsigned touch_queue_free(void) {
    return (TOUCH_HEAD + TOUCH_QUEUE - TOUCH_TAIL - 1) % TOUCH_QUEUE;
}

static int queue_touch(int action, int x, int y) {
    unsigned next = (TOUCH_TAIL + 1) % TOUCH_QUEUE;
    if (next == TOUCH_HEAD) {
        DROPPED++;
        return 0;
    }
    TOUCHES[TOUCH_TAIL] = (mr_touch){action, x, y};
    TOUCH_TAIL = next;
    return 1;
}

static int push_mouse_touch(int action, NSPoint window_point) {
    NSRect bounds = [VIEW bounds];
    if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0) return 0;
    NSPoint p = [VIEW convertPoint:window_point fromView:nil];

    // Cocoa uses a bottom-left origin while Android uses top-left.
    double x = p.x / bounds.size.width * (double)SURFACE_W;
    double y = (bounds.size.height - p.y) / bounds.size.height * (double)SURFACE_H;
    if (x < 0.0) x = 0.0;
    if (y < 0.0) y = 0.0;
    if (x > SURFACE_W - 1.0) x = SURFACE_W - 1.0;
    if (y > SURFACE_H - 1.0) y = SURFACE_H - 1.0;

    return queue_touch(action, (int)x, (int)y);
}

enum { SWIPE_PRESS, SWIPE_MOVE, SWIPE_RELEASE, SWIPE_STEPS };

static struct {
    int active;
    int step;
    int dx, dy;
    int queued, qdx, qdy; // New input received during the current swipe.
} SWIPE;

static void push_swipe(int dx, int dy) {
    if (TRACKING) {
        DROPPED++;
        return;
    }
    SWIPE.queued = 1;
    SWIPE.qdx = dx;
    SWIPE.qdy = dy;
}

static void swipe_point(int moved, int *x, int *y) {
    *x = (int)(SURFACE_W / 2);
    *y = (int)(SURFACE_H / 2);
    if (!moved) return;
    *x += SWIPE.dx * (int)(SURFACE_W / 4);
    *y += SWIPE.dy * (int)(SURFACE_H / 4);
}

static void swipe_emit_step(void) {
    int x, y;
    swipe_point(SWIPE.step != SWIPE_PRESS, &x, &y);
    if (!queue_touch(SWIPE.step == SWIPE_PRESS  ? MR_TOUCH_PRESS
                     : SWIPE.step == SWIPE_MOVE ? MR_TOUCH_MOVE
                                                : MR_TOUCH_RELEASE,
                     x, y))
        return;
    if (++SWIPE.step >= SWIPE_STEPS) SWIPE.active = 0;
}

static int swipe_cancel(void) {
    SWIPE.queued = 0;
    if (!SWIPE.active) return 1;

    if (SWIPE.step > SWIPE_PRESS) {
        int x, y;
        swipe_point(SWIPE.step > SWIPE_MOVE, &x, &y);
        if (!queue_touch(MR_TOUCH_RELEASE, x, y)) return 0;
    }
    SWIPE.active = 0;
    SWIPE.step = SWIPE_PRESS;
    return 1;
}

static void swipe_advance(void) {
    if (!SWIPE.active && SWIPE.queued && !TRACKING) {
        SWIPE.active = 1;
        SWIPE.step = SWIPE_PRESS;
        SWIPE.dx = SWIPE.qdx;
        SWIPE.dy = SWIPE.qdy;
        SWIPE.queued = 0;
    }
    if (SWIPE.active) swipe_emit_step();
}

static int handle_keyboard(NSEvent *e);

static int handle_input(NSEvent *e) {
    if (!INPUT_ENABLED) return 0;
    switch ([e type]) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp:
        return handle_keyboard(e);
    case NSEventTypeLeftMouseDown: {
        NSPoint p = [VIEW convertPoint:[e locationInWindow] fromView:nil];
        if (!NSPointInRect(p, [VIEW bounds])) return 0;
        if (!swipe_cancel()) return 0;
        TRACKING = push_mouse_touch(MR_TOUCH_PRESS, [e locationInWindow]);
        return 0;
    }
    case NSEventTypeLeftMouseDragged:
        if (TRACKING && touch_queue_free() > 1)
            push_mouse_touch(MR_TOUCH_MOVE, [e locationInWindow]);
        return 0;
    case NSEventTypeLeftMouseUp:
        if (TRACKING) push_mouse_touch(MR_TOUCH_RELEASE, [e locationInWindow]);
        TRACKING = 0;
        return 0;
    default:
        return 0;
    }
}

static int handle_keyboard(NSEvent *e) {
    NSEventType type = [e type];
    if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp) return 0;

    unsigned short key = [e keyCode];
    int dx = 0, dy = 0;
    switch (key) {
    case kVK_ANSI_W:
        dy = -1;
        break;
    case kVK_ANSI_S:
        dy = 1;
        break;
    case kVK_ANSI_A:
        dx = -1;
        break;
    case kVK_ANSI_D:
        dx = 1;
        break;
    default:
        return 0;
    }

    NSEventModifierFlags modifiers =
        [e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    int system_shortcut = (modifiers & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                        NSEventModifierFlagOption)) != 0;

    if (type == NSEventTypeKeyUp) {
        if (key == kVK_ANSI_A) TILT_LEFT = 0;
        if (key == kVK_ANSI_D) TILT_RIGHT = 0;
        return system_shortcut ? 0 : 1;
    }
    if (system_shortcut) return 0;

    if (key == kVK_ANSI_A) TILT_LEFT = 1;
    if (key == kVK_ANSI_D) TILT_RIGHT = 1;
    if (![e isARepeat]) push_swipe(dx, dy);
    return 1;
}

int mr_win_next_touch(mr_touch *out) {
    if (TOUCH_HEAD == TOUCH_TAIL) return 0;
    *out = TOUCHES[TOUCH_HEAD];
    TOUCH_HEAD = (TOUCH_HEAD + 1) % TOUCH_QUEUE;
    return 1;
}

void mr_win_enable_input(void) {
    INPUT_ENABLED = 1;
}

#define KEYBOARD_TILT_ACCELERATION 5.0f
#define GRAVITY_ACCELERATION 9.80665f

int mr_win_accel(mr_accel *out) {
    if (!out) return 0;
    out->x = -(float)(TILT_RIGHT - TILT_LEFT) * KEYBOARD_TILT_ACCELERATION;
    out->y = 0.0f;
    out->z = GRAVITY_ACCELERATION;
    return 1;
}

unsigned mr_win_dropped_touches(void) {
    return DROPPED;
}

int mr_win_open(uint32_t window_w, uint32_t window_h, uint32_t surface_w, uint32_t surface_h,
                const char *title) {
    SURFACE_W = surface_w ? surface_w : 1;
    SURFACE_H = surface_h ? surface_h : 1;
    CLOSED = INPUT_ENABLED = TRACKING = TILT_LEFT = TILT_RIGHT = 0;
    TOUCH_HEAD = TOUCH_TAIL = DROPPED = 0;
    LAST_BACKING_SIZE = NSZeroSize;
    DISPLAY_TICK = PRESENT_COUNT = 0;
    DISPLAY_TIMESTAMP = DISPLAY_TARGET_TIMESTAMP = LAST_DELIVERED_TARGET = 0.0;
    TARGET_REFRESH_HZ = START_REFRESH_HZ = MAX_REFRESH_HZ = 0.0;
    CADENCE_DROPS = CADENCE_RECOVERIES = 0;
    CADENCE_WINDOW_FRAMES = CADENCE_WINDOW_LATE = 0;
    CADENCE_CLEAN_WINDOWS = CADENCE_LATE_WINDOWS = 0;
    memset(&SWIPE, 0, sizeof SWIPE);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        configure_language_menu();

        NSRect frame = NSMakeRect(0, 0, window_w, window_h);
        WIN = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!WIN) return -1;
        [WIN setReleasedWhenClosed:NO];
        [WIN setTitle:[NSString stringWithUTF8String:title ? title : "Minion Rush"]];
        [WIN center];

        NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAAccelerated,
                                                NSOpenGLPFADoubleBuffer,
                                                NSOpenGLPFAColorSize,
                                                24,
                                                NSOpenGLPFAAlphaSize,
                                                8,
                                                NSOpenGLPFADepthSize,
                                                24,
                                                NSOpenGLPFAStencilSize,
                                                8,
                                                0};
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) {
            destroy_window();
            return -1;
        }

        VIEW = [[MRGameView alloc] initWithFrame:frame pixelFormat:pf];
        [pf release]; // MRGameView retains the pixel format.
        if (!VIEW) {
            destroy_window();
            return -1;
        }
        [VIEW setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [VIEW setWantsBestResolutionOpenGLSurface:YES];
        [WIN setContentView:VIEW];
        [VIEW release];

        CTX = [VIEW openGLContext];
        if (!CTX) {
            destroy_window();
            return -1;
        }

        [CTX makeCurrentContext];

        GLint no_sync = 0;
        [CTX setValues:&no_sync forParameter:NSOpenGLContextParameterSwapInterval];
        GLint applied_sync = 1;
        [CTX getValues:&applied_sync forParameter:NSOpenGLContextParameterSwapInterval];
        if (applied_sync != 0) {
            fprintf(stderr, "ERROR: swap interval cannot be set to 0\n");
            destroy_window();
            return -1;
        }

        [WIN makeKeyAndOrderFront:nil];
        [WIN makeFirstResponder:VIEW];
        [NSApp activateIgnoringOtherApps:YES];

        NSScreen *screen = [WIN screen];
        MAX_REFRESH_HZ = screen ? (double)[screen maximumFramesPerSecond] : 60.0;
        TARGET_REFRESH_HZ = START_REFRESH_HZ = MAX_REFRESH_HZ;

        DISPLAY_LINK = [[VIEW displayLinkWithTarget:VIEW selector:@selector(displayTick:)] retain];
        if (!DISPLAY_LINK) {
            fprintf(stderr, "ERROR: display link cannot be created for the screen\n");
            destroy_window();
            return -1;
        }
        apply_frame_rate(TARGET_REFRESH_HZ);
        [DISPLAY_LINK addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
        return 0;
    }
}

static void pump_events(void) {
    @autoreleasepool {
        for (;;) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:nil
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) break;
            if (!handle_input(e)) [NSApp sendEvent:e];
        }
        if (WIN && ![WIN isKeyWindow]) {
            TILT_LEFT = TILT_RIGHT = 0;
            swipe_cancel();
        }
        if (WIN && ![WIN isVisible] && ![WIN isMiniaturized]) CLOSED = 1;
    }
}

#define IDLE_GUARD_MS 0.35

double mr_win_time_to_tick_ms(void) {
    return mr_win_tick_remaining_ms(DISPLAY_TARGET_TIMESTAMP, CACurrentMediaTime());
}

static double idle_budget_ms(void) {
    double remaining = mr_win_time_to_tick_ms();
    return remaining > IDLE_GUARD_MS ? remaining - IDLE_GUARD_MS : 0.0;
}

static double stepped_cadence(double hz, int step) {
    if (!(MAX_REFRESH_HZ > 0.0) || !(hz > 0.0)) return 0.0;
    long divisor = lround(MAX_REFRESH_HZ / hz) + step;
    if (divisor < 1) return 0.0;
    double candidate = MAX_REFRESH_HZ / (double)divisor;
    if (candidate < CADENCE_MIN_HZ) return 0.0;
    if (step < 0 && candidate > START_REFRESH_HZ) return 0.0;
    return candidate;
}

static void observe_cadence(double frame_ms, double refresh_ms) {
    if (!(refresh_ms > 0.0) || !DISPLAY_LINK) return;

    if (frame_ms > refresh_ms * (double)CADENCE_GAP_PERIODS) {
        CADENCE_WINDOW_FRAMES = 0;
        CADENCE_WINDOW_LATE = 0;
        return;
    }

    CADENCE_WINDOW_FRAMES++;
    if (frame_ms > refresh_ms * 1.5) CADENCE_WINDOW_LATE++;
    if (CADENCE_WINDOW_FRAMES < CADENCE_WINDOW) return;

    if (CADENCE_WINDOW_LATE > CADENCE_LATE_LIMIT) {
        CADENCE_CLEAN_WINDOWS = 0;
        if (++CADENCE_LATE_WINDOWS >= CADENCE_LATE_WINDOWS_NEEDED) {
            double slower = stepped_cadence(TARGET_REFRESH_HZ, +1);
            CADENCE_LATE_WINDOWS = 0;
            if (slower > 0.0) {
                apply_frame_rate(slower);
                CADENCE_DROPS++;
                fprintf(stderr,
                        "[display] %u consecutive windows with %llu+ late frames; "
                        "engine updates reduced to %.0f Hz\n",
                        CADENCE_LATE_WINDOWS_NEEDED, (unsigned long long)CADENCE_LATE_LIMIT,
                        slower);
            }
        }
    } else if (CADENCE_WINDOW_LATE <= CADENCE_CLEAN_LIMIT) {
        CADENCE_LATE_WINDOWS = 0;
        if (++CADENCE_CLEAN_WINDOWS >= CADENCE_CLEAN_WINDOWS_NEEDED) {
            double faster = stepped_cadence(TARGET_REFRESH_HZ, -1);
            CADENCE_CLEAN_WINDOWS = 0;
            if (faster > 0.0) {
                apply_frame_rate(faster);
                CADENCE_RECOVERIES++;
                fprintf(stderr,
                        "[display] %u clean windows; engine updates "
                        "restored to %.0f Hz\n",
                        CADENCE_CLEAN_WINDOWS_NEEDED, faster);
            }
        }
    } else {
        CADENCE_CLEAN_WINDOWS = CADENCE_LATE_WINDOWS = 0;
    }
    CADENCE_WINDOW_FRAMES = 0;
    CADENCE_WINDOW_LATE = 0;
}

int mr_win_wait_frame(mr_frame_timing *out, mr_idle_service_fn idle, void *idle_ctx) {
    if (!DISPLAY_LINK || CLOSED) return 0;
    uint64_t previous_tick = DISPLAY_TICK;
    int idle_was_busy = 1;

    @autoreleasepool {
        while (!CLOSED && DISPLAY_TICK == previous_tick) {
            pump_events();
            if (CLOSED) break;

            double wait_s = idle_was_busy ? 0.001 : 0.004;
            double budget_before_wait = idle_budget_ms();
            if (budget_before_wait > 0.0 && wait_s > budget_before_wait / 1000.0)
                wait_s = budget_before_wait / 1000.0;
            if (wait_s < 0.0001) wait_s = 0.0001;
            NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:wait_s];
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:limit];
            if (CLOSED || DISPLAY_TICK != previous_tick || !idle) continue;

            double budget = idle_budget_ms();
            if (budget <= 0.0) {
                idle_was_busy = 0;
                continue;
            }
            int idle_rc = idle(idle_ctx, budget);
            if (idle_rc < 0) return -1;
            idle_was_busy = idle_rc > 0;
        }
        pump_events();
    }
    if (CLOSED) return 0;

    if (WIN && [WIN isKeyWindow]) swipe_advance();

    double refresh = DISPLAY_TARGET_TIMESTAMP - DISPLAY_TIMESTAMP;
    double frame =
        LAST_DELIVERED_TARGET > 0.0 ? DISPLAY_TARGET_TIMESTAMP - LAST_DELIVERED_TARGET : refresh;
    LAST_DELIVERED_TARGET = DISPLAY_TARGET_TIMESTAMP;
    if (!(refresh > 0.0)) refresh = 0.0;
    if (!(frame > 0.0)) frame = refresh;
    observe_cadence(frame * 1000.0, refresh * 1000.0);
    if (out) {
        out->frame_ms = frame * 1000.0;
        out->refresh_ms = refresh * 1000.0;
    }
    return 1;
}

void mr_win_present(void) {
    if (!CTX) return;
    @autoreleasepool {
        NSRect bounds = [VIEW bounds];
        NSRect backing = [VIEW convertRectToBacking:bounds];
        if (!NSEqualSizes(backing.size, LAST_BACKING_SIZE)) {
            [CTX update];
            backing = [VIEW convertRectToBacking:[VIEW bounds]];
            LAST_BACKING_SIZE = backing.size;
        }
        [CTX makeCurrentContext];

        double t0 = mr_monotonic_ms();
        mr_gl_blit_to_window((uint32_t)backing.size.width, (uint32_t)backing.size.height);
        double t1 = mr_monotonic_ms();
        // Normal execution performs one host present per engine update.
        swap_drawable();
        COPY_MS = t1 - t0;
        SWAP_MS = mr_monotonic_ms() - t1;
        PRESENT_COUNT++;
        mr_gl_restore_target();
    }
}

void mr_win_present_split(double *copy_ms, double *swap_ms) {
    if (copy_ms) *copy_ms = COPY_MS;
    if (swap_ms) *swap_ms = SWAP_MS;
}

void mr_win_request_content_rate(double hz) {
    mr_win_apply_content_rate(hz, MAX_REFRESH_HZ, &START_REFRESH_HZ, apply_frame_rate);
}

double mr_win_display_aspect(void) {
    return 0.0;
}

int mr_win_thermal_state(void) {
    return (int)NSProcessInfo.processInfo.thermalState;
}

double mr_win_refresh_hz(void) {
    return TARGET_REFRESH_HZ;
}
double mr_win_max_refresh_hz(void) {
    return MAX_REFRESH_HZ;
}
uint64_t mr_win_present_count(void) {
    return PRESENT_COUNT;
}
uint64_t mr_win_cadence_drops(void) {
    return CADENCE_DROPS;
}
uint64_t mr_win_cadence_recoveries(void) {
    return CADENCE_RECOVERIES;
}

void mr_win_close(void) {
    @autoreleasepool {
        TILT_LEFT = TILT_RIGHT = TRACKING = 0;
        memset(&SWIPE, 0, sizeof SWIPE);
        destroy_window();
    }
}

void mr_win_preferred_language(char *output, size_t capacity) {
    if (!output || !capacity) return;
    @autoreleasepool {
        NSArray<NSString *> *languages = NSLocale.preferredLanguages;
        NSString *language = languages.count ? languages[0] : @"en";
        const char *value = language.UTF8String;
        snprintf(output, capacity, "%s", value ? value : "en");
    }
}

mr_win_insets mr_win_safe_area(void) {
    return (mr_win_insets){0};
}

void mr_win_surface_size(uint32_t *width, uint32_t *height) {
    if (width) *width = SURFACE_W;
    if (height) *height = SURFACE_H;
}

void mr_win_set_movie_orientation(int landscape) {
    (void)landscape;
}

int mr_win_take_surface_orientation(void) {
    return -1;
}

uint32_t mr_win_take_surface_long_side(void) {
    return 0;
}

void mr_win_set_surface_size(uint32_t width, uint32_t height) {
    SURFACE_W = width ? width : 1u;
    SURFACE_H = height ? height : 1u;
}
