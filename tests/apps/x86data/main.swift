// x86data (UIKit): Core Data (SQLite store, model built in code), files (write/read/rename, attributes),
// UserDefaults, a local notification (authorization + scheduling), ImageIO/CoreImage on a generated image.
import UIKit
import CoreData
import UserNotifications
import CoreImage

final class VC: UITableViewController {
    var rows: [String] = []
    override func viewDidLoad() {
        super.viewDidLoad()
        title = "x86data"
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "c")
        navigationItem.rightBarButtonItem = UIBarButtonItem(systemItem: .add, primaryAction: UIAction { [weak self] _ in self?.add() })
        DispatchQueue.global().async { self.runChecks() }
    }
    lazy var container: NSPersistentContainer = {
        let m = NSManagedObjectModel()
        let e = NSEntityDescription(); e.name = "Note"; e.managedObjectClassName = "NSManagedObject"
        let t = NSAttributeDescription(); t.name = "text"; t.attributeType = .stringAttributeType
        let d = NSAttributeDescription(); d.name = "date"; d.attributeType = .dateAttributeType
        e.properties = [t, d]; m.entities = [e]
        let c = NSPersistentContainer(name: "Notes", managedObjectModel: m)
        var err: Error?
        c.loadPersistentStores { _, e in err = e }
        X86Test.log("coredata-open", err == nil, err.map { "\($0)" } ?? (c.persistentStoreDescriptions.first?.url?.lastPathComponent ?? ""))
        return c
    }()
    func add() {
        let ctx = container.viewContext
        let n = NSManagedObject(entity: container.managedObjectModel.entitiesByName["Note"]!, insertInto: ctx)
        n.setValue("Note \(rows.count + 1)", forKey: "text"); n.setValue(Date(), forKey: "date")
        try? ctx.save(); reload()
    }
    func reload() {
        let r = NSFetchRequest<NSManagedObject>(entityName: "Note")
        r.sortDescriptors = [NSSortDescriptor(key: "date", ascending: true)]
        rows = ((try? container.viewContext.fetch(r)) ?? []).compactMap { $0.value(forKey: "text") as? String }
        tableView.reloadData()
    }
    override func tableView(_ t: UITableView, numberOfRowsInSection _: Int) -> Int { rows.count }
    override func tableView(_ t: UITableView, cellForRowAt i: IndexPath) -> UITableViewCell {
        let c = t.dequeueReusableCell(withIdentifier: "c", for: i); c.textLabel?.text = rows[i.row]; return c
    }
    func runChecks() {
        // files
        let fm = FileManager.default
        let dir = fm.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("files")
        do {
            try? fm.removeItem(at: dir)
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            let a = dir.appendingPathComponent("a.txt"), b = dir.appendingPathComponent("b.txt")
            try "hello x86code".write(to: a, atomically: true, encoding: .utf8)
            try fm.moveItem(at: a, to: b)
            let s = try String(contentsOf: b, encoding: .utf8)
            let size = (try fm.attributesOfItem(atPath: b.path)[.size] as? Int) ?? -1
            let list = try fm.contentsOfDirectory(atPath: dir.path)
            X86Test.log("files", s == "hello x86code" && size == 13 && list == ["b.txt"], "size \(size) list \(list)")
        } catch { X86Test.log("files", false, "\(error)") }
        // defaults
        let ud = UserDefaults.standard
        let runs = ud.integer(forKey: "runs") + 1; ud.set(runs, forKey: "runs")
        X86Test.log("defaults", ud.integer(forKey: "runs") == runs, "runs \(runs)")
        // image processing
        let ci = CIImage(color: CIColor(red: 1, green: 0, blue: 0)).cropped(to: CGRect(x: 0, y: 0, width: 64, height: 64))
        let blur = ci.applyingGaussianBlur(sigma: 2)
        let cg = CIContext().createCGImage(blur, from: ci.extent)
        let png = cg.map { UIImage(cgImage: $0).pngData() } ?? nil
        X86Test.log("coreimage", cg?.width == 64 && (png?.count ?? 0) > 50, "png \(png?.count ?? 0) bytes")
        DispatchQueue.main.async {
            // Core Data on the main context
            let before = self.rows.count
            self.reload(); let start = self.rows.count
            self.add(); self.add()
            X86Test.log("coredata", self.rows.count == start + 2, "\(before)->\(start)->\(self.rows.count) rows")
            self.notify()
        }
    }
    func notify() {
        let c = UNUserNotificationCenter.current()
        c.requestAuthorization(options: [.alert, .sound, .badge]) { ok, err in
            X86Test.log("notif-auth", err == nil, "granted \(ok) \(err.map { "\($0)" } ?? "")")
            let content = UNMutableNotificationContent(); content.title = "x86data"; content.body = "local notification"
            let r = UNNotificationRequest(identifier: "x86", content: content, trigger: UNTimeIntervalNotificationTrigger(timeInterval: 2, repeats: false))
            c.add(r) { e in
                X86Test.log("notif-add", e == nil, e.map { "\($0)" } ?? "")
                c.getPendingNotificationRequests { p in X86Test.log("notif-pending", p.contains { $0.identifier == "x86" }, "\(p.count) pending"); X86Test.done() }
            }
        }
    }
}

// iOS 27 UIKit requires the scene lifecycle (an app without it traps in
// _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption), so the window comes from a UIWindowSceneDelegate.
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    func scene(_ scene: UIScene, willConnectTo _: UISceneSession, options _: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        window = UIWindow(windowScene: ws)
        window?.rootViewController = UINavigationController(rootViewController: VC(style: .insetGrouped))
        window?.makeKeyAndVisible()
    }
}

final class Delegate: UIResponder, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    func application(_: UIApplication, didFinishLaunchingWithOptions _: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        X86Test.reset()
        UNUserNotificationCenter.current().delegate = self
        return true
    }
    func application(_: UIApplication, configurationForConnecting s: UISceneSession, options _: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let c = UISceneConfiguration(name: nil, sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
    }
    func userNotificationCenter(_: UNUserNotificationCenter, willPresent n: UNNotification) async -> UNNotificationPresentationOptions {
        X86Test.log("notif-delivered", true, n.request.identifier); return [.banner]
    }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(Delegate.self))
