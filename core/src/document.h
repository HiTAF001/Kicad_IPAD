/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Editable, lossless view of a .kicad_pcb file.
 *
 * The file is split into its top-level items (footprints, segments, vias, zones, ...). Edits
 * replace, remove or append whole items; everything else is written back byte for byte, so
 * data Netloom does not understand (stackup, net classes, 3D models, groups, ...) survives and
 * diffs stay small. Item indices never change (removed items become tombstones), which keeps
 * undo/redo simple. New items go at the end, or right after an existing item (InsertAfter);
 * the file order is kept separately from the indices.
 */
#ifndef NETLOOM_DOCUMENT_H
#define NETLOOM_DOCUMENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace kp
{

struct DocItem
{
    std::string ws;   ///< whitespace (and anything stray) before the item
    std::string body; ///< the item itself, "(...)"
    bool        removed = false;
};

class Document
{
public:
    /// Split a file into items. Throws kp::ParseError.
    void Load( const char* aData, size_t aLen );

    std::string Text() const;

    size_t         ItemCount() const { return m_items.size(); }
    const DocItem& Item( size_t aIndex ) const { return m_items[aIndex]; }

    /// Indices of the items that are present, in file order (= parse order of the text).
    std::vector<int> LiveItems() const;

    /// Identifies the current content; changes with every edit, undo and redo.
    uint64_t StateId() const { return m_state; }

    // ---- editing: changes between Begin() and Commit() form one undo step
    void Begin( const std::string& aLabel );
    void Replace( int aIndex, const std::string& aBody );
    void Remove( int aIndex );
    int  Append( const std::string& aBody, const std::string& aWhitespace = "\n\t" );

    /// Like Append, but the new item goes right after item aAnchor in the file (e.g. a net
    /// declaration next to the others, which a reader needs before the first item using it).
    int InsertAfter( int aAnchor, const std::string& aBody, const std::string& aWhitespace );

    /// Whitespace in front of the last present item (to indent appended items alike).
    std::string LastItemWhitespace() const;
    void Commit();
    void Cancel();

    bool        CanUndo() const { return !m_undo.empty(); }
    bool        CanRedo() const { return !m_redo.empty(); }
    std::string UndoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
    std::string RedoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
    bool        Undo();
    bool        Redo();

    bool Modified() const { return m_state != m_savedState; }
    void MarkSaved() { m_savedState = m_state; }

private:
    struct Change
    {
        int         index;
        std::string oldBody, newBody;
        bool        oldRemoved, newRemoved;
    };

    struct Step
    {
        std::string         label;
        std::vector<Change> changes;
        uint64_t            before = 0;
        uint64_t            after = 0;
    };

    void record( int aIndex, const std::string& aNewBody, bool aNewRemoved );

    std::string          m_prefix;
    std::string          m_suffix;
    std::vector<DocItem> m_items;
    std::vector<int>     m_order; ///< item indices in file order
    std::vector<Step>    m_undo, m_redo;
    Step                 m_open;
    bool                 m_inStep = false;
    uint64_t             m_state = 1;
    uint64_t             m_savedState = 1;
    uint64_t             m_nextState = 2;
};

} // namespace kp

#endif
