// x86Code: Linux stand-in for Apple's closed SwiftUIMacros compiler plugin (the macOS plugin in Xcode cannot be loaded by
// the Linux swift-frontend). Clean-room: written from the macro declarations in the SDK's SwiftUICore.swiftinterface
// and the expansions Apple Swift 6.4.2 (swiftlang-6.4.2.1.9) prints with -Xfrontend -dump-macro-expansions; it emits the same source.
// Covers the @State family (StateMacro and the helper macros its expansion uses). Not implemented (diagnosed as
// such): @Entry, @Animatable and the other SwiftUIMacros types. Built by build/linux-swiftc.sh into
// sdks/swift-lib/host-plugins/libSwiftUIMacros.so against the toolchain's swift-syntax (usr/lib/swift/host).
import SwiftDiagnostics
import SwiftSyntax
import SwiftSyntaxMacros

struct MacroError: Error, CustomStringConvertible {
	let description: String
	init(_ d: String) { description = d }
}

// ---- base64 (the helper macros carry names / source text as base64 string literals) ----
private let b64 = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".utf8)
func base64(_ s: String) -> String {
	let d = Array(s.utf8); var o = [UInt8](); var i = 0
	while i < d.count {
		let n = d.count - i, b0 = UInt32(d[i]), b1 = n > 1 ? UInt32(d[i + 1]) : 0, b2 = n > 2 ? UInt32(d[i + 2]) : 0
		let v = b0 << 16 | b1 << 8 | b2
		o += [b64[Int(v >> 18 & 63)], b64[Int(v >> 12 & 63)], n > 1 ? b64[Int(v >> 6 & 63)] : 61, n > 2 ? b64[Int(v & 63)] : 61]
		i += 3
	}
	return String(decoding: o, as: UTF8.self)
}
func unbase64(_ s: String) -> String {
	var map = [UInt8: UInt32](); for (i, c) in b64.enumerated() { map[c] = UInt32(i) }
	var o = [UInt8](), acc: UInt32 = 0, bits = 0
	for c in s.utf8 { guard let v = map[c] else { continue }; acc = acc << 6 | v; bits += 6
		if bits >= 8 { bits -= 8; o.append(UInt8(acc >> UInt32(bits) & 255)) } }
	return String(decoding: o, as: UTF8.self)
}

// ---- the @State property being expanded ----
struct StateProperty {
	let name: String
	let type: String?          // type annotation, as written
	let initial: String?       // initializer, @State(initialValue:/wrappedValue:) argument, or nil for an Optional
	let explicitInitial: Bool
	let access: String?        // private/fileprivate/internal/package/public/open (no (set) detail)

	init(_ attr: AttributeSyntax, _ decl: some DeclSyntaxProtocol) throws {
		guard let v = decl.as(VariableDeclSyntax.self), v.bindings.count == 1, let b = v.bindings.first,
		      let id = b.pattern.as(IdentifierPatternSyntax.self) else {
			throw MacroError("'@State' can only be applied to a single stored 'var' property")
		}
		name = id.identifier.trimmedDescription
		type = b.typeAnnotation?.type.trimmedDescription
		var e = b.initializer?.value.trimmedDescription
		if case .argumentList(let args)? = attr.arguments, let a = args.first,
		   ["initialValue", "wrappedValue"].contains(a.label?.text ?? "") { e = a.expression.trimmedDescription }
		explicitInitial = e != nil
		if e == nil, let t = b.typeAnnotation?.type,
		   t.is(OptionalTypeSyntax.self) || t.is(ImplicitlyUnwrappedOptionalTypeSyntax.self) { e = "nil" }
		initial = e
		access = v.modifiers.first { m in
			m.detail == nil && ["private", "fileprivate", "internal", "package", "public", "open"].contains(m.name.text)
		}?.name.text
		if initial == nil && type == nil { throw MacroError("type annotation missing in pattern") }
	}
	/// private/fileprivate with an initial value: SwiftUI's "_makeStorage" form (lazy initial value)
	var usesStorage: Bool { explicitInitial && (access == "private" || access == "fileprivate") }
	var accessPrefix: String { access.map { "\($0) " } ?? "" }
	var stateInit: String {
		"SwiftUICore.State\(type.map { "<\($0)>" } ?? "")(initialValue: \(initial!))"
	}
	var storageClosure: String {
		if let t = type { return "({ let value: \(t) = \(initial!)\nreturn value })" }
		return "{ \(initial!) }"
	}
	var staticInitial: String {
		if let t = type { return "{ let x: \(t) = \(initial!)\nreturn x }()" }
		return initial!
	}
}

public struct StateMacro: AccessorMacro, PeerMacro {
	public static func expansion(of node: AttributeSyntax, providingPeersOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [DeclSyntax] {
		let p = try StateProperty(node, declaration), n = p.name
		if p.usesStorage {
			let uniq = context.makeUniqueName("_initialStoredValue_").text
			let make = "SwiftUICore.State._makeStorage(\(p.storageClosure))"
			return [
				"private var __\(raw: n) = \(raw: make)",
				"""
				@SwiftUICore._StatePropertyWrapperStorage(initialValue: "\(raw: base64(uniq))")
				private var _\(raw: n): SwiftUICore.State<_>! = SwiftUICore._stateNil(of: { \(raw: p.stateInit) })
				""",
				"""
				@SwiftUICore._StateProjectedValue
				\(raw: p.accessPrefix)var $\(raw: n) = \(raw: p.stateInit).projectedValue
				""",
				"""
				@SwiftUICore._StateInitialStoredValue("\(raw: base64(make))")
				private static var \(raw: uniq) = (SwiftUICore.State._makeStorage(initialValue: \(raw: p.staticInitial)))
				""",
			]
		}
		if p.initial != nil {
			return [
				"private var _\(raw: n) = \(raw: p.stateInit)",
				"""
				@SwiftUICore._PropertyWrapperProjectedValue
				\(raw: p.accessPrefix)var $\(raw: n) = \(raw: p.stateInit).projectedValue
				""",
			]
		}
		return [
			"private var _\(raw: n): SwiftUICore.State<\(raw: p.type!)>",
			"\(raw: p.accessPrefix)var $\(raw: n): SwiftUICore.Binding<\(raw: p.type!)> { _\(raw: n).projectedValue }",
		]
	}

	public static func expansion(of node: AttributeSyntax, providingAccessorsOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [AccessorDeclSyntax] {
		let p = try StateProperty(node, declaration), n = p.name
		if p.usesStorage {
			return ["get { __\(raw: n).wrappedValue }", "nonmutating set { __\(raw: n).wrappedValue = newValue }"]
		}
		return [
			"""
			@storageRestrictions(initializes: _\(raw: n))
			init(initialValue) { _\(raw: n) = SwiftUICore.State(initialValue: initialValue) }
			""",
			"get { _\(raw: n).wrappedValue }",
			"nonmutating set { _\(raw: n).wrappedValue = newValue }",
		]
	}
}

private func propertyName(_ decl: some DeclSyntaxProtocol) throws -> String {
	guard let v = decl.as(VariableDeclSyntax.self), let id = v.bindings.first?.pattern.as(IdentifierPatternSyntax.self)
	else { throw MacroError("expected a 'var' property") }
	return id.identifier.trimmedDescription
}
private func stringArgument(_ node: AttributeSyntax) -> String? {
	guard case .argumentList(let args)? = node.arguments, let a = args.first,
	      let s = a.expression.as(StringLiteralExprSyntax.self) else { return nil }
	return s.segments.compactMap { $0.as(StringSegmentSyntax.self)?.content.text }.joined()
}

/// on `_x` (storage form): converts between State<Value> and the `__x` storage
public struct StatePropertyWrapperStorageMacro: AccessorMacro {
	public static func expansion(of node: AttributeSyntax, providingAccessorsOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [AccessorDeclSyntax] {
		let s = "_" + (try propertyName(declaration))
		let initBody: String
		if let b = stringArgument(node) {
			initBody = """
			if initialValue == nil { \(s) = Self.\(unbase64(b)) } \
			else { \(s) = SwiftUICore.State._makeStorage(initialValue: initialValue.wrappedValue) }
			"""
		} else {
			initBody = "\(s) = SwiftUICore.State._makeStorage(initialValue: initialValue.wrappedValue)"
		}
		return [
			"""
			@storageRestrictions(initializes: \(raw: s))
			init(initialValue) { \(raw: initBody) }
			""",
			"get { SwiftUICore.State(initialValue: \(raw: s).wrappedValue) }",
			"set { if newValue != nil { \(raw: s) = SwiftUICore.State._makeStorage(initialValue: newValue.wrappedValue) } }",
		]
	}
}

/// on `$x` (storage form): `get { __x.projectedValue }`
public struct StateProjectedValueMacro: AccessorMacro {
	public static func expansion(of node: AttributeSyntax, providingAccessorsOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [AccessorDeclSyntax] {
		let n = try propertyName(declaration)
		return ["get { __\(raw: n.dropFirst()).projectedValue }"]
	}
}

/// on `$x`: `get { _x.projectedValue }`
public struct ProjectedValueMacro: AccessorMacro {
	public static func expansion(of node: AttributeSyntax, providingAccessorsOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [AccessorDeclSyntax] {
		let n = try propertyName(declaration)
		return ["get { _\(raw: n.dropFirst()).projectedValue }"]
	}
}

/// on the static initial-value property: `get { <decoded initial storage expression> }`
public struct StateInitialStoredValueMacro: AccessorMacro {
	public static func expansion(of node: AttributeSyntax, providingAccessorsOf declaration: some DeclSyntaxProtocol,
	                             in context: some MacroExpansionContext) throws -> [AccessorDeclSyntax] {
		guard let b = stringArgument(node) else { throw MacroError("_StateInitialStoredValue needs a string argument") }
		return ["get { \(raw: unbase64(b)) }"]
	}
}
