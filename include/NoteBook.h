#pragma once

#include "AppPaths.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SCMiner {

// Notes with folders, kept as plain files: notes\<folder>\<note>.txt beside
// the exe. Nothing is packed into a database - a note is a text file, so it
// can be opened, searched, copied or backed up without this app, and a
// folder is a folder. Deleting moves to notes\.trash\ rather than erasing:
// a mis-click on a route someone spent an evening on is not worth an undo
// dialog nobody would trust.
class NoteBook {
public:
    struct Note {
        std::string name;                // what the file is called, without .txt
        std::filesystem::path path;
        std::string modified;            // "YYYY-MM-DD HH:MM"
        std::string preview;             // first non-empty line, for the list
        std::int64_t stamp = 0;          // for sorting, newest first
    };
    struct Folder {
        std::string name;
        std::filesystem::path path;
        std::vector<Note> notes;
        bool open = true;
    };

    static constexpr const char* ROOT = "notes";

    void load();                          // scans the tree from disk
    const std::vector<Folder>& folders() const { return folders_; }
    int noteCount() const;

    // Making things. Both return the index of what was made, -1 on failure.
    int addFolder(const std::string& name);
    int addNote(int folder, const std::string& name);

    bool renameFolder(int folder, const std::string& name);
    bool renameNote(int folder, int note, const std::string& name);
    // Into another folder, keeping the file and what is in it. Returns
    // where it landed, or -1.
    int moveNote(int folder, int note, int toFolder);
    // Both move into notes\.trash\ with the date on them, they do not erase
    bool trashFolder(int folder);
    bool trashNote(int folder, int note);

    // Folding a folder open or shut is a view state, but it belongs with the
    // tree rather than with the window that happens to draw it
    void toggleOpen(int folder);

    // What is in the trash, newest first, and the way back out. A note goes
    // back into the folder it came from (made again if it is gone), a folder
    // comes back whole. An older entry with no folder on record lands in
    // "Restored".
    struct Trashed {
        std::string name;        // as it was called
        std::string folder;      // where it came from, "" when unknown
        std::filesystem::path path;
        std::string when;        // "YYYY-MM-DD HH:MM"
        std::int64_t stamp = 0;
        bool isFolder = false;
        int notes = 0;           // a folder's notes, for the list
    };
    std::vector<Trashed> trashed() const;
    bool restore(const Trashed& item);

    std::string body(int folder, int note) const;
    bool writeBody(int folder, int note, const std::string& text);

    // ---- pictures kept with a note ------------------------------------
    // A note is a text file and stays one; a picture belongs beside it, in
    // a folder of its own named after the note:
    //
    //   notes\Mining\Nyx run.txt
    //   notes\Mining\Nyx run.media\20260914_152233.jpg
    //
    // The scan only ever reads *.txt files inside a folder, so the media
    // folder is invisible to it, and anyone can open the pictures in
    // Explorer without this app. They travel with the note: renaming,
    // moving and trashing carry the folder along, and the data backup takes
    // the whole tree anyway.
    static constexpr const char* MEDIA_SUFFIX = ".media";
    // Where a note's pictures live. `make` creates the folder; without it an
    // empty path comes back when there is none.
    std::filesystem::path mediaDir(int folder, int note, bool make = false) const;
    struct Shot {
        std::filesystem::path path;
        std::string when;        // "YYYY-MM-DD HH:MM"
        std::int64_t stamp = 0;
    };
    std::vector<Shot> media(int folder, int note) const;   // newest first
    // A picture goes to the trash the way a note does, never erased
    bool trashShot(const std::filesystem::path& shot);

    // ---- taking notes out, and bringing them back --------------------
    // One plain text file holds whole notes, so a route can be sent to
    // somebody, or kept as a backup, without either of you having to find
    // %LOCALAPPDATA% first.
    //
    // The format stays readable on purpose, and it is safe to read back
    // because every note says HOW MANY LINES it has: nothing inside a body
    // is ever looked at, so a note that itself contains the word NOTE, or
    // a row of tabs, is simply a note.
    //
    //   BEDROCK NOTES 1
    //   NOTE <tab> folder <tab> name <tab> 3
    //   ...three lines, exactly as they were...
    //   NOTE <tab> ...
    //
    // A folder or note name can hold no tab and no newline - sanitise()
    // drops the whole control range - so the head line cannot be confused.
    struct BundleNote {
        std::string folder;      // where it lived; empty lands in "Imported"
        std::string name;
        std::string body;
    };
    static constexpr const char* BUNDLE_MAGIC = "BEDROCK NOTES 1";
    // Everything (-1, -1), one folder (f, -1), or the one note (f, n)
    std::vector<BundleNote> collect(int folder = -1, int note = -1) const;
    static std::string writeBundle(const std::vector<BundleNote>& notes);
    // The structure only - whether the [[links]] inside still mean anything
    // is the window's business, see MinerGUI::brokenLink. On false `error`
    // holds a sentence saying what is wrong with the file.
    static bool readBundle(const std::string& text, std::vector<BundleNote>& out,
                           std::string& error);
    // In, never over: a folder that is already there is used, a note name
    // already taken gets a number, exactly as making one by hand does.
    // Returns how many notes landed.
    int importNotes(const std::vector<BundleNote>& notes);

    // A name that can be a file: the characters Windows forbids are dropped,
    // and a name already taken gets a number.
    static std::string sanitise(const std::string& raw);
    const std::string& lastError() const { return error_; }

private:
    std::vector<Folder> folders_;
    std::string error_;

    std::filesystem::path root() const { return AppPaths::data() / ROOT; }
    std::filesystem::path freePath(const std::filesystem::path& dir, const std::string& stem,
                                   const char* extension) const;
    static std::string stampOf(const std::filesystem::path& p, std::int64_t* raw);
    static std::string firstLine(const std::filesystem::path& p);
    bool valid(int folder, int note = -1) const;
};

}  // namespace SCMiner
