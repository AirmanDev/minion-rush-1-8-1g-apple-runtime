#import <UIKit/UIKit.h>

#include "engine.h"
#include "shim_gl.h"
#include "shim_libc.h"
#include "window_ios.h"

#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ENGINE_ARGUMENT_CAPACITY = 4 };

static char *ENGINE_ARGUMENTS[ENGINE_ARGUMENT_CAPACITY];
static int ENGINE_ARGUMENT_COUNT;

static void release_engine_arguments(void) {
    for (int index = 0; index < ENGINE_ARGUMENT_CAPACITY; index++) {
        free(ENGINE_ARGUMENTS[index]);
        ENGINE_ARGUMENTS[index] = NULL;
    }
    ENGINE_ARGUMENT_COUNT = 0;
}

static BOOL set_engine_argument(int index, const char *value) {
    if (index < 0 || index >= ENGINE_ARGUMENT_CAPACITY || !value) return NO;
    ENGINE_ARGUMENTS[index] = strdup(value);
    return ENGINE_ARGUMENTS[index] != NULL;
}

static NSURL *application_directory(NSSearchPathDirectory directory, NSString *component) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *base = [files URLsForDirectory:directory inDomains:NSUserDomainMask].firstObject;
    if (!base) return nil;

    NSURL *url = [base URLByAppendingPathComponent:component isDirectory:YES];
    NSError *error = nil;
    if (![files createDirectoryAtURL:url
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error]) {
        NSLog(@"Cannot create %@: %@", url.path, error.localizedDescription);
        return nil;
    }
    return url;
}

static void redirect_logs(NSURL *directory) {
    NSURL *log = [directory URLByAppendingPathComponent:@"minion-rush.log"];
    FILE *stream = freopen(log.fileSystemRepresentation, "w", stdout);
    if (!stream) {
        NSLog(@"Cannot open log file: %@", log.path);
        return;
    }
    if (dup2(fileno(stdout), STDERR_FILENO) < 0) {
        NSLog(@"Cannot redirect stderr to %@", log.path);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static BOOL configure_engine(void) {
    NSString *resources = NSBundle.mainBundle.resourcePath;
    NSString *engine = [resources stringByAppendingPathComponent:@"lib/libdespicablemefree.so"];
    NSString *game = [resources stringByAppendingPathComponent:@"game/files"];
    NSString *localizations = [resources stringByAppendingPathComponent:@"localizations"];
    NSURL *data = application_directory(NSApplicationSupportDirectory, @"MinionRush");
    NSURL *logs = application_directory(NSCachesDirectory, @"MinionRush/Logs");
    if (!data || !logs || ![NSFileManager.defaultManager fileExistsAtPath:engine] ||
        ![NSFileManager.defaultManager fileExistsAtPath:game]) {
        NSLog(@"The packaged game engine or data is missing");
        return NO;
    }

    redirect_logs(logs);
    NSURL *capture = [logs URLByAppendingPathComponent:@"startup.png"];
    if (getenv("MR_DIAGNOSTICS")) {
        mr_gl_set_startup_capture(capture.fileSystemRepresentation);
    } else {
        [NSFileManager.defaultManager removeItemAtURL:capture error:nil];
    }
    release_engine_arguments();
    if (!set_engine_argument(0, "minion-rush") ||
        !set_engine_argument(1, engine.fileSystemRepresentation) ||
        !set_engine_argument(2, data.path.fileSystemRepresentation)) {
        release_engine_arguments();
        fprintf(stderr, "ERROR: insufficient memory to start the engine\n");
        return NO;
    }
    ENGINE_ARGUMENT_COUNT = 3;

    const char *frames = getenv("MR_FRAMES");
    if (frames && *frames) {
        if (!set_engine_argument(3, frames)) {
            release_engine_arguments();
            return NO;
        }
        ENGINE_ARGUMENT_COUNT = 4;
    }

    mr_set_data_base(game.fileSystemRepresentation);
    setenv("MR_LOCALIZATION_ROOT", localizations.fileSystemRepresentation, 1);
    return YES;
}

static void *engine_thread(void *unused) {
    (void)unused;
    int result = mr_engine_main(ENGINE_ARGUMENT_COUNT, ENGINE_ARGUMENTS);
    release_engine_arguments();
    dispatch_async(dispatch_get_main_queue(), ^{
      UIApplication.sharedApplication.idleTimerDisabled = NO;
    });
    if (result != 0) fprintf(stderr, "ERROR: engine stopped (%d)\n", result);
    return NULL;
}

static void start_engine_once(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      pthread_attr_t attributes;
      int result = pthread_attr_init(&attributes);
      BOOL initialized = result == 0;
      if (result == 0) {
          result = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
      }
      if (result == 0) {
          result = pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INTERACTIVE, 0);
      }

      pthread_t thread = NULL;
      if (result == 0) {
          result = pthread_create(&thread, &attributes, engine_thread, NULL);
      }
      if (initialized) pthread_attr_destroy(&attributes);
      if (result == 0) {
          pthread_detach(thread);
      } else {
          fprintf(stderr, "ERROR: engine thread cannot start (%d)\n", result);
          release_engine_arguments();
      }
    });
}

@interface MRSceneDelegate : NSObject <UIWindowSceneDelegate>
@end

@implementation MRSceneDelegate

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    (void)options;
    if (![scene isKindOfClass:UIWindowScene.class] ||
        ![session.role isEqualToString:UIWindowSceneSessionRoleApplication]) {
        return;
    }
    mr_ios_attach_scene((UIWindowScene *)scene);
    start_engine_once();
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    mr_ios_set_scene_active(YES);
}

- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    mr_ios_set_scene_active(NO);
}

- (void)sceneDidDisconnect:(UIScene *)scene {
    (void)scene;
    mr_ios_set_scene_active(NO);
}

- (void)windowScene:(UIWindowScene *)windowScene
    didUpdateEffectiveGeometry:(UIWindowSceneGeometry *)previousGeometry {
    (void)previousGeometry;
    mr_ios_scene_geometry_changed(windowScene);
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientationsForWindowScene:
    (UIWindowScene *)windowScene {
    (void)windowScene;
    return mr_ios_supported_orientations();
}

@end

@interface MRAppDelegate : NSObject <UIApplicationDelegate>
@end

@implementation MRAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    if (!configure_engine()) return NO;
    application.idleTimerDisabled = YES;
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"Minion Rush"
                                                                         sessionRole:session.role];
    configuration.delegateClass = MRSceneDelegate.class;
    return configuration;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(MRAppDelegate.class));
    }
}
