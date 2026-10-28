import UIKit

final class ViewController: UIViewController {
    private let titleLabel = UILabel()
    private let counterLabel = UILabel()
    private var count = 0

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemTeal

        titleLabel.text = "Hello UIKit"
        titleLabel.font = .systemFont(ofSize: 44, weight: .bold)
        titleLabel.textColor = .white
        titleLabel.textAlignment = .center

        counterLabel.text = "Taps: 0"
        counterLabel.font = .monospacedDigitSystemFont(ofSize: 28, weight: .medium)
        counterLabel.textColor = .white
        counterLabel.textAlignment = .center

        var config = UIButton.Configuration.filled()
        config.title = "Tap me"
        let button = UIButton(configuration: config, primaryAction: UIAction { [weak self] _ in
            self?.increment()
        })

        let stack = UIStackView(arrangedSubviews: [titleLabel, counterLabel, button])
        stack.axis = .vertical
        stack.alignment = .center
        stack.spacing = 24
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 20),
        ])
    }

    private func increment() {
        count += 1
        counterLabel.text = "Taps: \(count)"
    }
}
