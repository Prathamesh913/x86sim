// x86net: URLSession (HTTPS GET, JSON decode, download task) and a WKWebView loading a page.
import SwiftUI
import WebKit

struct Web: UIViewRepresentable {
    let url: URL
    final class Coord: NSObject, WKNavigationDelegate {
        func webView(_ w: WKWebView, didFinish _: WKNavigation!) {
            w.evaluateJavaScript("document.title") { t, e in X86Test.log("webview", e == nil, "title=\(t ?? "nil")") ; X86Test.done() }
        }
        func webView(_ w: WKWebView, didFail _: WKNavigation!, withError e: Error) { X86Test.log("webview", false, "\(e)") }
        func webView(_ w: WKWebView, didFailProvisionalNavigation _: WKNavigation!, withError e: Error) { X86Test.log("webview", false, "\(e)") }
        func webViewWebContentProcessDidTerminate(_ w: WKWebView) { X86Test.log("webcontent", false, "terminated") }
    }
    func makeCoordinator() -> Coord { Coord() }
    func makeUIView(context: Context) -> WKWebView {
        let w = WKWebView(); w.navigationDelegate = context.coordinator; w.load(URLRequest(url: url)); return w
    }
    func updateUIView(_: WKWebView, context: Context) {}
}

struct Root: View {
    @State var status = "fetching…"
    var body: some View {
        VStack(alignment: .leading) {
            Text("x86net").font(.largeTitle).bold()
            Text(status).font(.footnote).accessibilityIdentifier("status")
            Web(url: URL(string: "https://example.com/")!).border(.gray)
        }.padding()
        .task { await fetch() }
    }
    func fetch() async {
        do {
            let (d, r) = try await URLSession.shared.data(from: URL(string: "https://httpbin.org/json")!)
            let code = (r as? HTTPURLResponse)?.statusCode ?? 0
            let j = try JSONSerialization.jsonObject(with: d) as? [String: Any]
            X86Test.log("https-json", code == 200 && j?["slideshow"] != nil, "status \(code), \(d.count) bytes")
            status = "GET \(code), \(d.count) bytes"
        } catch { X86Test.log("https-json", false, "\(error)"); status = "\(error)" }
        do {
            let (u, r) = try await URLSession.shared.download(from: URL(string: "https://example.com/")!)
            let n = (try? Data(contentsOf: u).count) ?? 0
            X86Test.log("download", (r as? HTTPURLResponse)?.statusCode == 200 && n > 100, "\(n) bytes")
        } catch { X86Test.log("download", false, "\(error)") }
    }
}

@main
struct X86NetApp: App {
    init() { X86Test.reset() }
    var body: some Scene { WindowGroup { Root() } }
}
