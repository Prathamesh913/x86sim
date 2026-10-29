// x86Code: AudioDemo, the audio test app (tests/audio.sh): a sine tone through AVAudioEngine (an AVAudioSourceNode on
// the output device) and the microphone's level (an input-node tap). Launch arguments for tests:
//   -tone SECONDS [-freq HZ]   play the tone at launch, then print "tone done frames=N rate=R"
//   -mic SECONDS               record at launch, then print "mic done frames=N rms=X peak=Y"
// Everything it reports is printed to stdout too (x86sim launch --console).
import SwiftUI
import AVFoundation

final class Audio: ObservableObject {
	@Published var status = "idle"
	@Published var level: Float = 0
	let engine = AVAudioEngine()
	var source: AVAudioSourceNode?
	var phase = 0.0
	var freq = 440.0
	var rendered = 0
	var micFrames = 0
	var micSum = 0.0
	var micPeak: Float = 0

	func say(_ s: String) { print(s); fflush(stdout); DispatchQueue.main.async { self.status = s } }

	func session() {
		let s = AVAudioSession.sharedInstance()
		do {
			try s.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
			try s.setActive(true)
			say("session active: \(s.sampleRate) Hz, out \(s.outputNumberOfChannels) ch, route \(s.currentRoute.outputs.map { $0.portName })")
		} catch { say("session error: \(error)") }
	}

	func startTone() {
		let out = engine.outputNode.outputFormat(forBus: 0)
		let rate = out.sampleRate > 0 ? out.sampleRate : 48000
		let fmt = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1)!
		let node = AVAudioSourceNode(format: fmt) { [unowned self] _, _, frames, abl -> OSStatus in
			let bufs = UnsafeMutableAudioBufferListPointer(abl)
			let step = 2 * Double.pi * self.freq / rate
			for f in 0..<Int(frames) {
				let v = Float(sin(self.phase)) * 0.2
				self.phase += step
				if self.phase > 2 * Double.pi { self.phase -= 2 * Double.pi }
				for b in bufs { b.mData!.assumingMemoryBound(to: Float.self)[f] = v }
			}
			self.rendered += Int(frames)
			return noErr
		}
		source = node
		engine.attach(node)
		engine.connect(node, to: engine.mainMixerNode, format: fmt)
		do { try engine.start(); say("tone \(Int(freq)) Hz: engine running at \(rate) Hz") }
		catch { say("engine error: \(error)") }
	}

	func stopTone() {
		engine.stop()
		if let n = source { engine.detach(n); source = nil }
		say("tone done frames=\(rendered) rate=\(engine.outputNode.outputFormat(forBus: 0).sampleRate)")
	}

	func startMic() {
		let input = engine.inputNode
		let fmt = input.inputFormat(forBus: 0)
		say("mic format: \(fmt.sampleRate) Hz \(fmt.channelCount) ch")
		input.installTap(onBus: 0, bufferSize: 1024, format: fmt) { [unowned self] buf, _ in
			guard let d = buf.floatChannelData?[0] else { return }
			var s = 0.0; var p: Float = 0
			for i in 0..<Int(buf.frameLength) { s += Double(d[i] * d[i]); p = max(p, abs(d[i])) }
			self.micFrames += Int(buf.frameLength); self.micSum += s; self.micPeak = max(self.micPeak, p)
			let rms = Float(sqrt(s / Double(max(1, buf.frameLength))))
			DispatchQueue.main.async { self.level = rms }
		}
		do { try engine.start(); say("mic running") } catch { say("mic engine error: \(error)") }
	}

	func stopMic() {
		engine.inputNode.removeTap(onBus: 0)
		engine.stop()
		let rms = sqrt(micSum / Double(max(1, micFrames)))
		say(String(format: "mic done frames=%d rms=%.5f peak=%.5f", micFrames, rms, micPeak))
	}
}

struct ContentView: View {
	@StateObject var audio = Audio()
	@State var playing = false
	@State var recording = false
	var body: some View {
		VStack(spacing: 24) {
			Text("AudioDemo").font(.largeTitle).bold()
			Button(playing ? "Stop tone" : "Play 440 Hz tone") {
				if playing { audio.stopTone() } else { audio.startTone() }
				playing.toggle()
			}.buttonStyle(.borderedProminent).font(.title2)
			Button(recording ? "Stop microphone" : "Microphone level") {
				if recording { audio.stopMic() } else {
					AVAudioApplication.requestRecordPermission { ok in
						DispatchQueue.main.async { if ok { audio.startMic() } else { audio.say("mic permission denied") } }
					}
				}
				recording.toggle()
			}.buttonStyle(.bordered).font(.title2)
			ProgressView(value: min(1, Double(audio.level) * 5)).padding(.horizontal, 40)
			Text(audio.status).font(.body.monospaced()).multilineTextAlignment(.center).padding()
		}
		.onAppear {
			audio.session()
			let a = CommandLine.arguments
			func arg(_ n: String) -> Double? { if let i = a.firstIndex(of: n), i + 1 < a.count { return Double(a[i + 1]) }; return nil }
			if let f = arg("-freq") { audio.freq = f }
			if let t = arg("-tone") {
				playing = true; audio.startTone()
				DispatchQueue.main.asyncAfter(deadline: .now() + t) { audio.stopTone(); playing = false }
			} else if let t = arg("-mic") {
				recording = true; audio.startMic()
				DispatchQueue.main.asyncAfter(deadline: .now() + t) { audio.stopMic(); recording = false }
			}
		}
	}
}

@main
struct AudioDemoApp: App {
	var body: some Scene { WindowGroup { ContentView() } }
}
