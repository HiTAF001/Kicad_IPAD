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

    if( aRoot >= aFiles.size() )
        throw ParseError( Tr( "No schematic file" ), 0 );

    for( size_t i = 0; i < aFiles.size(); ++i )
    {
        File f;
        f.name = aFiles[i].name;

        if( LooksLikeSchematic( aFiles[i].data.data(), aFiles[i].data.size() ) )
        {
            try
            {
                f.doc.Load( aFiles[i].data.data(), aFiles[i].data.size() );
                f.editable = true;
            }
            catch( const ParseError& )
            {
                if( i == aRoot )
                    throw;
            }
        }

        if( !f.editable )
        {
            if( i == aRoot )
                throw ParseError( Tr( "Not a KiCad schematic file" ), 0 );

            f.raw = aFiles[i].data;
        }

        m_files.push_back( std::move( f ) );
    }

    m_root = aRoot;
    m_touched.assign( m_files.size(), 0 );
}


std::string SchDocument::FileText( size_t aFile ) const
{
    const File& f = m_files[aFile];
    return f.editable ? f.doc.Text() : f.raw;
}


bool SchDocument::FileModified( size_t aFile ) const
{
    return m_files[aFile].editable && m_files[aFile].doc.Modified();
}


std::shared_ptr<const Schematic> SchDocument::Build()
{
    if( m_built && m_builtState == m_state )
        return m_built;

    auto                 sch = std::make_shared<Schematic>();
    std::vector<SchFile> files;
    files.reserve( m_files.size() );

    for( size_t i = 0; i < m_files.size(); ++i )
        files.push_back( { m_files[i].name, FileText( i ) } );

    LoadSchematic( *sch, files, m_root );
    m_built = sch;
    m_builtState = m_state;
    return m_built;
}


void SchDocument::Begin( const std::string& aLabel )
{
    if( m_inStep )
        Commit();

    m_label = aLabel;
    m_touched.assign( m_files.size(), 0 );
    m_inStep = true;
}


Document& SchDocument::Edit( int aFile )
{
    Document& d = m_files[(size_t) aFile].doc;

    if( !m_inStep )
        Begin( Tr( "Change" ) );

    if( m_touched[(size_t) aFile] == 0 )
    {
        m_touched[(size_t) aFile] = d.StateId();
        d.Begin( m_label );
    }

    return d;
}


void SchDocument::Commit()
{
    if( !m_inStep )
        return;

    m_inStep = false;
    Step step;
    step.label = m_label;

    for( size_t f = 0; f < m_files.size(); ++f )
    {
        if( m_touched[f] == 0 )
            continue;

        m_files[f].doc.Commit();

        if( m_files[f].doc.StateId() != m_touched[f] )
            step.files.push_back( (int) f );
    }

    m_touched.assign( m_files.size(), 0 );

    if( step.files.empty() )
        return;

    step.before = m_state;
    step.after = m_nextState++;
    m_state = step.after;
    m_undo.push_back( std::move( step ) );
    m_redo.clear();

    // the files' own histories follow the shared one
    for( File& f : m_files )
        f.doc.ClearRedo();
}


void SchDocument::Cancel()
{
    if( !m_inStep )
        return;

    for( size_t f = 0; f < m_files.size(); ++f )
    {
        if( m_touched[f] != 0 )
            m_files[f].doc.Cancel();
    }

    m_touched.assign( m_files.size(), 0 );
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
        m_files[(size_t) *it].doc.Undo();

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
        m_files[(size_t) f].doc.Redo();

    m_state = s.after;
    m_undo.push_back( std::move( s ) );
    return true;
}


bool SchDocument::Modified() const
{
    for( size_t i = 0; i < m_files.size(); ++i )
    {
        if( FileModified( i ) )
            return true;
    }

    return false;
}


void SchDocument::MarkSaved( int aFile )
{
    for( size_t i = 0; i < m_files.size(); ++i )
    {
        if( aFile < 0 || (size_t) aFile == i )
            m_files[i].doc.MarkSaved();
    }
}

} // namespace kp
