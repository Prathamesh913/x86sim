import UIKit
import SpriteKit

final class GameViewController: UIViewController {
    override func loadView() {
        view = SKView(frame: .zero)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        let skView = view as! SKView
        skView.showsFPS = true
        skView.showsNodeCount = true
        let scene = GameScene(size: .zero)
        scene.scaleMode = .resizeFill
        skView.presentScene(scene)
    }
}
