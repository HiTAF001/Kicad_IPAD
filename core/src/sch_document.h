/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Editable, lossless view of a schematic hierarchy: every sheet file (root and sub-sheets) is
 * a kp::Document, and all of them share one undo history, so a step may touch several files
 * (a KiCad 6 root keeps the references of the symbols of every sheet). The schematic model is
 * rebuilt from the current texts whenever it is needed.
 */
#ifndef NETLOOM_SCH_DOCUMENT_H
#define NETLOOM_SCH_DOCUMENT_H

#include "document.h"
#include "schematic.h"

#include <memory>
#include <string>
#include <vector>

namespace kp
{

/// The schematic built from one state of a SchDocument.
struct SchDocState
{
    Schematic                     sch;   ///< screens[i].sourceFile = file index in the document
    std::vector<std::vector<int>> items; ///< per file: parse index -> document item index
    uint64_t                      state = 0;
};

class SchDocument
{
public:
    /// Keep the files the hierarchy below aFiles[aRoot] uses. Throws kp::ParseError.
    void Load( const std::vector<SchFile>& aFiles, size_t aRoot );

    size_t             FileCount() const { return m_docs.size(); }
    const std::string& FileName( size_t aFile ) const { return m_names[aFile]; }
    std::string        FileText( size_t aFile ) const { return m_docs[aFile].Text(); }
    bool               FileModified( size_t aFile ) const { return m_docs[aFile].Modified(); }
    const Document&    Doc( size_t aFile ) const { return m_docs[aFile]; }

    /// Schematic of the current texts. Throws kp::ParseError.
    std::shared_ptr<const SchDocState> Build() const;

    // ---- editing: changes between Begin() and Commit() form one undo step
    void Begin( const std::string& aLabel );
    void Replace( int aFile, int aItem, const std::string& aBody );
    void Remove( int aFile, int aItem );
    int  InsertBefore( int aFile, int aItem, const std::string& aBody, const std::string& aWhitespace );
    int  InsertAfter( int aFile, int aItem, const std::string& aBody, const std::string& aWhitespace );
    void Commit();
    void Cancel();

    bool        CanUndo() const { return !m_undo.empty(); }
    bool        CanRedo() const { return !m_redo.empty(); }
    std::string UndoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
    std::string RedoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
    bool        Undo();
    bool        Redo();

    bool     Modified() const;
    void     MarkSaved();
    uint64_t StateId() const { return m_state; }

private:
    struct Step
    {
        std::string      label;
        std::vector<int> files; ///< files changed by the step
        uint64_t         before = 0;
        uint64_t         after = 0;
    };

    Document& touch( int aFile );

    std::vector<std::string> m_names;
    std::vector<Document>    m_docs;
    std::vector<Step>        m_undo, m_redo;
    std::string              m_label;
    std::vector<uint64_t>    m_touched; ///< per file: its state when the open step first touched it, 0 = not
    bool                     m_inStep = false;
    uint64_t                 m_state = 1;
    uint64_t                 m_nextState = 2;
};

} // namespace kp

#endif
