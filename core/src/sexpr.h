/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Minimal, allocation-light S-expression reader for KiCad files.
 *
 * The reader works on an in-memory buffer and builds one top-level element at a time into a
 * reusable node pool, so very large boards (tens of MB) can be read with bounded memory.
 */
#ifndef KIPAD_SEXPR_H
#define KIPAD_SEXPR_H

#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kp
{

class ParseError : public std::runtime_error
{
public:
    ParseError( const std::string& aMsg, size_t aOffset ) :
            std::runtime_error( aMsg ),
            offset( aOffset )
    {
    }

    size_t offset;
};

static constexpr uint32_t NIL = 0xFFFFFFFFu;

struct SNode
{
    enum : uint8_t
    {
        LIST,
        ATOM,   ///< unquoted symbol or number
        STRING  ///< quoted string
    };

    uint8_t     type = ATOM;
    uint32_t    first = NIL;  ///< first child (lists only)
    uint32_t    next = NIL;   ///< next sibling
    uint32_t    count = 0;    ///< number of children (lists only)
    const char* text = nullptr;
    uint32_t    len = 0;
};

class SPool
{
public:
    void clear()
    {
        nodes.clear();
        strings.clear();
    }

    std::vector<SNode>      nodes;
    std::deque<std::string> strings; ///< storage for unescaped strings
};

/// Parse a decimal number without locale dependence. Returns aDefault on failure.
double ParseNumber( std::string_view aText, double aDefault = 0.0, bool* aOk = nullptr );

/// Lightweight handle to a node in a pool.
class N
{
public:
    N() = default;
    N( const SPool* aPool, uint32_t aIndex ) :
            m_pool( aPool ),
            m_idx( aIndex )
    {
    }

    bool valid() const { return m_pool && m_idx != NIL; }
    explicit operator bool() const { return valid(); }

    bool isList() const { return valid() && node().type == SNode::LIST; }
    bool isAtom() const { return valid() && node().type != SNode::LIST; }
    bool isString() const { return valid() && node().type == SNode::STRING; }

    /// Text of an atom (empty for lists).
    std::string_view text() const
    {
        if( !isAtom() )
            return {};

        return std::string_view( node().text, node().len );
    }

    /// For a list: the text of its first child when it is an atom (the "keyword").
    std::string_view head() const
    {
        if( !isList() )
            return {};

        return first().text();
    }

    uint32_t size() const { return isList() ? node().count : 0; }

    N first() const { return isList() ? N( m_pool, node().first ) : N(); }
    N next() const { return valid() ? N( m_pool, node().next ) : N(); }

    /// k-th child (0 = keyword).
    N child( uint32_t k ) const
    {
        N c = first();

        while( c && k-- )
            c = c.next();

        return c;
    }

    /// k-th argument after the keyword.
    N arg( uint32_t k ) const { return child( k + 1 ); }

    double num( uint32_t k, double aDefault = 0.0 ) const
    {
        N a = arg( k );

        if( !a.isAtom() )
            return aDefault;

        bool   ok = false;
        double v = ParseNumber( a.text(), aDefault, &ok );
        return ok ? v : aDefault;
    }

    bool isNumber() const
    {
        if( !isAtom() || isString() )
            return false;

        bool ok = false;
        ParseNumber( text(), 0.0, &ok );
        return ok;
    }

    std::string_view str( uint32_t k ) const { return arg( k ).text(); }

    /// First child list whose keyword is aKey.
    N find( std::string_view aKey ) const
    {
        for( N c = first(); c; c = c.next() )
        {
            if( c.isList() && c.head() == aKey )
                return c;
        }

        return N();
    }

    /// True if a bare atom aFlag appears among the children (e.g. "hide", "mirror").
    bool hasAtom( std::string_view aFlag ) const
    {
        for( N c = first(); c; c = c.next() )
        {
            if( c.isAtom() && !c.isString() && c.text() == aFlag )
                return true;
        }

        return false;
    }

    /**
     * KiCad booleans appear as "(key yes)", "(key no)", "(key)" or a bare "key" atom.
     * Returns aDefault when the key is absent.
     */
    bool flag( std::string_view aKey, bool aDefault = false ) const
    {
        for( N c = first(); c; c = c.next() )
        {
            if( c.isAtom() && !c.isString() && c.text() == aKey )
                return true;

            if( c.isList() && c.head() == aKey )
            {
                N v = c.arg( 0 );

                if( !v )
                    return true;

                std::string_view t = v.text();
                return !( t == "no" || t == "false" || t == "0" );
            }
        }

        return aDefault;
    }

    template <typename F>
    void each( std::string_view aKey, F&& aFunc ) const
    {
        for( N c = first(); c; c = c.next() )
        {
            if( c.isList() && c.head() == aKey )
                aFunc( c );
        }
    }

private:
    const SNode& node() const { return m_pool->nodes[m_idx]; }

    const SPool* m_pool = nullptr;
    uint32_t     m_idx = NIL;
};

/**
 * Streaming reader. Usage:
 *   SReader r( data, len );
 *   std::string_view root = r.OpenRoot();       // e.g. "kicad_pcb"
 *   SPool pool;
 *   while( N item = r.NextTopLevel( pool ) ) { ... }
 */
class SReader
{
public:
    SReader( const char* aData, size_t aLen ) :
            m_begin( aData ),
            m_p( aData ),
            m_end( aData + aLen )
    {
    }

    /// Consume "(keyword" of the root list and return the keyword.
    std::string_view OpenRoot();

    /// Parse the next child of the root into aPool (cleared first). Invalid N at end.
    N NextTopLevel( SPool& aPool );

    /// Parse a complete standalone expression (whole buffer) into aPool.
    N ParseAll( SPool& aPool );

    size_t Offset() const { return (size_t) ( m_p - m_begin ); }
    size_t Length() const { return (size_t) ( m_end - m_begin ); }

private:
    void     skipSpace();
    uint32_t parseExpr( SPool& aPool );
    uint32_t parseAtom( SPool& aPool );
    [[noreturn]] void fail( const char* aMsg ) const;

    const char* m_begin;
    const char* m_p;
    const char* m_end;
};

} // namespace kp

#endif
