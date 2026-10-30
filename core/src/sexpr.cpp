/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "sexpr.h"

#include <cmath>

namespace kp
{

double ParseNumber( std::string_view aText, double aDefault, bool* aOk )
{
    const char* p = aText.data();
    const char* e = p + aText.size();

    if( aOk )
        *aOk = false;

    if( p == e )
        return aDefault;

    bool neg = false;

    if( *p == '+' || *p == '-' )
    {
        neg = ( *p == '-' );
        ++p;
    }

    uint64_t mant = 0;
    int      digits = 0;
    int      exp10 = 0;
    bool     any = false;

    while( p < e && *p >= '0' && *p <= '9' )
    {
        if( digits < 18 )
        {
            mant = mant * 10 + (uint64_t) ( *p - '0' );

            if( mant )
                ++digits;
        }
        else
        {
            ++exp10;
        }

        any = true;
        ++p;
    }

    if( p < e && *p == '.' )
    {
        ++p;

        while( p < e && *p >= '0' && *p <= '9' )
        {
            if( digits < 18 )
            {
                mant = mant * 10 + (uint64_t) ( *p - '0' );

                if( mant )
                    ++digits;

                --exp10;
            }

            any = true;
            ++p;
        }
    }

    if( !any )
        return aDefault;

    if( p < e && ( *p == 'e' || *p == 'E' ) )
    {
        ++p;
        bool eneg = false;

        if( p < e && ( *p == '+' || *p == '-' ) )
        {
            eneg = ( *p == '-' );
            ++p;
        }

        int  ev = 0;
        bool eany = false;

        while( p < e && *p >= '0' && *p <= '9' )
        {
            if( ev < 10000 )
                ev = ev * 10 + ( *p - '0' );

            eany = true;
            ++p;
        }

        if( !eany )
            return aDefault;

        exp10 += eneg ? -ev : ev;
    }

    if( p != e )
        return aDefault;

    static const double pow10[] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    double v = (double) mant;

    if( exp10 < 0 && exp10 >= -22 )
        v /= pow10[-exp10];
    else if( exp10 > 0 && exp10 <= 22 )
        v *= pow10[exp10];
    else if( exp10 != 0 )
        v *= std::pow( 10.0, exp10 );

    if( aOk )
        *aOk = true;

    return neg ? -v : v;
}


void SReader::fail( const char* aMsg ) const
{
    // compute line number for a friendlier message
    size_t line = 1;

    for( const char* c = m_begin; c < m_p && c < m_end; ++c )
    {
        if( *c == '\n' )
            ++line;
    }

    throw ParseError( std::string( aMsg ) + " (satır " + std::to_string( line ) + ")",
                      (size_t) ( m_p - m_begin ) );
}


void SReader::skipSpace()
{
    while( m_p < m_end )
    {
        char c = *m_p;

        if( c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' )
        {
            ++m_p;
        }
        else if( c == '#' && ( m_p == m_begin || m_p[-1] == '\n' ) )
        {
            // comment line (allowed by DSNLEXER at line start)
            while( m_p < m_end && *m_p != '\n' )
                ++m_p;
        }
        else
        {
            break;
        }
    }
}


std::string_view SReader::OpenRoot()
{
    // skip UTF-8 BOM
    if( m_end - m_p >= 3 && (unsigned char) m_p[0] == 0xEF && (unsigned char) m_p[1] == 0xBB
        && (unsigned char) m_p[2] == 0xBF )
    {
        m_p += 3;
    }

    skipSpace();

    if( m_p >= m_end || *m_p != '(' )
        fail( "Dosya bir KiCad S-ifadesiyle başlamıyor" );

    ++m_p;
    skipSpace();

    const char* s = m_p;

    while( m_p < m_end && *m_p != ' ' && *m_p != '\t' && *m_p != '\n' && *m_p != '\r'
           && *m_p != '(' && *m_p != ')' )
    {
        ++m_p;
    }

    return std::string_view( s, (size_t) ( m_p - s ) );
}


N SReader::NextTopLevel( SPool& aPool )
{
    aPool.clear();

    for( ;; )
    {
        skipSpace();

        if( m_p >= m_end )
            return N(); // tolerate missing closing paren

        if( *m_p == ')' )
        {
            ++m_p;
            return N();
        }

        uint32_t idx = parseExpr( aPool );

        if( aPool.nodes[idx].type == SNode::LIST )
            return N( &aPool, idx );

        // stray atom at top level: ignore and continue
        aPool.clear();
    }
}


N SReader::ParseAll( SPool& aPool )
{
    aPool.clear();
    skipSpace();

    if( m_p >= m_end )
        return N();

    return N( &aPool, parseExpr( aPool ) );
}


uint32_t SReader::parseExpr( SPool& aPool )
{
    // Iterative list parsing to avoid deep recursion on pathological files.
    struct Frame
    {
        uint32_t list;
        uint32_t last;
    };

    skipSpace();

    if( m_p >= m_end )
        fail( "Beklenmeyen dosya sonu" );

    if( *m_p != '(' )
        return parseAtom( aPool );

    std::vector<Frame> stack;
    stack.reserve( 16 );

    auto newList = [&]() -> uint32_t
    {
        aPool.nodes.emplace_back();
        aPool.nodes.back().type = SNode::LIST;
        return (uint32_t) ( aPool.nodes.size() - 1 );
    };

    ++m_p;
    uint32_t root = newList();
    stack.push_back( { root, NIL } );

    auto append = [&]( uint32_t aChild )
    {
        Frame& f = stack.back();

        if( f.last == NIL )
            aPool.nodes[f.list].first = aChild;
        else
            aPool.nodes[f.last].next = aChild;

        f.last = aChild;
        aPool.nodes[f.list].count++;
    };

    while( !stack.empty() )
    {
        skipSpace();

        if( m_p >= m_end )
            fail( "Kapanmamış parantez" );

        char c = *m_p;

        if( c == ')' )
        {
            ++m_p;
            stack.pop_back();
        }
        else if( c == '(' )
        {
            ++m_p;
            uint32_t l = newList();
            append( l );
            stack.push_back( { l, NIL } );
        }
        else
        {
            uint32_t a = parseAtom( aPool );
            append( a );
        }
    }

    return root;
}


uint32_t SReader::parseAtom( SPool& aPool )
{
    SNode n;

    if( *m_p == '"' )
    {
        ++m_p;
        const char* s = m_p;
        bool        hasEscape = false;

        while( m_p < m_end && *m_p != '"' )
        {
            if( *m_p == '\\' && m_p + 1 < m_end )
            {
                hasEscape = true;
                m_p += 2;
            }
            else
            {
                ++m_p;
            }
        }

        if( m_p >= m_end )
            fail( "Kapanmamış tırnak" );

        const char* e = m_p;
        ++m_p; // closing quote

        n.type = SNode::STRING;

        if( !hasEscape )
        {
            n.text = s;
            n.len = (uint32_t) ( e - s );
        }
        else
        {
            std::string out;
            out.reserve( (size_t) ( e - s ) );

            for( const char* c = s; c < e; ++c )
            {
                if( *c != '\\' || c + 1 >= e )
                {
                    out.push_back( *c );
                    continue;
                }

                ++c;

                switch( *c )
                {
                case 'n': out.push_back( '\n' ); break;
                case 'r': out.push_back( '\r' ); break;
                case 't': out.push_back( '\t' ); break;
                case 'a': out.push_back( '\a' ); break;
                case 'b': out.push_back( '\b' ); break;
                case 'f': out.push_back( '\f' ); break;
                case 'v': out.push_back( '\v' ); break;
                case 'x':
                {
                    int v = 0;
                    int k = 0;

                    while( k < 2 && c + 1 < e && isxdigit( (unsigned char) c[1] ) )
                    {
                        ++c;
                        v = v * 16 + ( isdigit( (unsigned char) *c ) ? *c - '0'
                                                                    : ( tolower( *c ) - 'a' + 10 ) );
                        ++k;
                    }

                    out.push_back( (char) v );
                    break;
                }
                default:
                    if( *c >= '0' && *c <= '7' )
                    {
                        int v = *c - '0';
                        int k = 1;

                        while( k < 3 && c + 1 < e && c[1] >= '0' && c[1] <= '7' )
                        {
                            ++c;
                            v = v * 8 + ( *c - '0' );
                            ++k;
                        }

                        out.push_back( (char) v );
                    }
                    else
                    {
                        out.push_back( *c ); // \" \\ and unknown escapes
                    }
                }
            }

            aPool.strings.push_back( std::move( out ) );
            const std::string& stored = aPool.strings.back();
            n.text = stored.data();
            n.len = (uint32_t) stored.size();
        }
    }
    else
    {
        const char* s = m_p;

        while( m_p < m_end )
        {
            char c = *m_p;

            if( c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' )
                break;

            ++m_p;
        }

        n.type = SNode::ATOM;
        n.text = s;
        n.len = (uint32_t) ( m_p - s );
    }

    aPool.nodes.push_back( n );
    return (uint32_t) ( aPool.nodes.size() - 1 );
}

} // namespace kp
