import SwiftUI

/// Dark theme constants, 1:1 with the Tauri/React UI (App.css / Chat.tsx):
/// page #0b0b0b, cards/bubbles #141414, hairlines #262626, indigo accent,
/// emerald for user, rose for errors.
enum Theme {
    static let page      = Color(red: 0x0b/255, green: 0x0b/255, blue: 0x0b/255)
    static let card      = Color(red: 0x14/255, green: 0x14/255, blue: 0x14/255)
    static let cardAlt   = Color(red: 0x1a/255, green: 0x1a/255, blue: 0x1a/255)
    static let hairline  = Color(red: 0x26/255, green: 0x26/255, blue: 0x26/255)
    static let accent    = Color(red: 0x63/255, green: 0x66/255, blue: 0xf1/255) // indigo-400
    static let accentDim = Color(red: 0x4f/255, green: 0x46/255, blue: 0xe5/255) // indigo-600
    static let user      = Color(red: 0x34/255, green: 0xd3/255, blue: 0x99/255) // emerald-400
    static let error     = Color(red: 0xfb/255, green: 0x71/255, blue: 0x85/255) // rose-400
    static let ok        = Color(red: 0x34/255, green: 0xd3/255, blue: 0x99/255)
    static let warn      = Color(red: 0xf5/255, green: 0x9e/255, blue: 0x0b/255) // amber-500

    static let textPrimary   = Color(red: 0xe5/255, green: 0xe7/255, blue: 0xeb/255)
    static let textSecondary = Color(red: 0x9c/255, green: 0xa3/255, blue: 0xaf/255)
    static let textTertiary  = Color(red: 0x6b/255, green: 0x72/255, blue: 0x80/255)
}
