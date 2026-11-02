# Test apps (regression suite for third-party apps)

Small purpose-built iOS apps, built for the arm64 iOS Simulator on the Mac with `swiftc` (no Xcode project), installed
with `x86sim install` and checked logs-first. Every check prints `X86TEST <app> <check> PASS|FAIL <detail>` to stderr
and appends it to `Documents/x86test-results.txt` in the app's data container.

| app | exercises |
|---|---|
| x86ui | SwiftUI List (200 rows), NavigationStack push/pop, repeating animation, TextField + keyboard, SF Symbols, UIGraphicsImageRenderer → PNG |
| x86net | URLSession HTTPS GET + JSON, download task, WKWebView page load (+ JS `document.title`) |
| x86data | UIKit scene lifecycle, Core Data (SQLite store, model built in code), files (write/rename/attributes), UserDefaults, Core Image blur → PNG, local notification (authorization prompt, schedule, delivery) |

    tests/apps/build.sh [app...]                 build on the Mac (X86SIM_MAC) into $X86SIM_SCRATCH/apps/
    tests/apps/run.sh <phone> [--no-build] [app...]
                                                 build, install, launch, wait for "done", answer permission prompts
                                                 (simtouch dismiss), check results, launchd lines, crash reports;
                                                 screenshot per app in $X86SIM_SCRATCH/apps/run-<time>/

`<phone>` is a phone's name, folder or dir (as `x86sim install`). The phone must be running.
Apps without the UIScene lifecycle trap in iOS 27's UIKit (`_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption`),
so every app here uses scenes.
