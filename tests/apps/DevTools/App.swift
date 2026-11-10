// x86Sim: test app for the developer commands (tests/devtools.sh): prints its arguments and the environment variable
// X86_HELLO to stdout, a counter to stdout/stderr every second (x86sim launch --console), os_log lines (x86sim log
// stream), shows received notifications (x86sim push) and the location (x86sim location set) on screen and in the log.
import UIKit
import UserNotifications
import CoreLocation
import os

let log = Logger(subsystem: "com.x86sim.devtools", category: "test")

func say(_ s: String) {
    print("devtools: \(s)")
    fflush(stdout)
    log.notice("devtools-oslog: \(s, privacy: .public)")
}

@main
final class AppDelegate: UIResponder, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        say("args \(CommandLine.arguments.dropFirst().joined(separator: " "))")
        say("env X86_HELLO=\(ProcessInfo.processInfo.environment["X86_HELLO"] ?? "(unset)")")
        say("pid \(getpid())")
        say("stdout tty \(isatty(1) != 0)")
        let c = UNUserNotificationCenter.current()
        c.delegate = self
        c.requestAuthorization(options: [.alert, .sound, .badge]) { ok, err in say("notifications authorized \(ok) \(err.map { "\($0)" } ?? "")") }
        application.registerForRemoteNotifications()
        var n = 0
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
            n += 1
            print("devtools: tick \(n)"); fflush(stdout)
            fputs("devtools: stderr tick \(n)\n", stderr)
            log.notice("devtools-oslog: tick \(n)")
        }
        return true
    }

    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        UISceneConfiguration(name: "Default", sessionRole: connectingSceneSession.role)
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        let c = notification.request.content
        say("notification \(c.title) / \(c.body)")
        ViewController.shared?.show("Push: \(c.title)\n\(c.body)")
        completionHandler([.banner, .list, .sound])
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = ViewController()
        w.makeKeyAndVisible()
        window = w
    }
}

final class ViewController: UIViewController, CLLocationManagerDelegate {
    static weak var shared: ViewController?
    let label = UILabel()
    let lm = CLLocationManager()
    var lines: [String] = []

    override func viewDidLoad() {
        super.viewDidLoad()
        ViewController.shared = self
        view.backgroundColor = UIColor(red: 0.1, green: 0.2, blue: 0.3, alpha: 1)
        label.numberOfLines = 0
        label.textColor = .white
        label.font = .systemFont(ofSize: 22, weight: .medium)
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            label.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -24),
            label.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 24),
        ])
        show("DevTools\nargs: \(CommandLine.arguments.dropFirst().joined(separator: " "))\nX86_HELLO=\(ProcessInfo.processInfo.environment["X86_HELLO"] ?? "(unset)")")
        lm.delegate = self
        lm.requestWhenInUseAuthorization()
        lm.startUpdatingLocation()
    }

    func show(_ s: String) {
        lines.append(s)
        label.text = lines.suffix(8).joined(separator: "\n\n")
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let l = locations.last else { return }
        let s = String(format: "location %.4f,%.4f", l.coordinate.latitude, l.coordinate.longitude)
        say(s)
        show(s)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) { say("location error \(error)") }
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) { say("location auth \(manager.authorizationStatus.rawValue)") }
}
