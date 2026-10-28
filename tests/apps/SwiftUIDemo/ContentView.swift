import SwiftUI

struct ContentView: View {
    var body: some View {
        NavigationStack {
            List {
                Section {
                    HeaderView()
                    FetchView()
                }
                Section("Items") {
                    ForEach(1...20, id: \.self) { n in
                        NavigationLink(value: n) {
                            Text("Item \(n)")
                        }
                    }
                }
            }
            .navigationTitle("SwiftUI Demo")
            .navigationDestination(for: Int.self) { n in
                DetailView(number: n)
            }
        }
    }
}

// Continuously rotating and pulsing shape, driven by TimelineView so it animates every frame.
struct HeaderView: View {
    var body: some View {
        TimelineView(.animation) { context in
            let t = context.date.timeIntervalSinceReferenceDate
            let angle = (t.truncatingRemainder(dividingBy: 6) / 6) * 360
            let pulse = 1 + 0.2 * sin(t * 2 * Double.pi / 1.5)
            RoundedRectangle(cornerRadius: 24)
                .fill(LinearGradient(colors: [.orange, .pink],
                                     startPoint: .topLeading,
                                     endPoint: .bottomTrailing))
                .frame(width: 90, height: 90)
                .rotationEffect(.degrees(angle))
                .scaleEffect(pulse)
                .frame(maxWidth: .infinity, minHeight: 140)
        }
    }
}

// Fetches https://example.com and shows the HTTP status and the first 80 characters of the body.
struct FetchView: View {
    @State private var result = "Loading https://example.com ..."

    var body: some View {
        Text(result)
            .font(.system(.footnote, design: .monospaced))
            .task { await load() }
    }

    private func load() async {
        do {
            let (data, response) = try await URLSession.shared.data(from: URL(string: "https://example.com")!)
            let status = (response as? HTTPURLResponse)?.statusCode ?? -1
            let text = String(decoding: data, as: UTF8.self)
            result = "HTTP \(status)\n\(String(text.prefix(80)))"
        } catch {
            result = "Error: \(error.localizedDescription)"
        }
    }
}

struct DetailView: View {
    let number: Int

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "cube.fill")
                .font(.system(size: 80))
                .foregroundStyle(.teal)
            Text("Detail for item \(number)")
                .font(.title)
        }
        .navigationTitle("Item \(number)")
    }
}
