/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */
#include "sch_document.h"
#include "i18n.h"
#include "sexpr.h"

namespace kp
{

void SchDocument::Load( const std::vector<SchFile>& aFiles, size_t aRoot )
{
    *this = SchDocument();

    // the hierarchy decides which files are sheets of this schematic
    Schematic sch;
    LoadSchematic( sch, aFiles, aRoot );

    for( const SchScreen& sc : sch.screens )
    {
        if( sc.sourceFile < 0 || sc.sourceFile >= (int) aFiles.size() )
            continue;

        const SchFile& f = aFiles[(size_t) sc.sourceFile];
        Document       d;
        d.Load( f.data.data(), f.data.size() );
        m_names.push_back( f.name );
        m_docs.push_back( std::move( d ) );
    }

    if( m_docs.empty() )
        throw ParseError( Tr( "No schematic file" ), 0 );

    m_touched.assign( m_docs.size(), 0 );
}


std::shared_ptr<const SchDocState> SchDocument::Build() const
{
    auto                 st = std::make_shared<SchDocState>();
    std::vector<SchFile> files;
    files.reserve( m_docs.size() );

    for( size_t i = 0; i < m_docs.size(); ++i )
        files.push_back( { m_names[i], m_docs[i].Text() } );

    LoadSchematic( st->sch, files, 0 );

    for( const Document& d : m_docs )
        st->items.push_back( d.LiveItems() );

    st->state = m_state;
    return st;
}


void SchDocument::Begin( const std::string& aLabel )
{
    if( m_inStep )
        Commit();

    m_label = aLabel;
    m_touched.assign( m_docs.size(), 0 );
    m_inStep = true;
}


Document& SchDocument::touch( int aFile )
{
    Document& d = m_docs[(size_t) aFile];

    if( !m_inStep )
        Begin( Tr( "Change" ) );

    if( m_touched[(size_t) aFile] == 0 )
    {
        m_touched[(size_t) aFile] = d.StateId();
        d.Begin( m_label );
    }

    return d;
}


void SchDocument::Replace( int aFile, int aItem, const std::string& aBody )
{
    if( aFile >= 0 && aFile < (int) m_docs.size() )
        touch( aFile ).Replace( aItem, aBody );
}


void SchDocument::Remove( int aFile, int aItem )
{
    if( aFile >= 0 && aFile < (int) m_docs.size() )
        touch( aFile ).Remove( aItem );
}


int SchDocument::InsertBefore( int aFile, int aItem, const std::string& aBody, const std::string& aWhitespace )
{
    if( aFile < 0 || aFile >= (int) m_docs.size() )
        return -1;

    return touch( aFile ).InsertBefore( aItem, aBody, aWhitespace );
}


int SchDocument::InsertAfter( int aFile, int aItem, const std::string& aBody, const std::string& aWhitespace )
{
    if( aFile < 0 || aFile >= (int) m_docs.size() )
        return -1;

    return touch( aFile ).InsertAfter( aItem, aBody, aWhitespace );
}


void SchDocument::Commit()
{
    if( !m_inStep )
        return;

    m_inStep = false;
    Step step;
    step.label = m_label;

    for( size_t f = 0; f < m_docs.size(); ++f )
    {
        if( m_touched[f] == 0 )
            continue;

        m_docs[f].Commit();

        if( m_docs[f].StateId() != m_touched[f] )
            step.files.push_back( (int) f );
    }

    m_touched.assign( m_docs.size(), 0 );

    if( step.files.empty() )
        return;

    step.before = m_state;
    step.after = m_nextState++;
    m_state = step.after;
    m_undo.push_back( std::move( step ) );
    m_redo.clear();

    // every file's own history must follow the shared one
    for( Document& d : m_docs )
        d.ClearRedo();
}


void SchDocument::Cancel()
{
    if( !m_inStep )
        return;

    for( size_t f = 0; f < m_docs.size(); ++f )
    {
        if( m_touched[f] != 0 )
            m_docs[f].Cancel();
    }

    m_touched.assign( m_docs.size(), 0 );
    m_inStep = false;
}


bool SchDocument::Undo()
{
    Cancel();

    if( m_undo.empty() )
        return false;

    Step s = std::move( m_undo.back() );
    m_undo.pop_back();

    for( auto it = s.files.rbegin(); it != s.files.rend(); ++it )
        m_docs[(size_t) *it].Undo();

    m_state = s.before;
    m_redo.push_back( std::move( s ) );
    return true;
}


bool SchDocument::Redo()
{
    Cancel();

    if( m_redo.empty() )
        return false;

    Step s = std::move( m_redo.back() );
    m_redo.pop_back();

    for( int f : s.files )
        m_docs[(size_t) f].Redo();

    m_state = s.after;
    m_undo.push_back( std::move( s ) );
    return true;
}


bool SchDocument::Modified() const
{
    for( const Document& d : m_docs )
    {
        if( d.Modified() )
            return true;
    }

    return false;
}


void SchDocument::MarkSaved()
{
    for( Document& d : m_docs )
        d.MarkSaved();
}

} // namespace kp
