import SwiftUI

/// Native stand-in for Caz's "Sunshine Sweep" backdrop image, which is not part
/// of the reference material.
///
/// The clean backdrop was reconstructed from the specification's screenshots by
/// inverting the documented compositing (panels are #3F3F3F at 114/255 with no
/// blur; the dimmed screen is the backdrop at 40 percent over black), then fitted
/// with one multi-stop linear gradient and four elliptical glows. Fit error
/// against the reconstruction: 3.3 of 255 levels RMS, 6.9 at the 99th
/// percentile. The fit lives in a 390 x 844 pt reference frame (iPhone 13) and
/// is aspect-filled on other screens, as the original image was.
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
            [0.0, 0.2, 0.4, 0.6, 0.8, 1.0].map { t in
                Gradient.Stop(color: color.opacity(alpha * pow(1 - t * t, 2)), location: t)
            }
        }
    }

    private static let baseStops: [Gradient.Stop] = [
        .init(color: Color(.sRGB, red: 0.9011, green: 0.6355, blue: 0.0163), location: 0.0000),
        .init(color: Color(.sRGB, red: 0.9166, green: 0.6781, blue: 0.0828), location: 0.1250),
        .init(color: Color(.sRGB, red: 0.9477, green: 0.7893, blue: 0.2968), location: 0.2500),
        .init(color: Color(.sRGB, red: 0.9593, green: 0.8143, blue: 0.3775), location: 0.3750),
        .init(color: Color(.sRGB, red: 0.9455, green: 0.7324, blue: 0.2486), location: 0.5000),
        .init(color: Color(.sRGB, red: 0.9289, green: 0.7038, blue: 0.1902), location: 0.6250),
        .init(color: Color(.sRGB, red: 0.9217, green: 0.6642, blue: 0.1351), location: 0.7500),
        .init(color: Color(.sRGB, red: 0.9243, green: 0.6233, blue: 0.0780), location: 0.8750),
        .init(color: Color(.sRGB, red: 0.9086, green: 0.6309, blue: 0.1199), location: 1.0000),
    ]
    private static let baseStart = UnitPoint(x: 0.11697, y: 0.03507)
    private static let baseEnd = UnitPoint(x: 1.84850, y: 0.55419)
    private static let glows: [Glow] = [
        Glow(center: CGPoint(x: 223.68, y: 508.52), radiusX: 228.61, radiusY: 178.48, rotation: 1.4879,
             color: Color(.sRGB, red: 0.8451, green: 0.5571, blue: 0.1293), alpha: 1.0000),
        Glow(center: CGPoint(x: -233.24, y: 503.23), radiusX: 249.02, radiusY: 742.48, rotation: 1.1054,
             color: Color(.sRGB, red: 1.0000, green: 0.9817, blue: 0.7832), alpha: 0.8136),
        Glow(center: CGPoint(x: 489.32, y: -9.17), radiusX: 795.14, radiusY: 169.52, rotation: -1.0048,
             color: Color(.sRGB, red: 0.9158, green: 0.6695, blue: 0.0411), alpha: 1.0000),
        Glow(center: CGPoint(x: 216.77, y: 771.52), radiusX: 285.26, radiusY: 119.96, rotation: 1.3348,
             color: Color(.sRGB, red: 0.9072, green: 0.5651, blue: 0.0000), alpha: 0.6875),
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
