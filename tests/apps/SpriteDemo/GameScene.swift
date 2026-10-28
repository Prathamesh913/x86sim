import SpriteKit

// A few SKShapeNode / SKLabelNode sprites that spin and bounce off the scene edges under gravity.
final class GameScene: SKScene {
    private struct Mover {
        let node: SKNode
        let radius: CGFloat
        var velocity: CGVector
        let spin: CGFloat
    }

    private var movers: [Mover] = []
    private var lastTime: TimeInterval = 0
    private var placed = false

    override func didMove(to view: SKView) {
        backgroundColor = SKColor(white: 0.07, alpha: 1)

        let circle = SKShapeNode(circleOfRadius: 40)
        circle.fillColor = .systemRed
        circle.strokeColor = .white

        let square = SKShapeNode(rectOf: CGSize(width: 80, height: 80), cornerRadius: 10)
        square.fillColor = .systemBlue
        square.strokeColor = .white

        let path = CGMutablePath()
        path.move(to: CGPoint(x: 0, y: 46))
        path.addLine(to: CGPoint(x: -42, y: -26))
        path.addLine(to: CGPoint(x: 42, y: -26))
        path.closeSubpath()
        let triangle = SKShapeNode(path: path)
        triangle.fillColor = .systemGreen
        triangle.strokeColor = .white

        let label = SKLabelNode(fontNamed: "Menlo-Bold")
        label.text = "SpriteKit"
        label.fontSize = 36
        label.fontColor = .systemYellow
        label.verticalAlignmentMode = .center

        for (node, radius) in [(circle as SKNode, CGFloat(40)), (square, 56), (triangle, 46), (label, 60)] {
            addChild(node)
            movers.append(Mover(node: node,
                                radius: radius,
                                velocity: CGVector(dx: CGFloat.random(in: -220...220),
                                                   dy: CGFloat.random(in: 100...400)),
                                spin: CGFloat.random(in: -3...3)))
        }
    }

    override func update(_ currentTime: TimeInterval) {
        // Scene size is 0 until the view is laid out: place the sprites once it is known.
        guard size.width > 0, size.height > 0 else { return }
        if !placed {
            placed = true
            for m in movers {
                m.node.position = CGPoint(x: CGFloat.random(in: 80...(size.width - 80)),
                                          y: CGFloat.random(in: 80...(size.height - 80)))
            }
        }

        let dt = lastTime == 0 ? 1.0 / 60 : min(currentTime - lastTime, 1.0 / 30)
        lastTime = currentTime
        let step = CGFloat(dt)
        let gravity: CGFloat = -900

        for i in movers.indices {
            var m = movers[i]
            let r = m.radius
            m.velocity.dy += gravity * step
            m.node.position.x += m.velocity.dx * step
            m.node.position.y += m.velocity.dy * step

            if m.node.position.x < r {
                m.node.position.x = r
                m.velocity.dx = abs(m.velocity.dx)
            } else if m.node.position.x > size.width - r {
                m.node.position.x = size.width - r
                m.velocity.dx = -abs(m.velocity.dx)
            }
            if m.node.position.y < r {
                m.node.position.y = r
                m.velocity.dy = abs(m.velocity.dy) * 0.9   // bounce with some energy loss
            } else if m.node.position.y > size.height - r {
                m.node.position.y = size.height - r
                m.velocity.dy = -abs(m.velocity.dy)
            }

            m.node.zRotation += m.spin * step
            movers[i] = m
        }
    }
}
