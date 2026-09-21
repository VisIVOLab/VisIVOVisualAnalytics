// click.swift — a raw mouse event at a screen point, for gui_smoke.py.
//
// Why not AppleScript: System Events can SEE Qt widgets (it reports
// "checkbox Region"), but its `click` performs an accessibility action that Qt
// does not implement for custom-painted controls — the element is found and
// nothing happens. A CGEvent is a real press and release at a point on the
// screen, which the application cannot tell from a hand.
//
// The native menu bar is the exception: that IS a real NSMenu, so gui_smoke.py
// drives menus through System Events and only the in-window widgets through
// this.
//
//   click X Y              — click once at (X, Y), in screen POINTS (not pixels)
//   click X Y toX toY      — press at (X, Y), drag, release at (toX, toY)

import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count >= 3, let x = Double(args[1]), let y = Double(args[2]) else {
    print("usage: click X Y [dragToX dragToY]"); exit(1)
}
let p = CGPoint(x: x, y: y)
let src = CGEventSource(stateID: .hidSystemState)
CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
usleep(80_000)
CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
usleep(80_000)
if args.count >= 5, let dx = Double(args[3]), let dy = Double(args[4]) {
    let steps = 12
    for i in 1...steps {
        let t = Double(i) / Double(steps)
        let q = CGPoint(x: x + (dx - x) * t, y: y + (dy - y) * t)
        CGEvent(mouseEventSource: src, mouseType: .leftMouseDragged, mouseCursorPosition: q, mouseButton: .left)?.post(tap: .cghidEventTap)
        usleep(40_000)
    }
    CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: CGPoint(x: dx, y: dy), mouseButton: .left)?.post(tap: .cghidEventTap)
} else {
    CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
}
