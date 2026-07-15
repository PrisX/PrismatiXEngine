#pragma once

namespace px::editor {

enum class EditorIcon {
    Add, Import, Folder, File, Details, Compact, Thumbnails,
    Search, Save, Play, Stop, Build, Warning, Error, Pin,
    Undo, Redo, Duplicate, Delete, Reset, Lock, Visible, More
};

// Font Awesome Free codepoints. LoadFonts merges the solid icon font when the
// distributable asset is present; labels still retain text for accessibility.
[[nodiscard]] inline const char* Icon(EditorIcon icon) {
    switch (icon) {
        case EditorIcon::Add: return "\xEF\x81\xA7";       // f067
        case EditorIcon::Import: return "\xEF\x80\x99";    // f019
        case EditorIcon::Folder: return "\xEF\x81\xBB";    // f07b
        case EditorIcon::File: return "\xEF\x85\x9B";      // f15b
        case EditorIcon::Details: return "\xEF\x80\xBA";   // f03a
        case EditorIcon::Compact: return "\xEF\x80\x8A";   // f00a
        case EditorIcon::Thumbnails: return "\xEF\x80\x8A";
        case EditorIcon::Search: return "\xEF\x80\x82";    // f002
        case EditorIcon::Save: return "\xEF\x83\x87";      // f0c7
        case EditorIcon::Play: return "\xEF\x81\x8B";      // f04b
        case EditorIcon::Stop: return "\xEF\x81\x8D";      // f04d
        case EditorIcon::Build: return "\xEF\x86\x87";     // f187
        case EditorIcon::Warning: return "\xEF\x81\xB1";   // f071
        case EditorIcon::Error: return "\xEF\x81\x97";     // f057
        case EditorIcon::Pin: return "\xEF\x82\x8D";       // f08d
        case EditorIcon::Undo: return "\xEF\x83\xA2";      // f0e2
        case EditorIcon::Redo: return "\xEF\x80\x9E";      // f01e
        case EditorIcon::Duplicate: return "\xEF\x83\x85"; // f0c5
        case EditorIcon::Delete: return "\xEF\x8B\xAD";    // f2ed
        case EditorIcon::Reset: return "\xEF\x80\xA1";     // f021
        case EditorIcon::Lock: return "\xEF\x80\xA3";      // f023
        case EditorIcon::Visible: return "\xEF\x81\xAE";   // f06e
        case EditorIcon::More: return "\xEF\x85\x82";      // f142
    }
    return "";
}

}  // namespace px::editor
