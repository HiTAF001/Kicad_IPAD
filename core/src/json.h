/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Small JSON reader, enough for KiCad project files (.kicad_pro).
 */
#ifndef NETLOOM_JSON_H
#define NETLOOM_JSON_H

#include <string>
#include <utility>
#include <vector>

namespace kp
{

struct JValue
{
    enum Type : unsigned char
    {
        NUL,
        BOOL,
        NUM,
        STR,
        ARR,
        OBJ
    };

    Type                                        type = NUL;
    bool                                        b = false;
    double                                      n = 0.0;
    std::string                                 s;
    std::vector<JValue>                         arr;
    std::vector<std::pair<std::string, JValue>> obj;

    /// Member of an object, or nullptr.
    const JValue* Get( const char* aKey ) const;

    /// Member path, e.g. Path( { "board", "design_settings", "rules" } ).
    const JValue* Path( std::initializer_list<const char*> aKeys ) const;

    double Num( double aDefault ) const { return type == NUM ? n : aDefault; }
    bool   IsObject() const { return type == OBJ; }
    bool   IsArray() const { return type == ARR; }
    bool   IsString() const { return type == STR; }
};

/// Parse UTF-8 JSON. Returns false (with a message) on malformed input.
bool ParseJson( const char* aData, size_t aLen, JValue& aOut, std::string& aError );

} // namespace kp

#endif
