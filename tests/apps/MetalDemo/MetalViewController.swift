import UIKit
import MetalKit

final class MetalViewController: UIViewController {
    private let mtkView = MTKView()
    private let frameLabel = UILabel()
    private var renderer: Renderer?

    override func viewDidLoad() {
        super.viewDidLoad()
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("no Metal device")
        }

        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.device = device
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.preferredFramesPerSecond = 60
        view.addSubview(mtkView)

        frameLabel.translatesAutoresizingMaskIntoConstraints = false
        frameLabel.text = "Frames: 0"
        frameLabel.font = .monospacedDigitSystemFont(ofSize: 22, weight: .semibold)
        frameLabel.textColor = .white
        frameLabel.textAlignment = .center
        frameLabel.backgroundColor = UIColor.black.withAlphaComponent(0.5)
        frameLabel.layer.cornerRadius = 8
        frameLabel.clipsToBounds = true
        view.addSubview(frameLabel)

        NSLayoutConstraint.activate([
            mtkView.topAnchor.constraint(equalTo: view.topAnchor),
            mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            frameLabel.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 12),
            frameLabel.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            frameLabel.widthAnchor.constraint(equalToConstant: 220),
            frameLabel.heightAnchor.constraint(equalToConstant: 40),
        ])

        do {
            let r = try Renderer(device: device, pixelFormat: mtkView.colorPixelFormat)
            r.onFrame = { [weak self] n in self?.frameLabel.text = "Frames: \(n)" }
            mtkView.delegate = r
            renderer = r
        } catch {
            frameLabel.text = "Shader error: \(error)"
        }
    }
}
