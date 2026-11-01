/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "i18n.h"

#include <atomic>
#include <vector>

namespace kp
{

using Table = std::unordered_map<std::string, std::string>;

// Tables are never freed: a replaced table may still be read by a running load.
static std::atomic<const Table*> g_table { nullptr };


void SetTranslations( Table aTable )
{
    g_table.store( new Table( std::move( aTable ) ), std::memory_order_release );
}


const char* Tr( const char* aEnglish )
{
    const Table* t = g_table.load( std::memory_order_acquire );

    if( !t || !aEnglish )
        return aEnglish;

    auto it = t->find( aEnglish );
    return it != t->end() && !it->second.empty() ? it->second.c_str() : aEnglish;
}


std::string TrF( const char* aPattern, std::initializer_list<std::string> aArgs )
{
    std::string                    pattern = Tr( aPattern );
    std::vector<const std::string*> args;

    for( const std::string& a : aArgs )
        args.push_back( &a );

    std::string out;
    out.reserve( pattern.size() + 32 );

    for( size_t i = 0; i < pattern.size(); ++i )
    {
        if( pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' && pattern[i + 1] >= '0'
            && pattern[i + 1] <= '9' )
        {
            size_t k = (size_t) ( pattern[i + 1] - '0' );

            if( k < args.size() )
                out += *args[k];

            i += 2;
            continue;
        }

        out += pattern[i];
    }

    return out;
}

} // namespace kp
