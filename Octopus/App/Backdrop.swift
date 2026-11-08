import SwiftUI

/// Slate: a muted blue-grey version of Caz's sweep.
///
/// Caz's "Sunshine Sweep" image is not part of the reference material, so it was
/// reconstructed from the specification's screenshots by inverting the documented
/// compositing (panels are #3F3F3F at 114/255 with no blur; the dimmed screen is
/// the backdrop at 40 percent over black) and fitted with one multi-stop linear
/// gradient and four elliptical glows. Slate keeps that geometry and recolours it
/// to a blue-grey: hue 250 degrees, 28 percent of the original chroma, slightly
/// darker. It is design 48 of the backdrop gallery. The gradient lives in a
/// 390 x 844 pt reference frame (iPhone 13) and is aspect-filled on other
/// screens, as the original image was.
struct Backdrop: View {
    private static let reference = CGSize(width: 390, height: 844)

    private struct Glow {
        let center: CGPoint
        let radiusX: CGFloat
        let radiusY: CGFloat
        let rotation: Double
        let color: Color
        let alpha: Double

        /// Opacity follows alpha * (1 - t^2)^2 across the ellipse radius t.
        var stops: [Gradient.Stop] {
            let radii: [Double] = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
            return radii.map { (t: Double) -> Gradient.Stop in
                let falloff = (1 - t * t) * (1 - t * t)
                return Gradient.Stop(color: color.opacity(alpha * falloff), location: CGFloat(t))
            }
        }
    }

    private static let baseStops: [Gradient.Stop] = [
        .init(color: Color(.sRGB, red: 0.5583, green: 0.6560, blue: 0.7475), location: 0.0000),
        .init(color: Color(.sRGB, red: 0.5947, green: 0.6845, blue: 0.7822), location: 0.1250),
        .init(color: Color(.sRGB, red: 0.6953, green: 0.7609, blue: 0.8626), location: 0.2500),
        .init(color: Color(.sRGB, red: 0.7215, green: 0.7824, blue: 0.8772), location: 0.3750),
        .init(color: Color(.sRGB, red: 0.6466, green: 0.7281, blue: 0.8223), location: 0.5000),
        .init(color: Color(.sRGB, red: 0.6196, green: 0.7049, blue: 0.7997), location: 0.6250),
        .init(color: Color(.sRGB, red: 0.5853, green: 0.6804, blue: 0.7714), location: 0.7500),
        .init(color: Color(.sRGB, red: 0.5520, green: 0.6606, blue: 0.7458), location: 0.8750),
        .init(color: Color(.sRGB, red: 0.5572, green: 0.6582, blue: 0.7434), location: 1.0000),
    ]
    private static let baseStart = UnitPoint(x: 0.11697, y: 0.03507)
    private static let baseEnd = UnitPoint(x: 1.84850, y: 0.55419)
    private static let glows: [Glow] = [
        Glow(center: CGPoint(x: 223.68, y: 508.52), radiusX: 228.61, radiusY: 178.48, rotation: 1.4879,
             color: Color(.sRGB, red: 0.4926, green: 0.5953, blue: 0.6668), alpha: 1.0000),
        Glow(center: CGPoint(x: -233.24, y: 503.23), radiusX: 249.02, radiusY: 742.48, rotation: 1.1054,
             color: Color(.sRGB, red: 0.9004, green: 0.9173, blue: 0.9686), alpha: 0.8136),
        Glow(center: CGPoint(x: 489.32, y: -9.17), radiusX: 795.14, radiusY: 169.52, rotation: -1.0048,
             color: Color(.sRGB, red: 0.5872, green: 0.6794, blue: 0.7768), alpha: 1.0000),
        Glow(center: CGPoint(x: 216.77, y: 771.52), radiusX: 285.26, radiusY: 119.96, rotation: 1.3348,
             color: Color(.sRGB, red: 0.5039, green: 0.6250, blue: 0.7015), alpha: 0.6875),
    ]

    var body: some View {
        GeometryReader { proxy in
            let scale = max(proxy.size.width / Self.reference.width, proxy.size.height / Self.reference.height)
            ZStack(alignment: .topLeading) {
                LinearGradient(stops: Self.baseStops, startPoint: Self.baseStart, endPoint: Self.baseEnd)
                ForEach(0..<Self.glows.count, id: \.self) { index in
                    glow(Self.glows[index])
                }
            }
            .frame(width: Self.reference.width, height: Self.reference.height)
            .scaleEffect(scale)
            .position(x: proxy.size.width / 2, y: proxy.size.height / 2)
        }
        .clipped()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func glow(_ glow: Glow) -> some View {
        Circle()
            .fill(RadialGradient(stops: glow.stops, center: .center, startRadius: 0, endRadius: glow.radiusX))
            .frame(width: glow.radiusX * 2, height: glow.radiusX * 2)
            .scaleEffect(x: 1, y: glow.radiusY / glow.radiusX)
            .rotationEffect(.radians(glow.rotation))
            .position(glow.center)
    }
}
