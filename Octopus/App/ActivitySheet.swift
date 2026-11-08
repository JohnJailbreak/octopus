import SwiftUI

struct SheetLine: Identifiable, Equatable {
    enum Status: Equatable {
        case running, done, failed, summary, failedSummary
    }

    let id: Int
    var text: String
    var status: Status
}

struct ActivitySheetState: Equatable {
    var isPresented = false
    var title = ""
    var lines: [SheetLine] = []
}

/// The log sheet the primary button morphs into while an operation runs.
/// Lines stack upward from just above the bottom edge and fade out toward the
/// top; at most 14 are kept.
struct ActivitySheetView: View {
    let sheet: ActivitySheetState
    let layout: Layout

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                DotHelix()
                    .frame(width: 30, height: 12)
                Text(sheet.title)
                    .font(.system(size: 18))
                    .foregroundStyle(.white)
                    .lineLimit(1)
            }
            .padding(.top, 20)
            ZStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(sheet.lines.suffix(14)) { line in
                        SheetLineView(line: line)
                            .transition(.opacity.combined(with: .offset(y: 8)))
                    }
                }
                .padding(.horizontal, 25)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            .mask {
                LinearGradient(stops: [.init(color: .clear, location: 0), .init(color: .black, location: 0.45)],
                               startPoint: .top, endPoint: .bottom)
            }
            Color.clear
                .frame(height: 100 + layout.logBottomInset)
        }
    }
}

struct SheetLineView: View {
    let line: SheetLine

    private var running: Bool { line.status == .running }

    private var symbol: String {
        switch line.status {
        case .failed, .failedSummary: return "exclamationmark.circle"
        case .summary: return "externaldrive.badge.checkmark"
        case .running, .done: return "checkmark"
        }
    }

    var body: some View {
        HStack(spacing: 15) {
            ZStack {
                if running {
                    Spinner()
                } else {
                    Image(systemName: symbol)
                        .font(.system(size: 19))
                }
            }
            .frame(width: 30, height: 30)
            Text(line.text)
                .font(.system(size: running ? 20 : 18))
                .lineLimit(1)
                .minimumScaleFactor(0.6)
        }
        .foregroundStyle(.white)
        .opacity(running ? 0.9 : 0.5)
        .frame(height: 40)
        .animation(Motion.lineFinish, value: line.status)
        .accessibilityElement(children: .combine)
    }
}

/// A 72 percent arc of a 20 pt ring with a 2.5 pt round stroke, one turn per second.
struct Spinner: View {
    var body: some View {
        TimelineView(.animation) { context in
            let turn = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1)
            Circle()
                .trim(from: 0, to: 0.72)
                .stroke(style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
                .frame(width: 20, height: 20)
                .rotationEffect(.degrees(turn * 360))
        }
        .accessibilityHidden(true)
    }
}

/// Two rows of ten 2 pt dots, 3 pt apart. Each dot follows
/// 0.5 - 0.5 cos(2 pi t / 2.3 s), travelling 8 pt between the rows, and runs
/// 0.1 s behind the dot to its left, so the rows wave past each other.
struct DotHelix: View {
    var body: some View {
        TimelineView(.animation) { context in
            let time = context.date.timeIntervalSinceReferenceDate
            Canvas { graphics, _ in
                for index in 0..<10 {
                    let phase = 0.5 - 0.5 * cos(2 * Double.pi * (time - 0.1 * Double(index)) / 2.3)
                    let x = 1.5 + 3 * CGFloat(index)
                    let travel = 8 * CGFloat(phase)
                    let upper = CGRect(x: x - 1, y: 1 + travel, width: 2, height: 2)
                    let lower = CGRect(x: x - 1, y: 9 - travel, width: 2, height: 2)
                    graphics.fill(Path(ellipseIn: upper), with: .color(.white.opacity(0.8)))
                    graphics.fill(Path(ellipseIn: lower), with: .color(.white))
                }
            }
        }
        .accessibilityHidden(true)
    }
}
