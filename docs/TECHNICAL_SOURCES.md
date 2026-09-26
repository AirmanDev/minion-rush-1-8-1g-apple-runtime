# Primary technical sources

## Project scope

- Current Minion Rush update and its 2025 transition to Unity:
  https://www.gameloft.com/blog/players/minion-rush-massive-update
- Developer background on the new visual and UX systems:
  https://www.gameloft.com/blog/players/minion-rush-massive-update-developers
- Official history of the 2013 launch and the Gameloft, Illumination, and
  Universal collaboration:
  https://www.gameloft.com/newsroom/minion-rush-10-year-anniversary

These sources establish that the current Unity client is a different technical
generation. This project identifies its exact 1.8.1g engine locally by SHA-256
and does not distribute that binary.

## Public repository

- Secure GitHub Actions use and full-length commit SHA pinning:
  https://docs.github.com/en/actions/reference/security/secure-use
- Issue and pull-request templates:
  https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/configuring-issue-templates-for-your-repository
- Community health files:
  https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/creating-a-default-community-health-file

## iOS and iPadOS

- Configuring an app icon using an asset catalog:
  https://developer.apple.com/documentation/xcode/configuring-your-app-icon
- App icon layout and appearance specifications:
  https://developer.apple.com/design/human-interface-guidelines/app-icons
- iPhone models compatible with the current iOS release:
  https://support.apple.com/en-ie/guide/iphone/iphe3fa5df43/ios
- Running on simulated and physical devices:
  https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices
- UI automation and stable accessibility identifiers:
  https://developer.apple.com/documentation/xcuiautomation
  https://developer.apple.com/documentation/uikit/uiaccessibilityidentification/accessibilityidentifier
- Measuring and improving performance on devices:
  https://developer.apple.com/documentation/Xcode/improving-your-app-s-performance
- Xcode performance and energy tools:
  https://developer.apple.com/documentation/xcode/performance-and-metrics
- iPadOS 26 windowing and resizing:
  https://developer.apple.com/videos/play/wwdc2025/282/
- iPadOS 26 interface-orientation locking:
  https://developer.apple.com/documentation/uikit/uiviewcontroller/prefersinterfaceorientationlocked
- iPadOS 26 window resizing and orientation-lock guidance:
  https://developer.apple.com/documentation/technotes/tn3192-migrating-your-app-from-the-deprecated-uirequiresfullscreen-key
- Version-limited iPad full-screen behavior:
  https://developer.apple.com/documentation/bundleresources/information-property-list/uirequiresfullscreenignoredstartingwithversion
- Apple-confirmed OpenGL ES crash in Apple Silicon Simulator runtimes:
  https://developer.apple.com/forums/thread/756598
- View-controller supported orientations:
  https://developer.apple.com/documentation/uikit/uiviewcontroller/supportedinterfaceorientations
- Interface-orientation masks:
  https://developer.apple.com/documentation/uikit/uiinterfaceorientationmask
- Physical device orientation and notification lifecycle:
  https://developer.apple.com/documentation/uikit/uidevice
- Application-level supported orientations:
  https://developer.apple.com/documentation/bundleresources/information-property-list/uisupportedinterfaceorientations
- Updating a view controller's supported orientations:
  https://developer.apple.com/documentation/uikit/uiviewcontroller/setneedsupdateofsupportedinterfaceorientations()
- Scene-level supported orientations on iOS and iPadOS 27:
  https://developer.apple.com/documentation/uikit/uiwindowscenedelegate/supportedinterfaceorientations(for:)
- Window-scene geometry updates:
  https://developer.apple.com/documentation/uikit/uiwindowscene/requestgeometryupdate(_:errorhandler:)
- Effective window-scene geometry:
  https://developer.apple.com/documentation/uikit/uiwindowscene/effectivegeometry
- Scene geometry callbacks on iOS/iPadOS 17 and 18:
  https://developer.apple.com/documentation/uikit/uiwindowscenedelegate/windowscene(_:didupdate:interfaceorientation:traitcollection:)
- Scene geometry callbacks on iOS/iPadOS 26 and later:
  https://developer.apple.com/documentation/uikit/uiwindowscenedelegate/windowscene(_:didupdateeffectivegeometry:)
- Shared Xcode configuration files:
  https://developer.apple.com/documentation/xcode/adding-a-build-configuration-file-to-your-project
- UIKit scene lifecycle:
  https://developer.apple.com/documentation/uikit/app_and_environment/scenes
- UIKit safe-area insets:
  https://developer.apple.com/documentation/uikit/uiview/safeareainsets
- Safe-area change notifications for view controllers:
  https://developer.apple.com/documentation/uikit/uiviewcontroller/viewsafeareainsetsdidchange()
- `CADisplayLink.preferredFrameRateRange`:
  https://developer.apple.com/documentation/quartzcore/cadisplaylink/preferredframeraterange
- Energy-efficient timer and wait usage:
  https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/MinimizeTimerUse.html
- Reducing networking and Bluetooth power usage:
  https://developer.apple.com/documentation/xcode/reducing-networking-and-bluetooth-power-usage
- Core Motion energy best practices:
  https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/MotionBestPractices.html
- `CMMotionManager.stopAccelerometerUpdates()`:
  https://developer.apple.com/documentation/coremotion/cmmotionmanager/1616138-stopaccelerometerupdates
- iOS file-system directories:
  https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemOverview/FileSystemOverview.html
- Privacy manifests:
  https://developer.apple.com/documentation/bundleresources/privacy-manifest-files
- Required-reason API declarations:
  https://developer.apple.com/documentation/bundleresources/describing-use-of-required-reason-api
- Required-reason API technote:
  https://developer.apple.com/documentation/technotes/tn3183-adding-required-reason-api-entries-to-your-privacy-manifest
- Declaring manually managed bundle localizations:
  https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundlelocalizations

## Localization

- BCP 47 language tags in RFC 5646:
  https://www.rfc-editor.org/info/rfc5646/
- Unicode locale data and formatter conventions:
  https://unicode.org/reports/tr35/
- Unicode NFC normalization:
  https://www.unicode.org/reports/tr15/
- XLIFF 2.1 specification:
  https://docs.oasis-open.org/xliff/xliff-core/v2.1/os/xliff-core-v2.1-os.html
- Hungarian flag source and public-domain status:
  https://commons.wikimedia.org/wiki/File:Flag_of_Hungary.svg

## Graphics

- OpenGL ES documentation and deprecation status:
  https://developer.apple.com/documentation/opengles
- OpenGL ES 3.0 specification:
  https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf
- OpenGL ES renderbuffers, `CAEAGLLayer`, and frame presentation:
  https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/OpenGLES_ProgrammingGuide/WorkingwithEAGLContexts/WorkingwithEAGLContexts.html
- `CADisplayLink`:
  https://developer.apple.com/documentation/quartzcore/cadisplaylink
- `NSOpenGLContext.flushBuffer`:
  https://developer.apple.com/documentation/appkit/nsopenglcontext/flushbuffer()
- Apple OpenGL threading:
  https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_threading/opengl_threading.html
- Apple OpenGL texture data and PBOs:
  https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_texturedata/opengl_texturedata.html
- Khronos buffer streaming:
  https://wikis.khronos.org/opengl/Buffer_Object_Streaming

## Audio

- Audio Toolbox:
  https://developer.apple.com/documentation/audiotoolbox
- `AURenderCallback`:
  https://developer.apple.com/documentation/audiotoolbox/aurendercallback
- Real-time Audio Unit rendering:
  https://developer.apple.com/documentation/audiotoolbox/audio_unit_hosting_guide_for_ios/performing_realtime_audio
- `kAudioUnitRenderAction_OutputIsSilence`:
  https://developer.apple.com/documentation/audiotoolbox/kaudiounitrenderaction_outputissilence
- Audio Unit real-time rendering rules:
  https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/AudioUnitProgrammingGuide/TheAudioUnit/TheAudioUnit.html
- `AVAudioSession` media-services reset:
  https://developer.apple.com/documentation/avfaudio/avaudiosession/mediaserviceswereresetnotification
- Audio resumption recommendations on iOS/iPadOS 27 and later:
  https://developer.apple.com/documentation/avfaudio/avaudiosession/resumptionrecommendationnotification
- Audio-session deactivation notifications on iOS/iPadOS 27 and later:
  https://developer.apple.com/documentation/avfaudio/avaudiosession/didbecomeinactivenotification
- Interruption resumption on iOS/iPadOS 17 through 26:
  https://developer.apple.com/documentation/avfaudio/avaudiosession/interruptionoptions/shouldresume
- Apple QA1749, rebuilding audio objects after a reset:
  https://developer.apple.com/library/archive/qa/qa1749/
- OpenSL ES registry:
  https://registry.khronos.org/OpenSL-ES/

## ABI, ELF, and POSIX

- AAPCS32:
  https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst
- AAPCS64:
  https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
- Arm A64 instruction set:
  https://developer.arm.com/documentation/102374/latest/
- QEMU TCG block chaining:
  https://www.qemu.org/docs/master/devel/tcg.html
- Android 32-bit ABI:
  https://android.googlesource.com/platform/bionic/+/main/docs/32-bit-abi.md
- POSIX `nanosleep`:
  https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html
- POSIX `rename`:
  https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html
- POSIX `read` and `write`:
  https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html
  https://pubs.opengroup.org/onlinepubs/9799919799/functions/write.html
- Python ZIP archive validation with `ZipFile.testzip`:
  https://docs.python.org/3/library/zipfile.html
- Apple Silicon memory ordering and Thread Sanitizer:
  https://developer.apple.com/documentation/apple-silicon/addressing-architectural-differences-in-your-macos-code
- SQLite WAL:
  https://www.sqlite.org/wal.html
- JsonCpp:
  https://github.com/open-source-parsers/jsoncpp

## Game data

Weekly-mission identifiers, localization keys, and reward types come from the
validated 1.8.1g game data. The asset validator finds that evidence in the
original data files and accepts only the unmodified engine by SHA-256.
