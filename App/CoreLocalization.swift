// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.
//
// The C++ core writes its messages and item descriptions in English. At start-up the app
// passes the table for the current language (Core.strings of the localization iOS picked:
// the device language, or English when it is not one of ours) to the core.

import Foundation

enum CoreLocalization {
    /// Call once, before any board is loaded.
    static func install(bundle: Bundle = .main) {
        guard let url = bundle.url(forResource: "Core", withExtension: "strings"),
              let table = NSDictionary(contentsOf: url) as? [String: String],
              !table.isEmpty else { return }

        let pairs = Array(table)
        let keys = pairs.map { strdup($0.key) }
        let values = pairs.map { strdup($0.value) }

        defer {
            keys.forEach { free($0) }
            values.forEach { free($0) }
        }

        let keyPointers = keys.map { UnsafePointer<CChar>($0) }
        let valuePointers = values.map { UnsafePointer<CChar>($0) }

        keyPointers.withUnsafeBufferPointer { k in
            valuePointers.withUnsafeBufferPointer { v in
                kp_set_translations(k.baseAddress, v.baseAddress, Int32(pairs.count))
            }
        }
    }
}
