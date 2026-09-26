#import <XCTest/XCTest.h>

@interface MinionRushUITests : XCTestCase
@end

@implementation MinionRushUITests

- (void)setUp {
    self.continueAfterFailure = NO;
}

- (void)waitForWindow:(XCUIElement *)window
            landscape:(BOOL)landscape
              timeout:(NSTimeInterval)timeout {
    NSPredicate *geometry =
        [NSPredicate predicateWithBlock:^BOOL(XCUIElement *element, NSDictionary *bindings) {
          (void)bindings;
          CGSize size = element.frame.size;
          return landscape ? size.width > size.height : size.height > size.width;
        }];
    XCTNSPredicateExpectation *expectation =
        [[XCTNSPredicateExpectation alloc] initWithPredicate:geometry object:window];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[ expectation ] timeout:timeout],
                   XCTWaiterResultCompleted);
}

- (void)waitForDelay:(NSTimeInterval)delay {
    XCTestExpectation *expectation = [[XCTestExpectation alloc] initWithDescription:@"delay"];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [expectation fulfill];
                   });
    XCTAssertEqual([XCTWaiter waitForExpectations:@[ expectation ] timeout:delay + 1.0],
                   XCTWaiterResultCompleted);
}

- (void)attachScreenshotNamed:(NSString *)name {
    XCTAttachment *attachment =
        [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
    attachment.name = name;
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:attachment];
}

- (void)testIntroUsesLandscapeGeometry {
    XCUIDevice *device = XCUIDevice.sharedDevice;
    device.orientation = UIDeviceOrientationPortrait;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    app.launchEnvironment = @{
        @"MR_LANGUAGE" : @"hu",
        @"MR_DIAGNOSTICS" : @"1",
    };
    [app launch];

    XCUIElement *window = app.windows.firstMatch;
    XCTAssertTrue([window waitForExistenceWithTimeout:30.0]);
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        [self waitForDelay:12.0];
        device.orientation = UIDeviceOrientationLandscapeRight;
    }
    [self waitForWindow:window landscape:YES timeout:45.0];
    [self waitForDelay:2.0];
    [self attachScreenshotNamed:@"Landscape intro"];
    [app terminate];
    device.orientation = UIDeviceOrientationPortrait;
}

- (void)testHungarianButtonUsesEngineSettingsPage {
    BOOL tablet = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    app.launchEnvironment = @{
        @"MR_LANGUAGE" : tablet ? @"hu" : @"en",
        @"MR_DIAGNOSTICS" : @"1",
        @"MR_GUEST_LOG" : @"1",
    };
    [app launch];

    XCUIElement *window = app.windows.firstMatch;
    XCTAssertTrue([window waitForExistenceWithTimeout:30.0]);
    [self waitForWindow:window landscape:NO timeout:30.0];
    [self waitForDelay:15.0];
    [self attachScreenshotNamed:@"Hungarian main menu"];

    XCUICoordinate *settings = [window coordinateWithNormalizedOffset:CGVectorMake(0.84, 0.04)];
    [settings tap];
    [self waitForDelay:4.0];
    XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground);
    [self attachScreenshotNamed:@"Engine language settings"];
    if (!tablet) {
        XCUICoordinate *hungarian =
            [window coordinateWithNormalizedOffset:CGVectorMake(0.59, 0.83)];
        [hungarian tap];
        [self waitForDelay:4.0];
        XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground);
        [self attachScreenshotNamed:@"Hungarian engine settings"];
    }
    [app terminate];
}

- (void)testGameplayOrientationPolicy {
    XCUIDevice *device = XCUIDevice.sharedDevice;
    device.orientation = UIDeviceOrientationPortrait;

    XCUIApplication *app = [[XCUIApplication alloc] init];
    app.launchEnvironment = @{
        @"MR_LANGUAGE" : @"hu",
        @"MR_DIAGNOSTICS" : @"1",
    };
    [app launch];

    XCUIElement *window = app.windows.firstMatch;
    XCTAssertTrue([window waitForExistenceWithTimeout:30.0]);
    [self waitForWindow:window landscape:NO timeout:30.0];
    [self waitForDelay:3.0];

    device.orientation = UIDeviceOrientationPortraitUpsideDown;
    [self waitForDelay:3.0];
    XCTAssertGreaterThan(window.frame.size.height, window.frame.size.width);
    [self attachScreenshotNamed:UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
                                    ? @"Gameplay upside down"
                                    : @"Gameplay upside down rejected"];

    device.orientation = UIDeviceOrientationLandscapeLeft;
    [self waitForDelay:3.0];
    XCTAssertGreaterThan(window.frame.size.height, window.frame.size.width);
    [self attachScreenshotNamed:@"Gameplay landscape left rejected"];

    device.orientation = UIDeviceOrientationLandscapeRight;
    [self waitForDelay:3.0];
    XCTAssertGreaterThan(window.frame.size.height, window.frame.size.width);
    [self attachScreenshotNamed:@"Gameplay landscape right rejected"];

    device.orientation = UIDeviceOrientationPortrait;
    [self waitForWindow:window landscape:NO timeout:15.0];
    [self waitForDelay:3.0];
    [self attachScreenshotNamed:@"Gameplay portrait"];
    [app terminate];
}

- (void)testResultScreenRemainsResponsive {
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationPortrait;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    app.launchEnvironment = @{
        @"MR_LANGUAGE" : @"hu",
        @"MR_DIAGNOSTICS" : @"1",
        @"MR_OFFLINE_LOG" : @"1",
    };
    [app launch];

    XCUIElement *window = app.windows.firstMatch;
    XCTAssertTrue([window waitForExistenceWithTimeout:30.0]);
    [self waitForWindow:window landscape:NO timeout:30.0];
    [self waitForDelay:18.0];
    [[window coordinateWithNormalizedOffset:CGVectorMake(0.80, 0.16)] tap];
    [self waitForDelay:12.0];
    [self attachScreenshotNamed:@"Run start"];
    [self waitForDelay:65.0];
    [self attachScreenshotNamed:@"Run end"];
    [[window coordinateWithNormalizedOffset:CGVectorMake(0.82, 0.94)] tap];
    [self waitForDelay:6.0];
    [self attachScreenshotNamed:@"After result continue"];
    [self waitForDelay:10.0];
    [self attachScreenshotNamed:@"After result transition"];
    XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground);
    [app terminate];
}

@end
