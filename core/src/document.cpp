/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "document.h"
#include "i18n.h"
#include "sexpr.h"

namespace kp
{

void Document::Load( const char* aData, size_t aLen )
{
    *this = Document();

    SReader r( aData, aLen );
    r.OpenRoot();
    size_t prefixEnd = r.Offset();
    m_prefix.assign( aData, prefixEnd );

    size_t prevEnd = prefixEnd;
    size_t start = 0, end = 0;

    while( r.SkipTopLevel( start, end ) )
    {
        DocItem it;
        it.ws.assign( aData + prevEnd, start - prevEnd );
        it.body.assign( aData + start, end - start );
        m_items.push_back( std::move( it ) );
        prevEnd = end;
    }

    m_suffix.assign( aData + prevEnd, aLen - prevEnd );
}


std::string Document::Text() const
{
    size_t total = m_prefix.size() + m_suffix.size();

    for( const DocItem& it : m_items )
    {
        if( !it.removed )
            total += it.ws.size() + it.body.size();
    }

    std::string out;
    out.reserve( total );
    out += m_prefix;

    for( const DocItem& it : m_items )
    {
        if( !it.removed )
        {
            out += it.ws;
            out += it.body;
        }
    }

    out += m_suffix;
    return out;
}


std::vector<int> Document::LiveItems() const
{
    std::vector<int> out;
    out.reserve( m_items.size() );

    for( size_t i = 0; i < m_items.size(); ++i )
    {
        if( !m_items[i].removed )
            out.push_back( (int) i );
    }

    return out;
}


void Document::Begin( const std::string& aLabel )
{
    if( m_inStep )
        Commit();

    m_open = Step();
    m_open.label = aLabel;
    m_open.before = m_state;
    m_inStep = true;
}


void Document::record( int aIndex, const std::string& aNewBody, bool aNewRemoved )
{
    DocItem& it = m_items[(size_t) aIndex];

    // merge repeated changes of the same item within one step
    for( Change& c : m_open.changes )
    {
        if( c.index == aIndex )
        {
            c.newBody = aNewBody;
            c.newRemoved = aNewRemoved;
            it.body = aNewBody;
            it.removed = aNewRemoved;
            return;
        }
    }

    m_open.changes.push_back( { aIndex, it.body, aNewBody, it.removed, aNewRemoved } );
    it.body = aNewBody;
    it.removed = aNewRemoved;
}


void Document::Replace( int aIndex, const std::string& aBody )
{
    if( aIndex < 0 || aIndex >= (int) m_items.size() )
        return;

    bool autoStep = !m_inStep;

    if( autoStep )
        Begin( Tr( "Change" ) );

    record( aIndex, aBody, false );

    if( autoStep )
        Commit();
}


void Document::Remove( int aIndex )
{
    if( aIndex < 0 || aIndex >= (int) m_items.size() || m_items[(size_t) aIndex].removed )
        return;

    bool autoStep = !m_inStep;

    if( autoStep )
        Begin( Tr( "Delete" ) );

    record( aIndex, m_items[(size_t) aIndex].body, true );

    if( autoStep )
        Commit();
}


int Document::Append( const std::string& aBody )
{
    bool autoStep = !m_inStep;

    if( autoStep )
        Begin( Tr( "Add" ) );

    DocItem it;
    it.ws = "\n\t";
    it.body = std::string();
    it.removed = true; // does not exist before the step
    m_items.push_back( it );
    int idx = (int) m_items.size() - 1;
    record( idx, aBody, false );

    if( autoStep )
        Commit();

    return idx;
}


void Document::Commit()
{
    if( !m_inStep )
        return;

    m_inStep = false;

    if( m_open.changes.empty() )
        return;

    m_open.after = m_nextState++;
    m_state = m_open.after;
    m_undo.push_back( std::move( m_open ) );
    m_redo.clear();
    m_open = Step();
}


void Document::Cancel()
{
    if( !m_inStep )
        return;

    for( auto it = m_open.changes.rbegin(); it != m_open.changes.rend(); ++it )
    {
        m_items[(size_t) it->index].body = it->oldBody;
        m_items[(size_t) it->index].removed = it->oldRemoved;
    }

    m_open = Step();
    m_inStep = false;
}


bool Document::Undo()
{
    if( m_inStep )
        Cancel();

    if( m_undo.empty() )
        return false;

    Step s = std::move( m_undo.back() );
    m_undo.pop_back();

    for( auto it = s.changes.rbegin(); it != s.changes.rend(); ++it )
    {
        m_items[(size_t) it->index].body = it->oldBody;
        m_items[(size_t) it->index].removed = it->oldRemoved;
    }

    m_state = s.before;
    m_redo.push_back( std::move( s ) );
    return true;
}


bool Document::Redo()
{
    if( m_inStep )
        Cancel();

    if( m_redo.empty() )
        return false;

    Step s = std::move( m_redo.back() );
    m_redo.pop_back();

    for( const Change& c : s.changes )
    {
        m_items[(size_t) c.index].body = c.newBody;
        m_items[(size_t) c.index].removed = c.newRemoved;
    }

    m_state = s.after;
    m_undo.push_back( std::move( s ) );
    return true;
}

} // namespace kp
