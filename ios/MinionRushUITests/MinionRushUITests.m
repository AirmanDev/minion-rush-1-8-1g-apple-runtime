#import <XCTest/XCTest.h>

@interface MinionRushUITests : XCTestCase
@end

@implementation MinionRushUITests

- (void)setUp {
    self.continueAfterFailure = NO;
}

- (void)waitForWindow:(XCUIElement *)window landscape:(BOOL)landscape timeout:(NSTimeInterval)timeout {
    NSPredicate *geometry = [NSPredicate predicateWithBlock:^BOOL(XCUIElement *element,
                                                                   NSDictionary *bindings) {
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
    } else {
        [self waitForWindow:window landscape:YES timeout:45.0];
    }
    [self waitForDelay:2.0];
    [self attachScreenshotNamed:@"Landscape intro"];
    [app terminate];
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

    XCUICoordinate *settings =
        [window coordinateWithNormalizedOffset:CGVectorMake(0.84, 0.04)];
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

@end
