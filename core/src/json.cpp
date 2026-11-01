/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "json.h"

#include <cstdlib>
#include <cstring>

namespace kp
{

const JValue* JValue::Get( const char* aKey ) const
{
    if( type != OBJ )
        return nullptr;

    for( const auto& [k, v] : obj )
    {
        if( k == aKey )
            return &v;
    }

    return nullptr;
}


const JValue* JValue::Path( std::initializer_list<const char*> aKeys ) const
{
    const JValue* v = this;

    for( const char* k : aKeys )
    {
        v = v ? v->Get( k ) : nullptr;

        if( !v )
            return nullptr;
    }

    return v;
}


namespace
{

class Reader
{
public:
    Reader( const char* d, size_t n ) :
            p( d ),
            end( d + n )
    {
    }

    bool value( JValue& out, int depth )
    {
        if( depth > 200 )
            return fail( "nesting too deep" );

        ws();

        if( p >= end )
            return fail( "unexpected end" );

        switch( *p )
        {
        case '{': return object( out, depth );
        case '[': return array( out, depth );
        case '"': out.type = JValue::STR; return string( out.s );
        case 't': return literal( "true", out, JValue::BOOL, true );
        case 'f': return literal( "false", out, JValue::BOOL, false );
        case 'n': return literal( "null", out, JValue::NUL, false );
        default: return number( out );
        }
    }

    void ws()
    {
        while( p < end && ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) )
            ++p;
    }

    bool fail( const char* m )
    {
        if( err.empty() )
            err = m;

        return false;
    }

    bool literal( const char* word, JValue& out, JValue::Type t, bool b )
    {
        size_t n = strlen( word );

        if( (size_t) ( end - p ) < n || strncmp( p, word, n ) != 0 )
            return fail( "invalid literal" );

        p += n;
        out.type = t;
        out.b = b;
        return true;
    }

    bool number( JValue& out )
    {
        const char* s = p;

        if( p < end && ( *p == '-' || *p == '+' ) )
            ++p;

        while( p < end && ( ( *p >= '0' && *p <= '9' ) || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+' ) )
            ++p;

        if( p == s )
            return fail( "unexpected character" );

        std::string tmp( s, p );
        char*       e = nullptr;
        out.n = strtod( tmp.c_str(), &e );
        out.type = JValue::NUM;

        if( !e || *e != 0 )
            return fail( "invalid number" );

        return true;
    }

    static void utf8( std::string& o, unsigned cp )
    {
        if( cp < 0x80 )
            o += (char) cp;
        else if( cp < 0x800 )
        {
            o += (char) ( 0xC0 | ( cp >> 6 ) );
            o += (char) ( 0x80 | ( cp & 0x3F ) );
        }
        else if( cp < 0x10000 )
        {
            o += (char) ( 0xE0 | ( cp >> 12 ) );
            o += (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
            o += (char) ( 0x80 | ( cp & 0x3F ) );
        }
        else
        {
            o += (char) ( 0xF0 | ( cp >> 18 ) );
            o += (char) ( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
            o += (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
            o += (char) ( 0x80 | ( cp & 0x3F ) );
        }
    }

    bool hex4( unsigned& cp )
    {
        if( end - p < 4 )
            return fail( "bad escape" );

        cp = 0;

        for( int i = 0; i < 4; ++i )
        {
            char     c = *p++;
            unsigned v;

            if( c >= '0' && c <= '9' )
                v = (unsigned) ( c - '0' );
            else if( c >= 'a' && c <= 'f' )
                v = (unsigned) ( c - 'a' + 10 );
            else if( c >= 'A' && c <= 'F' )
                v = (unsigned) ( c - 'A' + 10 );
            else
                return fail( "bad escape" );

            cp = cp * 16 + v;
        }

        return true;
    }

    bool string( std::string& o )
    {
        ++p; // opening quote
        o.clear();

        while( p < end && *p != '"' )
        {
            char c = *p++;

            if( c != '\\' )
            {
                o += c;
                continue;
            }

            if( p >= end )
                return fail( "bad escape" );

            char e = *p++;

            switch( e )
            {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u':
            {
                unsigned cp;

                if( !hex4( cp ) )
                    return false;

                if( cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' )
                {
                    p += 2;
                    unsigned lo;

                    if( !hex4( lo ) )
                        return false;

                    cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
                }

                utf8( o, cp );
                break;
            }
            default: return fail( "bad escape" );
            }
        }

        if( p >= end )
            return fail( "unterminated string" );

        ++p;
        return true;
    }

    bool array( JValue& out, int depth )
    {
        ++p;
        out.type = JValue::ARR;
        ws();

        if( p < end && *p == ']' )
        {
            ++p;
            return true;
        }

        while( true )
        {
            out.arr.emplace_back();

            if( !value( out.arr.back(), depth + 1 ) )
                return false;

            ws();

            if( p < end && *p == ',' )
            {
                ++p;
                continue;
            }

            if( p < end && *p == ']' )
            {
                ++p;
                return true;
            }

            return fail( "expected , or ]" );
        }
    }

    bool object( JValue& out, int depth )
    {
        ++p;
        out.type = JValue::OBJ;
        ws();

        if( p < end && *p == '}' )
        {
            ++p;
            return true;
        }

        while( true )
        {
            ws();

            if( p >= end || *p != '"' )
                return fail( "expected key" );

            std::string key;

            if( !string( key ) )
                return false;

            ws();

            if( p >= end || *p != ':' )
                return fail( "expected :" );

            ++p;
            out.obj.emplace_back( std::move( key ), JValue() );

            if( !value( out.obj.back().second, depth + 1 ) )
                return false;

            ws();

            if( p < end && *p == ',' )
            {
                ++p;
                continue;
            }

            if( p < end && *p == '}' )
            {
                ++p;
                return true;
            }

            return fail( "expected , or }" );
        }
    }

    const char* p;
    const char* end;
    std::string err;
};

} // anonymous namespace


bool ParseJson( const char* aData, size_t aLen, JValue& aOut, std::string& aError )
{
    aOut = JValue();

    if( !aData )
    {
        aError = "no data";
        return false;
    }

    // skip a UTF-8 byte order mark
    if( aLen >= 3 && (unsigned char) aData[0] == 0xEF && (unsigned char) aData[1] == 0xBB
        && (unsigned char) aData[2] == 0xBF )
    {
        aData += 3;
        aLen -= 3;
    }

    Reader r( aData, aLen );

    if( !r.value( aOut, 0 ) )
    {
        aError = r.err;
        return false;
    }

    r.ws();

    if( r.p != r.end )
    {
        aError = "trailing characters";
        return false;
    }

    return true;
}

} // namespace kp
