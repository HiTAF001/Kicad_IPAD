/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Translations of the core's user-facing text. The core writes English; the app passes a
 * table (English -> current language) once at start-up, before any board is loaded.
 */
#ifndef NETLOOM_I18N_H
#define NETLOOM_I18N_H

#include <initializer_list>
#include <string>
#include <unordered_map>

/// Marks an English literal that is translated later through Tr( variable ) (for the
/// string extractor, tools/i18n/extract.py). Expands to the literal.
#define N_( s ) s

namespace kp
{

/// Replace the translation table (call before loading boards; not concurrent with Tr).
void SetTranslations( std::unordered_map<std::string, std::string> aTable );

/// Translation of an English message, or the message itself.
const char* Tr( const char* aEnglish );

/// Translate a pattern with {0}, {1}, ... placeholders, then fill them in.
std::string TrF( const char* aPattern, std::initializer_list<std::string> aArgs );

} // namespace kp

#endif
