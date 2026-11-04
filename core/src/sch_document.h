/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Editable, lossless view of a schematic project: every .kicad_sch file is a kp::Document
 * (other files are kept as given), and all of them share one undo history. A step usually
 * touches one file, but may touch several (a KiCad 6 root file keeps the references of the
 * symbols of every sheet). The schematic model is rebuilt from the current texts on demand.
 */
#ifndef NETLOOM_SCH_DOCUMENT_H
#define NETLOOM_SCH_DOCUMENT_H

#include "document.h"
#include "schematic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kp
{

class SchDocument
{
public:
    /// Keep the files; aFiles[aRoot] is the root sheet. Throws kp::ParseError when the root
    /// sheet cannot be read.
    void Load( const std::vector<SchFile>& aFiles, size_t aRoot );

    size_t             FileCount() const { return m_files.size(); }
    size_t             Root() const { return m_root; }
    const std::string& FileName( size_t aFile ) const { return m_files[aFile].name; }
    std::string        FileText( size_t aFile ) const;
    bool               FileEditable( size_t aFile ) const { return m_files[aFile].editable; }
    bool               FileModified( size_t aFile ) const;

    /// The document of an editable file.
    const Document& Doc( size_t aFile ) const { return m_files[aFile].doc; }

    /// Schematic of the current texts (shared while the state does not change). Throws
    /// kp::ParseError.
    std::shared_ptr<const Schematic> Build();

    /// Identifies the current content of all files; changes with every edit, undo and redo.
    uint64_t StateId() const { return m_state; }

    // ---- editing: changes between Begin() and Commit() form one undo step
    void Begin( const std::string& aLabel );

    /// The document of an editable file, opened for changes in the current step.
    Document& Edit( int aFile );

    void Commit();
    void Cancel();
    bool InStep() const { return m_inStep; }

    bool        CanUndo() const { return !m_undo.empty(); }
    bool        CanRedo() const { return !m_redo.empty(); }
    std::string UndoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
    std::string RedoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
    bool        Undo();
    bool        Redo();

    bool Modified() const;
    /// Mark one file (or all files for aFile < 0) as saved.
    void MarkSaved( int aFile = -1 );

private:
    struct File
    {
        std::string name;
        std::string raw;      ///< content of a file that is not editable
        Document    doc;
        bool        editable = false;
    };

    struct Step
    {
        std::string      label;
        std::vector<int> files; ///< files changed by the step
        uint64_t         before = 0;
        uint64_t         after = 0;
    };

    std::vector<File>     m_files;
    size_t                m_root = 0;
    std::vector<Step>     m_undo, m_redo;
    std::string           m_label;
    std::vector<uint64_t> m_touched; ///< per file: its state when the open step first touched it, 0 = not
    bool                  m_inStep = false;
    uint64_t              m_state = 1;
    uint64_t              m_nextState = 2;

    std::shared_ptr<const Schematic> m_built;
    uint64_t                         m_builtState = 0;
};

} // namespace kp

#endif
