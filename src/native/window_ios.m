#import <CoreMotion/CoreMotion.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES3/gl.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include "gl_context.h"
#include "host_time.h"
#include "platform_window.h"
#include "shim_gl.h"
#include "window_ios.h"

#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

void mr_gl_context_set_window_framebuffer(unsigned framebuffer);

#define TOUCH_QUEUE_CAPACITY 64u
#define ACCELERATION_GRAVITY (-9.80665)
#define ACCELERATION_INTERVAL (1.0 / 60.0)
#define ACTIVE_IDLE_BUSY_WAIT_NS 1000000ll
#define ACTIVE_IDLE_QUIET_WAIT_NS 4000000ll

static os_unfair_lock FRAME_LOCK = OS_UNFAIR_LOCK_INIT;
static dispatch_semaphore_t FRAME_SIGNAL;
static CADisplayLink *DISPLAY_LINK;
static double DISPLAY_TIMESTAMP;
static double DISPLAY_TARGET_TIMESTAMP;
static double LAST_DELIVERED_TARGET;
static double TARGET_REFRESH_HZ;
static double START_REFRESH_HZ;
static double MAX_REFRESH_HZ;
static double LAST_COPY_MS;
static double LAST_SWAP_MS;
static uint64_t PRESENT_COUNT;
static BOOL FRAME_PENDING;
static BOOL SCENE_ACTIVE;
static BOOL CLOSED;

static UIWindowScene *SCENE;
static double SCENE_ASPECT;
static UIWindow *WINDOW;
static UIView *CONTAINER;
static UIView *VIEW;
static void *GL_CONTEXT;
static GLuint WINDOW_FBO;
static GLuint COLOR_RENDERBUFFER;
static GLint DRAWABLE_W;
static GLint DRAWABLE_H;
static atomic_uint SURFACE_W = 1;
static atomic_uint SURFACE_H = 1;
static atomic_bool DRAWABLE_NEEDS_RESIZE;
static atomic_bool MOVIE_LANDSCAPE;
static atomic_bool ORIENTATION_LOCK;
static atomic_int SURFACE_ORIENTATION_REQUEST = ATOMIC_VAR_INIT(-1);
static atomic_uint SAFE_AREA_TOP_PPM;
static atomic_uint SAFE_AREA_BOTTOM_PPM;

static mr_touch TOUCHES[TOUCH_QUEUE_CAPACITY];
static unsigned TOUCH_HEAD;
static unsigned TOUCH_TAIL;
static NSLock *TOUCH_LOCK;
static UITouch *ACTIVE_TOUCH;
static atomic_bool INPUT_ENABLED;
static atomic_uint DROPPED_TOUCHES;

static CMMotionManager *MOTION;
static atomic_int INTERFACE_ORIENTATION;
static void layout_game_view(void);
static void update_orientation_lock(void);

static UIInterfaceOrientationMask game_orientation_mask(void) {
    return atomic_load_explicit(&MOVIE_LANDSCAPE, memory_order_acquire)
               ? UIInterfaceOrientationMaskLandscape
               : UIInterfaceOrientationMaskPortrait;
}

UIInterfaceOrientationMask mr_ios_supported_orientations(void) {
    return game_orientation_mask();
}

static void run_on_main_sync(dispatch_block_t block) {
    if (NSThread.isMainThread) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

static void signal_frame_waiter(void) {
    if (FRAME_SIGNAL) dispatch_semaphore_signal(FRAME_SIGNAL);
}

static BOOL is_closed(void) {
    os_unfair_lock_lock(&FRAME_LOCK);
    BOOL value = CLOSED;
    os_unfair_lock_unlock(&FRAME_LOCK);
    return value;
}

static void queue_touch(int action, int x, int y) {
    [TOUCH_LOCK lock];
    unsigned next = (TOUCH_TAIL + 1u) % TOUCH_QUEUE_CAPACITY;
    if (next == TOUCH_HEAD) {
        TOUCH_HEAD = (TOUCH_HEAD + 1u) % TOUCH_QUEUE_CAPACITY;
        atomic_fetch_add_explicit(&DROPPED_TOUCHES, 1u, memory_order_relaxed);
    }
    TOUCHES[TOUCH_TAIL] = (mr_touch){action, x, y};
    TOUCH_TAIL = next;
    [TOUCH_LOCK unlock];
}

int mr_win_next_touch(mr_touch *out) {
    if (!out || !TOUCH_LOCK) return 0;
    int found = 0;
    [TOUCH_LOCK lock];
    if (TOUCH_HEAD != TOUCH_TAIL) {
        *out = TOUCHES[TOUCH_HEAD];
        TOUCH_HEAD = (TOUCH_HEAD + 1u) % TOUCH_QUEUE_CAPACITY;
        found = 1;
    }
    [TOUCH_LOCK unlock];
    return found;
}

void mr_win_enable_input(void) {
    atomic_store_explicit(&INPUT_ENABLED, true, memory_order_release);
}

unsigned mr_win_dropped_touches(void) {
    return atomic_load_explicit(&DROPPED_TOUCHES, memory_order_relaxed);
}

static void configure_accelerometer(void) {
    if (MOTION) return;
    MOTION = [[CMMotionManager alloc] init];
    if (!MOTION.accelerometerAvailable) {
        MOTION = nil;
        return;
    }
    MOTION.accelerometerUpdateInterval = ACCELERATION_INTERVAL;
}

static void set_accelerometer_active(BOOL active) {
    if (active) configure_accelerometer();
    if (!MOTION) return;
    if (active && !MOTION.accelerometerActive) {
        [MOTION startAccelerometerUpdates];
    } else if (!active && MOTION.accelerometerActive) {
        [MOTION stopAccelerometerUpdates];
    }
}

int mr_win_accel(mr_accel *out) {
    if (!out || !MOTION) return 0;
    CMAccelerometerData *data = MOTION.accelerometerData;
    if (!data) return 0;

    double x = data.acceleration.x;
    double y = data.acceleration.y;
    switch ((UIInterfaceOrientation)atomic_load_explicit(&INTERFACE_ORIENTATION,
                                                         memory_order_relaxed)) {
    case UIInterfaceOrientationPortraitUpsideDown:
        x = -data.acceleration.x;
        y = -data.acceleration.y;
        break;
    case UIInterfaceOrientationLandscapeLeft:
        x = -data.acceleration.y;
        y = data.acceleration.x;
        break;
    case UIInterfaceOrientationLandscapeRight:
        x = data.acceleration.y;
        y = -data.acceleration.x;
        break;
    default:
        break;
    }

    out->x = (float)(x * ACCELERATION_GRAVITY);
    out->y = (float)(y * ACCELERATION_GRAVITY);
    out->z = (float)(data.acceleration.z * ACCELERATION_GRAVITY);
    return 1;
}

@interface MRGameView : UIView
@end

@implementation MRGameView

+ (Class)layerClass {
    return CAEAGLLayer.class;
}

- (BOOL)mapPoint:(CGPoint)point toX:(int *)x y:(int *)y {
    if (!x || !y || self.bounds.size.width <= 0.0 || self.bounds.size.height <= 0.0) {
        return NO;
    }
    uint32_t surface_w = atomic_load_explicit(&SURFACE_W, memory_order_relaxed);
    uint32_t surface_h = atomic_load_explicit(&SURFACE_H, memory_order_relaxed);
    mr_win_fit fit = mr_win_fit_surface(self.bounds.size.width, self.bounds.size.height,
                                        (double)surface_w, (double)surface_h);
    double horizontal = (point.x - fit.x) / fit.w;
    double vertical = (point.y - fit.y) / fit.h;
    horizontal = fmax(0.0, fmin(1.0, horizontal));
    vertical = fmax(0.0, fmin(1.0, vertical));
    *x = (int)(horizontal * (double)(surface_w - 1u));
    *y = (int)(vertical * (double)(surface_h - 1u));
    return YES;
}

- (void)deliverTouch:(UITouch *)touch action:(int)action {
    int x = 0;
    int y = 0;
    if ([self mapPoint:[touch locationInView:self] toX:&x y:&y]) {
        queue_touch(action, x, y);
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (!atomic_load_explicit(&INPUT_ENABLED, memory_order_acquire) || ACTIVE_TOUCH) {
        return;
    }
    ACTIVE_TOUCH = touches.anyObject;
    if (ACTIVE_TOUCH) [self deliverTouch:ACTIVE_TOUCH action:MR_TOUCH_PRESS];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (ACTIVE_TOUCH && [touches containsObject:ACTIVE_TOUCH]) {
        [self deliverTouch:ACTIVE_TOUCH action:MR_TOUCH_MOVE];
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (ACTIVE_TOUCH && [touches containsObject:ACTIVE_TOUCH]) {
        [self deliverTouch:ACTIVE_TOUCH action:MR_TOUCH_RELEASE];
        ACTIVE_TOUCH = nil;
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}

- (void)displayTick:(CADisplayLink *)link {
    BOOL should_signal = NO;
    os_unfair_lock_lock(&FRAME_LOCK);
    if (!CLOSED && SCENE_ACTIVE) {
        DISPLAY_TIMESTAMP = link.timestamp;
        DISPLAY_TARGET_TIMESTAMP = link.targetTimestamp;
        if (!FRAME_PENDING) {
            FRAME_PENDING = YES;
            should_signal = YES;
        }
    }
    os_unfair_lock_unlock(&FRAME_LOCK);
    if (should_signal) signal_frame_waiter();
}

@end

@interface MRGameViewController : UIViewController
@end

@implementation MRGameViewController

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return game_orientation_mask();
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
    return atomic_load_explicit(&MOVIE_LANDSCAPE, memory_order_acquire)
               ? UIInterfaceOrientationLandscapeRight
               : UIInterfaceOrientationPortrait;
}

- (BOOL)prefersInterfaceOrientationLocked {
    return atomic_load_explicit(&ORIENTATION_LOCK, memory_order_acquire);
}

- (BOOL)shouldAutorotate {
    return YES;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    layout_game_view();
}

- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    layout_game_view();
}

@end

static void apply_frame_rate_on_main(double hz) {
    os_unfair_lock_lock(&FRAME_LOCK);
    TARGET_REFRESH_HZ = hz;
    os_unfair_lock_unlock(&FRAME_LOCK);

    dispatch_async(dispatch_get_main_queue(), ^{
      float target = (float)hz;
      DISPLAY_LINK.preferredFrameRateRange = CAFrameRateRangeMake(target, target, target);
    });
}

void mr_win_request_content_rate(double hz) {
    mr_win_apply_content_rate(hz, MAX_REFRESH_HZ, &START_REFRESH_HZ, apply_frame_rate_on_main);
}

double mr_win_display_aspect(void) {
    return SCENE_ASPECT;
}

double mr_win_time_to_tick_ms(void) {
    os_unfair_lock_lock(&FRAME_LOCK);
    double target = DISPLAY_TARGET_TIMESTAMP;
    os_unfair_lock_unlock(&FRAME_LOCK);
    return mr_win_tick_remaining_ms(target, CACurrentMediaTime());
}

int mr_win_thermal_state(void) {
    return (int)NSProcessInfo.processInfo.thermalState;
}

double mr_win_refresh_hz(void) {
    os_unfair_lock_lock(&FRAME_LOCK);
    double value = TARGET_REFRESH_HZ;
    os_unfair_lock_unlock(&FRAME_LOCK);
    return value;
}

double mr_win_max_refresh_hz(void) {
    return MAX_REFRESH_HZ;
}

uint64_t mr_win_present_count(void) {
    return PRESENT_COUNT;
}

uint64_t mr_win_cadence_drops(void) {
    return 0;
}

uint64_t mr_win_cadence_recoveries(void) {
    return 0;
}

static void layout_game_view(void) {
    if (!CONTAINER || !VIEW) return;
    CGSize size = CONTAINER.bounds.size;
    uint32_t surface_w = atomic_load_explicit(&SURFACE_W, memory_order_relaxed);
    uint32_t surface_h = atomic_load_explicit(&SURFACE_H, memory_order_relaxed);
    BOOL rotate = (size.width > size.height) != (surface_w > surface_h);
    CGSize fitting_size = rotate ? CGSizeMake(size.height, size.width) : size;
    mr_win_fit fit = mr_win_fit_surface(fitting_size.width, fitting_size.height, (double)surface_w,
                                        (double)surface_h);
    UIInterfaceOrientation orientation = SCENE.effectiveGeometry.interfaceOrientation;
    CGFloat angle = 0.0;
    if (rotate) {
        if (surface_w > surface_h) {
            angle = orientation == UIInterfaceOrientationPortraitUpsideDown ? -M_PI_2 : M_PI_2;
        } else {
            angle = orientation == UIInterfaceOrientationLandscapeRight ? -M_PI_2 : M_PI_2;
        }
    }
    CGRect bounds = CGRectMake(0.0, 0.0, fit.w, fit.h);
    CGPoint center = CGPointMake(CGRectGetMidX(CONTAINER.bounds), CGRectGetMidY(CONTAINER.bounds));
    CGAffineTransform transform = CGAffineTransformMakeRotation(angle);
    CGFloat scale = fit.w > 0.0 ? (CGFloat)((double)surface_w / fit.w) : 1.0;
    BOOL changed = !CGRectEqualToRect(VIEW.bounds, bounds) ||
                   !CGPointEqualToPoint(VIEW.center, center) ||
                   !CGAffineTransformEqualToTransform(VIEW.transform, transform) ||
                   fabs(VIEW.contentScaleFactor - scale) > 0.0001;
    if (changed) {
        VIEW.transform = CGAffineTransformIdentity;
        VIEW.bounds = bounds;
        VIEW.center = center;
        VIEW.transform = transform;
        VIEW.contentScaleFactor = scale;
    }
    CGFloat view_height = VIEW.bounds.size.height;
    double safe_top = view_height > 0.0 ? VIEW.safeAreaInsets.top / view_height : 0.0;
    double safe_bottom = view_height > 0.0 ? VIEW.safeAreaInsets.bottom / view_height : 0.0;
    safe_top = fmax(0.0, fmin(1.0, safe_top));
    safe_bottom = fmax(0.0, fmin(1.0, safe_bottom));
    unsigned safe_top_ppm = (unsigned)llround(safe_top * 1000000.0);
    unsigned safe_bottom_ppm = (unsigned)llround(safe_bottom * 1000000.0);
    unsigned previous_safe_top =
        atomic_exchange_explicit(&SAFE_AREA_TOP_PPM, safe_top_ppm, memory_order_release);
    unsigned previous_safe_bottom =
        atomic_exchange_explicit(&SAFE_AREA_BOTTOM_PPM, safe_bottom_ppm, memory_order_release);
    if ((previous_safe_top != safe_top_ppm || previous_safe_bottom != safe_bottom_ppm) &&
        getenv("MR_DIAGNOSTICS")) {
        printf("[UIKit] safe-area insets: top %.1f pt, bottom %.1f pt (%.2f%%, %.2f%% of game "
               "view)\n",
               (double)VIEW.safeAreaInsets.top, (double)VIEW.safeAreaInsets.bottom,
               safe_top * 100.0, safe_bottom * 100.0);
    }
    UIInterfaceOrientation input_orientation = orientation;
    if (rotate) {
        input_orientation = surface_w > surface_h ? UIInterfaceOrientationLandscapeRight
                                                  : UIInterfaceOrientationPortrait;
    }
    atomic_store_explicit(&INTERFACE_ORIENTATION, (int)input_orientation, memory_order_relaxed);
    if (changed) atomic_store_explicit(&DRAWABLE_NEEDS_RESIZE, true, memory_order_release);
    update_orientation_lock();
}

static BOOL orientation_matches_request(UIInterfaceOrientation orientation) {
    BOOL landscape = UIInterfaceOrientationIsLandscape(orientation);
    return landscape == atomic_load_explicit(&MOVIE_LANDSCAPE, memory_order_acquire);
}

static void update_orientation_lock(void) {
    if (!SCENE || !WINDOW) return;
    BOOL should_lock = orientation_matches_request(SCENE.effectiveGeometry.interfaceOrientation);
    bool previous = atomic_exchange_explicit(&ORIENTATION_LOCK, should_lock, memory_order_acq_rel);
    if (previous == should_lock) return;
    [WINDOW.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
}

static void request_game_orientation(UIWindowScene *scene, BOOL landscape) {
    UIInterfaceOrientationMask mask =
        landscape ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskPortrait;
    if (getenv("MR_DIAGNOSTICS")) {
        printf("[Orientation] requesting %s scene geometry (current %ld, locked %s)\n",
               mask == UIInterfaceOrientationMaskPortrait ? "portrait" : "landscape",
               (long)scene.effectiveGeometry.interfaceOrientation,
               scene.effectiveGeometry.interfaceOrientationLocked ? "yes" : "no");
    }
    UIWindowSceneGeometryPreferencesIOS *preferences =
        [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
    [scene requestGeometryUpdateWithPreferences:preferences
                                   errorHandler:^(NSError *error) {
                                     if (getenv("MR_DIAGNOSTICS")) {
                                         printf("[Orientation] scene geometry request denied: %s\n",
                                                error.localizedDescription.UTF8String);
                                     }
                                   }];
}

static void request_game_orientation_after_unlock(BOOL landscape, unsigned attempt) {
    if (!SCENE || landscape != atomic_load_explicit(&MOVIE_LANDSCAPE, memory_order_acquire)) {
        return;
    }
    if (SCENE.effectiveGeometry.interfaceOrientationLocked) {
        if (attempt < 60u) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16000000), dispatch_get_main_queue(), ^{
              request_game_orientation_after_unlock(landscape, attempt + 1u);
            });
        } else if (getenv("MR_DIAGNOSTICS")) {
            printf("[Orientation] scene remained locked during transition\n");
        }
        return;
    }
    if (orientation_matches_request(SCENE.effectiveGeometry.interfaceOrientation)) {
        update_orientation_lock();
        return;
    }
    request_game_orientation(SCENE, landscape);
}

static void present_game_window(UIWindowScene *scene) {
    [WINDOW makeKeyAndVisible];
    BOOL landscape = atomic_load_explicit(&MOVIE_LANDSCAPE, memory_order_acquire);
    if (!orientation_matches_request(scene.effectiveGeometry.interfaceOrientation)) {
        request_game_orientation(scene, landscape);
    }
    layout_game_view();
}

void mr_ios_attach_scene(UIWindowScene *scene) {
    if (!scene) return;
    if (WINDOW) {
        SCENE = scene;
        WINDOW.windowScene = scene;
        WINDOW.hidden = NO;
        present_game_window(scene);
        return;
    }

    SCENE = scene;
    TOUCH_LOCK = [[NSLock alloc] init];
    FRAME_SIGNAL = dispatch_semaphore_create(0);
    atomic_store_explicit(&INTERFACE_ORIENTATION, (int)scene.effectiveGeometry.interfaceOrientation,
                          memory_order_relaxed);

    MAX_REFRESH_HZ = (double)scene.screen.maximumFramesPerSecond;
    if (!(MAX_REFRESH_HZ > 0.0)) MAX_REFRESH_HZ = 60.0;
    TARGET_REFRESH_HZ = START_REFRESH_HZ = fmin(MAX_REFRESH_HZ, MR_WIN_MAX_PRESENT_HZ);

    WINDOW = [[UIWindow alloc] initWithWindowScene:scene];
    CGSize size = WINDOW.bounds.size;
    if (size.width > 0.0 && size.height > 0.0) {
        SCENE_ASPECT = fmin(size.width, size.height) / fmax(size.width, size.height);
    }

    CONTAINER = [[UIView alloc] initWithFrame:WINDOW.bounds];
    CONTAINER.backgroundColor = UIColor.blackColor;
    CONTAINER.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    VIEW = [[MRGameView alloc] initWithFrame:CONTAINER.bounds];
    VIEW.multipleTouchEnabled = NO;
    [CONTAINER addSubview:VIEW];

    CAEAGLLayer *layer = (CAEAGLLayer *)VIEW.layer;
    layer.opaque = YES;
    layer.drawableProperties = @{
        kEAGLDrawablePropertyRetainedBacking : @NO,
        kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8
    };

    MRGameViewController *controller = [[MRGameViewController alloc] init];
    controller.view = CONTAINER;
    WINDOW.rootViewController = controller;
    WINDOW.backgroundColor = UIColor.blackColor;
    present_game_window(scene);

    configure_accelerometer();
    DISPLAY_LINK = [CADisplayLink displayLinkWithTarget:VIEW selector:@selector(displayTick:)];
    float target = (float)TARGET_REFRESH_HZ;
    DISPLAY_LINK.preferredFrameRateRange = CAFrameRateRangeMake(target, target, target);
    DISPLAY_LINK.paused = YES;
    [DISPLAY_LINK addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

void mr_ios_set_scene_active(BOOL active) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
          mr_ios_set_scene_active(active);
        });
        return;
    }

    os_unfair_lock_lock(&FRAME_LOCK);
    SCENE_ACTIVE = active;
    if (!active) {
        FRAME_PENDING = NO;
        LAST_DELIVERED_TARGET = 0.0;
    }
    os_unfair_lock_unlock(&FRAME_LOCK);
    DISPLAY_LINK.paused = !active;
    set_accelerometer_active(active);
    if (!active && ACTIVE_TOUCH && VIEW) {
        [(MRGameView *)VIEW deliverTouch:ACTIVE_TOUCH action:MR_TOUCH_RELEASE];
        ACTIVE_TOUCH = nil;
    }
    signal_frame_waiter();
}

void mr_ios_scene_geometry_changed(UIWindowScene *scene) {
    if (scene != SCENE) return;
    layout_game_view();
}

static void destroy_drawable(void) {
    if (COLOR_RENDERBUFFER) glDeleteRenderbuffers(1, &COLOR_RENDERBUFFER);
    if (WINDOW_FBO) glDeleteFramebuffers(1, &WINDOW_FBO);
    COLOR_RENDERBUFFER = 0;
    WINDOW_FBO = 0;
    DRAWABLE_W = 0;
    DRAWABLE_H = 0;
    mr_gl_context_set_window_framebuffer(0);
}

static int attach_drawable(void) {
    destroy_drawable();
    mr_gl_context_make_current(NULL);

    __block int result = -1;
    run_on_main_sync(^{
      EAGLContext *context = (__bridge EAGLContext *)GL_CONTEXT;
      if (![EAGLContext setCurrentContext:context]) return;

      CAEAGLLayer *layer = (CAEAGLLayer *)VIEW.layer;
      glGenFramebuffers(1, &WINDOW_FBO);
      glBindFramebuffer(GL_FRAMEBUFFER, WINDOW_FBO);
      glGenRenderbuffers(1, &COLOR_RENDERBUFFER);
      glBindRenderbuffer(GL_RENDERBUFFER, COLOR_RENDERBUFFER);
      BOOL stored = [context renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
      if (stored) {
          glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &DRAWABLE_W);
          glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &DRAWABLE_H);
          glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                    COLOR_RENDERBUFFER);
          stored = DRAWABLE_W > 0 && DRAWABLE_H > 0 &&
                   glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
      }

      if (stored) {
          if (getenv("MR_DIAGNOSTICS")) {
              GLint format = 0;
              GLint samples = 0;
              glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT,
                                           &format);
              glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
              printf("[GL] drawable: %dx%d, format 0x%x, %d samples, FBO %u\n", DRAWABLE_W,
                     DRAWABLE_H, format, samples, WINDOW_FBO);
          }
          mr_gl_context_set_window_framebuffer(WINDOW_FBO);
          atomic_store_explicit(&DRAWABLE_NEEDS_RESIZE, false, memory_order_release);
          result = 0;
      } else {
          destroy_drawable();
      }
      [EAGLContext setCurrentContext:nil];
    });
    mr_gl_context_make_current(GL_CONTEXT);
    return result;
}

int mr_win_open(uint32_t window_w, uint32_t window_h, uint32_t surface_w, uint32_t surface_h,
                const char *title) {
    (void)window_w;
    (void)window_h;
    (void)title;
    atomic_store_explicit(&SURFACE_W, surface_w ? surface_w : 1u, memory_order_relaxed);
    atomic_store_explicit(&SURFACE_H, surface_h ? surface_h : 1u, memory_order_relaxed);
    if (!VIEW) return -1;

    run_on_main_sync(^{
      layout_game_view();
      [CATransaction flush];
    });
    GL_CONTEXT = mr_gl_context_create();
    if (!GL_CONTEXT) return -1;
    mr_gl_context_make_current(GL_CONTEXT);
    return attach_drawable();
}

int mr_win_wait_frame(mr_frame_timing *out, mr_idle_service_fn idle, void *idle_ctx) {
    int idle_was_busy = 1;
    for (;;) {
        os_unfair_lock_lock(&FRAME_LOCK);
        BOOL active = SCENE_ACTIVE;
        BOOL closed = CLOSED;
        os_unfair_lock_unlock(&FRAME_LOCK);
        if (closed) return 0;

        int64_t wait_ns = idle_was_busy ? ACTIVE_IDLE_BUSY_WAIT_NS : ACTIVE_IDLE_QUIET_WAIT_NS;
        dispatch_time_t timeout =
            active ? dispatch_time(DISPATCH_TIME_NOW, wait_ns) : DISPATCH_TIME_FOREVER;
        if (dispatch_semaphore_wait(FRAME_SIGNAL, timeout) == 0) {
            double timestamp = 0.0;
            double target = 0.0;
            double previous_target = 0.0;
            BOOL ready = NO;
            os_unfair_lock_lock(&FRAME_LOCK);
            if (CLOSED) {
                os_unfair_lock_unlock(&FRAME_LOCK);
                return 0;
            }
            if (SCENE_ACTIVE && FRAME_PENDING) {
                timestamp = DISPLAY_TIMESTAMP;
                target = DISPLAY_TARGET_TIMESTAMP;
                previous_target = LAST_DELIVERED_TARGET;
                LAST_DELIVERED_TARGET = target;
                FRAME_PENDING = NO;
                ready = YES;
            }
            os_unfair_lock_unlock(&FRAME_LOCK);
            if (!ready) continue;

            if (atomic_exchange_explicit(&DRAWABLE_NEEDS_RESIZE, false, memory_order_acq_rel) &&
                attach_drawable() != 0) {
                return -1;
            }

            double refresh = target - timestamp;
            double frame = previous_target > 0.0 ? target - previous_target : refresh;
            if (!(refresh > 0.0)) refresh = 0.0;
            if (!(frame > 0.0)) frame = refresh;
            if (out) {
                out->frame_ms = frame * 1000.0;
                out->refresh_ms = refresh * 1000.0;
            }
            return 1;
        }

        if (is_closed()) return 0;
        if (!idle) continue;
        double budget = mr_win_time_to_tick_ms();
        if (budget <= 0.0) continue;
        int idle_rc = idle(idle_ctx, budget);
        if (idle_rc < 0) return -1;
        idle_was_busy = idle_rc > 0;
    }
}

void mr_win_present(void) {
    if (!GL_CONTEXT || is_closed()) return;
    double started = mr_monotonic_ms();
    mr_gl_blit_to_window((uint32_t)DRAWABLE_W, (uint32_t)DRAWABLE_H);
    double copied = mr_monotonic_ms();
    glBindRenderbuffer(GL_RENDERBUFFER, COLOR_RENDERBUFFER);
    BOOL presented = [(__bridge EAGLContext *)GL_CONTEXT presentRenderbuffer:GL_RENDERBUFFER];
    double finished = mr_monotonic_ms();
    if (!presented) fprintf(stderr, "ERROR: frame cannot be presented\n");

    LAST_COPY_MS = copied - started;
    LAST_SWAP_MS = finished - copied;
    PRESENT_COUNT++;
    mr_gl_restore_target();
}

void mr_win_present_split(double *copy_ms, double *swap_ms) {
    if (copy_ms) *copy_ms = LAST_COPY_MS;
    if (swap_ms) *swap_ms = LAST_SWAP_MS;
}

void mr_win_close(void) {
    os_unfair_lock_lock(&FRAME_LOCK);
    CLOSED = YES;
    FRAME_PENDING = NO;
    os_unfair_lock_unlock(&FRAME_LOCK);
    signal_frame_waiter();

    run_on_main_sync(^{
      set_accelerometer_active(NO);
      MOTION = nil;
      [DISPLAY_LINK invalidate];
      DISPLAY_LINK = nil;
      WINDOW.hidden = YES;
    });

    if (GL_CONTEXT) {
        destroy_drawable();
        mr_gl_context_make_current(NULL);
        mr_gl_context_destroy(GL_CONTEXT);
        GL_CONTEXT = NULL;
    }
    run_on_main_sync(^{
      ACTIVE_TOUCH = nil;
      WINDOW = nil;
      VIEW = nil;
      CONTAINER = nil;
      SCENE = nil;
    });
    TOUCH_LOCK = nil;
    FRAME_SIGNAL = nil;
}

void mr_win_preferred_language(char *output, size_t capacity) {
    if (!output || !capacity) return;
    @autoreleasepool {
        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        NSString *language = preferred ? preferred : @"en";
        const char *value = language.UTF8String;
        snprintf(output, capacity, "%s", value ? value : "en");
    }
}

mr_win_insets mr_win_safe_area(void) {
    mr_win_insets insets = {
        .top = (double)atomic_load_explicit(&SAFE_AREA_TOP_PPM, memory_order_acquire) / 1000000.0,
        .bottom =
            (double)atomic_load_explicit(&SAFE_AREA_BOTTOM_PPM, memory_order_acquire) / 1000000.0,
    };
    return insets;
}

void mr_win_surface_size(uint32_t *width, uint32_t *height) {
    if (width) *width = atomic_load_explicit(&SURFACE_W, memory_order_relaxed);
    if (height) *height = atomic_load_explicit(&SURFACE_H, memory_order_relaxed);
}

void mr_win_set_movie_orientation(int landscape) {
    bool wanted = landscape != 0;
    bool previous = atomic_exchange_explicit(&MOVIE_LANDSCAPE, wanted, memory_order_acq_rel);
    if (previous == wanted) return;
    atomic_store_explicit(&SURFACE_ORIENTATION_REQUEST, wanted ? 1 : 0, memory_order_release);
    atomic_store_explicit(&ORIENTATION_LOCK, false, memory_order_release);
    dispatch_async(dispatch_get_main_queue(), ^{
      UIViewController *controller = WINDOW.rootViewController;
      [controller setNeedsUpdateOfPrefersInterfaceOrientationLocked];
      [controller setNeedsUpdateOfSupportedInterfaceOrientations];
      request_game_orientation_after_unlock(wanted, 0u);
    });
}

int mr_win_take_surface_orientation(void) {
    return atomic_exchange_explicit(&SURFACE_ORIENTATION_REQUEST, -1, memory_order_acq_rel);
}

void mr_win_set_surface_size(uint32_t width, uint32_t height) {
    if (!width || !height) return;
    atomic_store_explicit(&SURFACE_W, width, memory_order_relaxed);
    atomic_store_explicit(&SURFACE_H, height, memory_order_relaxed);
    run_on_main_sync(^{
      layout_game_view();
      [CATransaction flush];
    });
}
