// x86ui: SwiftUI list + NavigationStack + animation + TextField (keyboard) + images drawn with Core Graphics / SF Symbols.
import SwiftUI
import UIKit

struct Item: Identifiable, Hashable { let id: Int; var title: String { "Row \(id)" } }

final class Model: ObservableObject {
    @Published var items = (1...200).map(Item.init)
    @Published var text = ""
    @Published var spin = false
}

struct Detail: View {
    let item: Item
    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "star.circle.fill").font(.system(size: 80)).foregroundStyle(.orange)
            Text(item.title).font(.largeTitle).accessibilityIdentifier("detail")
        }
        .navigationTitle(item.title)
        .onAppear { X86Test.log("navigate", true, item.title) }
    }
}

struct Root: View {
    @StateObject var m = Model()
    @State var path = NavigationPath()
    @FocusState var focused: Bool
    var body: some View {
        NavigationStack(path: $path) {
            VStack(spacing: 8) {
                HStack {
                    Image(uiImage: drawn()).resizable().frame(width: 44, height: 44)
                    RoundedRectangle(cornerRadius: 8).fill(.blue).frame(width: 44, height: 44)
                        .rotationEffect(.degrees(m.spin ? 360 : 0))
                        .animation(.linear(duration: 1.5).repeatForever(autoreverses: false), value: m.spin)
                    TextField("Type here", text: $m.text).textFieldStyle(.roundedBorder).focused($focused)
                        .onSubmit { X86Test.log("text", !m.text.isEmpty, m.text) }
                }.padding(.horizontal)
                Text("typed: \(m.text)").font(.caption)
                List(m.items) { it in
                    NavigationLink(value: it) { Label(it.title, systemImage: it.id % 2 == 0 ? "circle" : "square") }
                }
            }
            .navigationTitle("x86ui")
            .navigationDestination(for: Item.self) { Detail(item: $0) }
        }
        .onAppear {
            m.spin = true
            X86Test.log("list", m.items.count == 200, "\(m.items.count) rows")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                path.append(m.items[4])       // navigate programmatically (the run checks the detail screen too)
                DispatchQueue.main.asyncAfter(deadline: .now() + 3) { path.removeLast(); X86Test.log("pop", path.isEmpty) ; X86Test.done() }
            }
        }
    }
    func drawn() -> UIImage {
        let r = UIGraphicsImageRenderer(size: CGSize(width: 44, height: 44))
        let img = r.image { c in
            UIColor.systemGreen.setFill(); c.fill(CGRect(x: 0, y: 0, width: 44, height: 44))
            UIColor.white.setFill(); UIBezierPath(ovalIn: CGRect(x: 8, y: 8, width: 28, height: 28)).fill()
        }
        let png = img.pngData()
        X86Test.log("image", (png?.count ?? 0) > 100, "png \(png?.count ?? 0) bytes")
        return img
    }
}

@main
struct X86UIApp: App {
    init() { X86Test.reset() }
    var body: some Scene { WindowGroup { Root() } }
}
