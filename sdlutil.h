#pragma once
#include "sdl2/include/SDL.h"
#include "sdl2/include/SDL_ttf.h"
#include <algorithm>
#include <climits>
#include <deque>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

struct file_enum {
	int rel_indent = 0;
	fs::path parent_path;
	fs::path subs_path;
    bool selected = false;
    SDL_Rect rect = {0, 0, 0, 0};
};
static constexpr int    WIN_W = 900;
static constexpr int    WIN_H = 600;
static constexpr int    FONT_SIZE = 18;
static constexpr int    FONTSML_SIZE = 12;
static constexpr int    PG = 16;
static constexpr int    LINE_SPACING = 4;
static constexpr int    CURSOR_WIDTH = 2;
static constexpr int    MAX_UNDO = 500;   // cheap: only diffs stored
static constexpr Uint32 BLINK_MS = 530;

size_t utf8_length(const std::string& s);
std::string utf8_substr(const std::string& s, size_t char_count);
namespace fs = std::filesystem;
namespace utf8 {

    inline int charLen(const std::string& s, int i) {
        if (i < 0 || i >= (int)s.size()) return 0;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) return 1;
        if (c < 0xE0) return 2;
        if (c < 0xF0) return 3;
        return 4;
    }
    inline int next(const std::string& s, int i) { return i + charLen(s, i); }
    inline int prev(const std::string& s, int i) {
        if (i <= 0) return 0;
        int p = i - 1;
        while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) --p;
        return p;
    }
    inline int countChars(const std::string& s, int byteLen) {
        int n = 0, i = 0;
        while (i < byteLen) { i = next(s, i); ++n; }
        return n;
    }
    inline int prevWord(const std::string& s, int i) {
        while (i > 0 && s[utf8::prev(s, i)] == ' ') i = utf8::prev(s, i);
        while (i > 0 && s[utf8::prev(s, i)] != ' ') i = utf8::prev(s, i);
        return i;
    }
    inline int nextWord(const std::string& s, int i) {
        int n = (int)s.size();
        while (i < n && s[i] != ' ') i = utf8::next(s, i);
        while (i < n && s[i] == ' ') i = utf8::next(s, i);
        return i;
    }

} // namespace utf8
// ============================================================
//  TextBuffer
// ============================================================

struct TextBuffer {
    std::vector<std::string> lines;
    TextBuffer() { lines.push_back(""); }

    struct Pos {
        int row = 0, col = 0;
        bool operator==(const Pos& o) const { return row == o.row && col == o.col; }
        bool operator!=(const Pos& o) const { return !(*this == o); }
        bool operator< (const Pos& o) const { return row < o.row || (row == o.row && col < o.col); }
        bool operator<=(const Pos& o) const { return !(o < *this); }
    };

    int numLines()  const { return (int)lines.size(); }
    const std::string& line(int r) const { return lines[r]; }
    int lineLen(int r) const { return (int)lines[r].size(); }

    void clamp(Pos& p) const {
        p.row = std::clamp(p.row, 0, numLines() - 1);
        p.col = std::clamp(p.col, 0, lineLen(p.row));
        while (p.col > 0 && ((unsigned char)lines[p.row][p.col] & 0xC0) == 0x80) --p.col;
    }

    // Insert text at pos; returns position after the inserted text
    Pos insert(Pos pos, const std::string& text) {
        for (char ch : text) {
            if (ch == '\n') {
                std::string rest = lines[pos.row].substr(pos.col);
                lines[pos.row] = lines[pos.row].substr(0, pos.col);
                lines.insert(lines.begin() + pos.row + 1, rest);
                pos.row++; pos.col = 0;
            }
            else {
                lines[pos.row].insert(pos.col, 1, ch);
                pos.col++;
            }
        }
        return pos;
    }

    // Erase [a, b)
    void erase(Pos a, Pos b) {
        if (b <= a) return;
        if (a.row == b.row) {
            lines[a.row].erase(a.col, b.col - a.col);
        }
        else {
            lines[a.row] = lines[a.row].substr(0, a.col) + lines[b.row].substr(b.col);
            lines.erase(lines.begin() + a.row + 1, lines.begin() + b.row + 1);
        }
    }

    // Extract text in [a, b)
    std::string extract(Pos a, Pos b) const {
        if (b <= a) return "";
        std::string out;
        for (int r = a.row; r <= b.row; ++r) {
            int from = (r == a.row) ? a.col : 0;
            int to = (r == b.row) ? b.col : lineLen(r);
            out += lines[r].substr(from, to - from);
            if (r < b.row) out += '\n';
        }
        return out;
    }

    std::string allText() const {
        std::string out;
        for (int i = 0; i < numLines(); ++i) { if (i) out += '\n'; out += lines[i]; }
        return out;
    }
    void setAllText(const std::string& text) {
        lines.clear();
        std::istringstream ss(text); std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
        if (lines.empty()) lines.push_back("");
    }

    // Advance a Pos by walking through a string (to find the end of an insertion)
    static Pos advance(Pos start, const std::string& s) {
        for (char ch : s) {
            if (ch == '\n') { start.row++; start.col = 0; }
            else { start.col++; }
        }
        return start;
    }
};
//  Diff-based Undo/Redo  (Command Pattern)
//
//  EditCommand stores:
//    pos          – start of the affected region (before the edit)
//    deleted      – bytes removed  (empty for pure insert)
//    inserted     – bytes added    (empty for pure delete)
//    cursorBefore – caret restored on undo
//    cursorAfter  – caret restored on redo
//
//  apply()  re-does the command on the buffer (used by redo)
//  revert() un-does the command on the buffer (used by undo)
//
//  Merging rules:
//    • Consecutive single-char InsertText calls → merged into one Insert command
//      (split on newline so Undo works intuitively across line breaks)
//    • Consecutive Backspace calls → merged into one Delete command
//    • Consecutive forward-Delete calls → merged into one Delete command
//    • Replace / paste / cut → never merged
enum class EditKind { Insert, Delete, Replace };

struct EditCommand {

    TextBuffer::Pos pos;            // start of affected range in the buffer
    std::string     deleted;        // text that was removed
    std::string     inserted;       // text that was added
    TextBuffer::Pos cursorBefore;   // caret before this command (undo target)
    TextBuffer::Pos cursorAfter;    // caret after  this command (redo target)
    EditKind        kind;

    // ---- Apply (redo) -----------------------------------------------
    // Remove 'inserted' length bytes then write 'deleted' back  ← wrong direction
    // Correct: erase deleted-region, insert 'inserted'.
    // On first apply the buffer already has the right content; this is called
    // only from redo, so we must replay: erase what was there, put back inserted.
    TextBuffer::Pos apply(TextBuffer& buf) const {
        // The region that 'deleted' once occupied has since been replaced by
        // 'inserted'. To redo: erase 'inserted' then re-insert 'inserted'... that
        // would be a no-op. The correct redo is:
        //   1. erase 'inserted' span starting at pos
        //   2. insert 'deleted'... NO – that is undo.
        // Redo = erase 'deleted' span, then insert 'inserted':
        //   But at redo-time the buffer already has 'deleted' restored (by undo).
        //   So redo = erase 'deleted' span (starting at pos), insert 'inserted'.
        if (!deleted.empty()) {
            TextBuffer::Pos endDel = TextBuffer::advance(pos, deleted);
            buf.erase(pos, endDel);
        }
        TextBuffer::Pos ret = pos;
        if (!inserted.empty()) ret = buf.insert(pos, inserted);
        return ret; // = cursorAfter
    }

    // ---- Revert (undo) ----------------------------------------------
    TextBuffer::Pos revert(TextBuffer& buf) const {
        // undo = erase 'inserted' span, then restore 'deleted'
        if (!inserted.empty()) {
            TextBuffer::Pos endIns = TextBuffer::advance(pos, inserted);
            buf.erase(pos, endIns);
        }
        if (!deleted.empty()) buf.insert(pos, deleted);
        return cursorBefore;
    }

    // ---- Merge check ------------------------------------------------
    // Returns true if `next` can be folded into *this.
    bool canMerge(const EditCommand& next) const {
        if (kind != next.kind)        return false;
        if (kind == EditKind::Replace) return false;

        if (kind == EditKind::Insert) {
            // Only single non-newline chars
            if (next.inserted.size() != 1)    return false;
            if (next.inserted[0] == '\n')      return false;
            // next must start exactly where this one ended
            return next.pos == cursorAfter;
        }

        // Delete (backspace or forward-delete)
        if (next.deleted.size() != 1)  return false;
        if (next.deleted[0] == '\n')   return false;
        // Backspace: pos moves left each keystroke; forward-delete: pos stays
        bool isBackspace = (next.cursorAfter == next.pos);  // cursor == pos means backspace
        bool thisIsBackspace = (cursorAfter == pos);
        if (isBackspace != thisIsBackspace) return false;        // don't mix directions

        if (isBackspace) {
            // next backspace erases the char just before `pos`
            return next.pos == pos || next.cursorAfter < pos;
        }
        else {
            // forward delete: pos stays the same
            return next.pos == pos;
        }
    }

    void mergeWith(const EditCommand& next) {
        if (kind == EditKind::Insert) {
            inserted += next.inserted;
            cursorAfter = next.cursorAfter;
        }
        else {
            bool isBackspace = (cursorAfter == pos);
            if (isBackspace) {
                // prepend newly deleted char (it was to the left)
                deleted = next.deleted + deleted;
                pos = next.pos;
                cursorAfter = next.cursorAfter;
            }
            else {
                // append (forward delete)
                deleted += next.deleted;
                cursorAfter = next.cursorAfter;
            }
        }
    }
};
// ============================================================
//  UndoHistory
// ============================================================
struct UndoHistory {
    std::deque<EditCommand> undoStack;
    std::deque<EditCommand> redoStack;

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    void push(EditCommand cmd) {
        if (!undoStack.empty() && undoStack.back().canMerge(cmd)) {
            undoStack.back().mergeWith(cmd);
        }
        else {
            if ((int)undoStack.size() >= MAX_UNDO) undoStack.pop_front();
            undoStack.push_back(std::move(cmd));
        }
        redoStack.clear();
    }

    TextBuffer::Pos undo(TextBuffer& buf) {
        EditCommand cmd = undoStack.back(); undoStack.pop_back();
        auto pos = cmd.revert(buf);
        redoStack.push_back(std::move(cmd));
        return pos;
    }

    TextBuffer::Pos redo(TextBuffer& buf) {
        EditCommand cmd = redoStack.back(); redoStack.pop_back();
        auto pos = cmd.apply(buf);
        undoStack.push_back(std::move(cmd));
        return pos;
    }

    std::size_t undoCount() const { return undoStack.size(); }
    std::size_t redoCount() const { return redoStack.size(); }
};
//file_system
class Editor {
public:
    bool lim_f = false;
    int limit = 0;
    TextBuffer      buf;
    TextBuffer::Pos cursor;
    TextBuffer::Pos selAnchor;
    bool            hasSelection = false;
    int PADDING = PG;
    // IME
    std::string imeComposing;
    int         imeCursor = 0;

    // Undo
    UndoHistory history;

    // Layout (set by Renderer each frame)
    int scrollRow = 0, scrollX = 0;
    int lineH = 0, viewRows = 0, viewW = 0;

    // txbox-locate
    int TX_X = 10, TX_Y = 20;
    int TX_W = 600, TX_H = 400;

    // Caret blink
    Uint32 lastBlink = 0;
    bool   caretOn = true;

    void init() { cursor = { 0,0 }; selAnchor = { 0,0 }; }

    // ---- Selection ----
    TextBuffer::Pos selMin() const { return cursor < selAnchor ? cursor : selAnchor; }
    TextBuffer::Pos selMax() const { return cursor < selAnchor ? selAnchor : cursor; }
    void clearSelection() { hasSelection = false; selAnchor = cursor; }
    void startSelection() { if (!hasSelection) { hasSelection = true; selAnchor = cursor; } }
    void selectAll() {
        hasSelection = true; selAnchor = { 0,0 };
        cursor = { buf.numLines() - 1, buf.lineLen(buf.numLines() - 1) };
    }
    std::string selectedText() const {
        return hasSelection ? buf.extract(selMin(), selMax()) : "";
    }

    // ---- Text insertion (records Insert or Replace command) ----
    void insertText(const std::string& text) {
        if (utf8_length(buf.line(cursor.row)) + utf8_length(text) > limit && lim_f) {
            return;
        }
        EditCommand cmd;
        cmd.cursorBefore = cursor;

        if (hasSelection) {
            cmd.kind = EditKind::Replace;
            cmd.pos = selMin();
            cmd.deleted = buf.extract(selMin(), selMax());
            buf.erase(selMin(), selMax());
            cursor = selMin();
            clearSelection();
        }
        else {
            cmd.kind = EditKind::Insert;
            cmd.pos = cursor;
        }

        cmd.inserted = text;
        cursor = buf.insert(cursor, text);
        cmd.cursorAfter = cursor;
        clearSelection();
        history.push(cmd);
        ensureCursorVisible();
    }

    void insertNewline() { insertText("\n"); }

    // ---- Backspace (records Delete command) ----
    void backspace() {
        if (hasSelection) { deleteSelection(); return; }
        if (cursor.col == 0 && cursor.row == 0) return;

        TextBuffer::Pos from = cursor;
        if (cursor.col == 0) {
            from = { cursor.row - 1, buf.lineLen(cursor.row - 1) };
        }
        else {
            from.col = utf8::prev(buf.line(cursor.row), cursor.col);
        }

        EditCommand cmd;
        cmd.kind = EditKind::Delete;
        cmd.pos = from;
        cmd.deleted = buf.extract(from, cursor);
        cmd.cursorBefore = cursor;
        cmd.cursorAfter = from;   // cursor == pos ⟹ backspace flag for merging
        buf.erase(from, cursor);
        cursor = from;
        clearSelection();
        history.push(cmd);
        ensureCursorVisible();
    }

    // ---- Forward delete (records Delete command) ----
    void deleteForward() {
        if (hasSelection) { deleteSelection(); return; }
        TextBuffer::Pos to = cursor;
        if (to.col < buf.lineLen(to.row))
            to.col = utf8::next(buf.line(to.row), to.col);
        else if (to.row < buf.numLines() - 1)
            to = { cursor.row + 1, 0 };
        else return;

        EditCommand cmd;
        cmd.kind = EditKind::Delete;
        cmd.pos = cursor;
        cmd.deleted = buf.extract(cursor, to);
        cmd.cursorBefore = cursor;
        cmd.cursorAfter = cursor;  // cursor stays ⟹ forward-delete flag
        buf.erase(cursor, to);
        clearSelection();
        history.push(cmd);
        ensureCursorVisible();
    }

    // ---- Delete selection (records Replace command with empty insert) ----
    void deleteSelection() {
        if (!hasSelection) return;
        EditCommand cmd;
        cmd.kind = EditKind::Replace;
        cmd.pos = selMin();
        cmd.deleted = buf.extract(selMin(), selMax());
        cmd.inserted = "";
        cmd.cursorBefore = cursor;
        cmd.cursorAfter = selMin();
        buf.erase(selMin(), selMax());
        cursor = selMin();
        clearSelection();
        history.push(cmd);
        ensureCursorVisible();
    }

    // ---- Undo / Redo ----
    void doUndo() {
        if (!history.canUndo()) return;
        cursor = history.undo(buf);
        clearSelection(); buf.clamp(cursor);
        ensureCursorVisible();
    }
    void doRedo() {
        if (!history.canRedo()) return;
        cursor = history.redo(buf);
        clearSelection(); buf.clamp(cursor);
        ensureCursorVisible();
    }

    // ---- Cursor movement ----
    void moveCursor(TextBuffer::Pos p, bool select) {
        if (select) startSelection(); else clearSelection();
        cursor = p; buf.clamp(cursor); ensureCursorVisible();
    }
    void moveLeft(bool sel, bool word) {
        TextBuffer::Pos p = cursor;
        if (!sel && hasSelection) { p = selMin(); clearSelection(); cursor = p; ensureCursorVisible(); return; }
        if (word) {
            if (p.col > 0) p.col = utf8::prevWord(buf.line(p.row), p.col);
            else if (p.row > 0) { p.row--; p.col = buf.lineLen(p.row); }
        }
        else {
            if (p.col > 0) p.col = utf8::prev(buf.line(p.row), p.col);
            else if (p.row > 0) { p.row--; p.col = buf.lineLen(p.row); }
        }
        moveCursor(p, sel);
    }
    void moveRight(bool sel, bool word) {
        TextBuffer::Pos p = cursor;
        if (!sel && hasSelection) { p = selMax(); clearSelection(); cursor = p; ensureCursorVisible(); return; }
        if (word) {
            if (p.col < buf.lineLen(p.row)) p.col = utf8::nextWord(buf.line(p.row), p.col);
            else if (p.row < buf.numLines() - 1) { p.row++; p.col = 0; }
        }
        else {
            if (p.col < buf.lineLen(p.row)) p.col = utf8::next(buf.line(p.row), p.col);
            else if (p.row < buf.numLines() - 1) { p.row++; p.col = 0; }
        }
        moveCursor(p, sel);
    }
    void moveUp(bool sel) {
        if (cursor.row == 0) { moveCursor({ 0,0 }, sel); return; }
        moveCursor({ cursor.row - 1, cursor.col }, sel);
    }
    void moveDown(bool sel) {
        if (cursor.row == buf.numLines() - 1) { moveCursor({ cursor.row, buf.lineLen(cursor.row) }, sel); return; }
        moveCursor({ cursor.row + 1, cursor.col }, sel);
    }
    void moveHome(bool sel, bool ctrl) {
        moveCursor(ctrl ? TextBuffer::Pos{ 0,0 } : TextBuffer::Pos{ cursor.row,0 }, sel);
    }
    void moveEnd(bool sel, bool ctrl) {
        int lr = ctrl ? buf.numLines() - 1 : cursor.row;
        moveCursor({ lr, buf.lineLen(lr) }, sel);
    }

    // ---- Clipboard ----
    void copy() { if (hasSelection) SDL_SetClipboardText(selectedText().c_str()); }
    void cut() { if (!hasSelection) return; copy(); deleteSelection(); }
    void paste() {
        if (!SDL_HasClipboardText()) return;
        char* txt = SDL_GetClipboardText();
        if (txt) {
            std::string s(txt);
            SDL_free(txt);

            // ★ CR を除去（Windows の CRLF 対策）
            s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());

            insertText(s);
        }
    }


    // ---- Scroll ----
    void ensureCursorVisible() {
        if (lineH <= 0 || viewRows <= 0) return;
        if (cursor.row < scrollRow) scrollRow = cursor.row;
        if (cursor.row >= scrollRow + viewRows) scrollRow = cursor.row - viewRows + 1;
    }

    // ---- Mouse hit-test ----
    TextBuffer::Pos hitTest(int px, int py, TTF_Font* font) const {
        px -= TX_X + PADDING + 35;
        py -= TX_Y;
        int row = scrollRow + (py - PADDING) / lineH;
        row = std::clamp(row, 0, buf.numLines() - 1);
        const std::string& ln = buf.line(row);
        if (ln.empty()) return { row, 0 };
        int best = 0, bestDist = INT_MAX, i = 0;
        while (true) {
            int w = 0, h = 0; TTF_SizeUTF8(font, ln.substr(0, i).c_str(), &w, &h);
            int d = std::abs(w - (px - PADDING + scrollX));
            if (d < bestDist) { bestDist = d; best = i; }
            if (i >= (int)ln.size()) break;
            i = utf8::next(ln, i);
        }
        return { row, best };
    }

    // ---- Caret blink ----
    void tickBlink() {
        Uint32 now = SDL_GetTicks();
        if (now - lastBlink > BLINK_MS) { caretOn = !caretOn; lastBlink = now; }
    }
    void resetBlink() { caretOn = true; lastBlink = SDL_GetTicks(); }
};

class file_explorer {
    public:
    bool ex_file = true;
    int scrollRow = 0;
    int F_X = 0,F_Y = 0,F_W = 0,F_H = 0;
    int PADDING = PG;
    fs::path p = fs::current_path();
    std::vector<file_enum> file_lists;
	Editor ed;
    void init(int lineH){
        ed.cursor = { 0,0 };
        ed.lineH = lineH;
        ed.init();
        ed.buf.setAllText(p.string());
        ed.lim_f = false;
        ed.cursor = { 0,0 };
	    ed.TX_X = F_X; ed.TX_Y = F_Y; ed.TX_W = F_W; ed.TX_H = 21;
	    ed.PADDING = 5;
        now_list();
        file_sort();
    }
    void list_dir(const fs::path& path,int indent,std::vector<file_enum>& file_list) {
        try {
            for (const auto& entry : fs::directory_iterator(path)) {
                file_enum fe;
                fe.rel_indent = indent;
                fe.parent_path = path;
                fe.subs_path = entry.path();
                file_list.push_back(fe);
                if (fs::is_directory(entry.path()) && !fs::is_symlink(entry.path())) {
                    list_dir(entry.path(), indent + 1, file_list);
                }
            }
        }
        catch (const fs::filesystem_error& e) {
            std::cerr << "Error accessing directory: " << e.what() << std::endl;
        }
    }
    void path_set(bool path_use,fs::path t_p){
        if(!path_use){
            const std::string& ln = ed.buf.line(0);
            fs::path temp_path = (ln);
            if(fs::exists(temp_path)){
                ex_file = true;
                temp_path = fs::absolute(temp_path);
                p = temp_path;
                now_list();
                file_sort();
            }else{
                ex_file = false;
            }
            ed.buf.setAllText(p.string());
            ed.cursor = { 0,0 };
        }else{
            p = t_p;
            ed.buf.setAllText(t_p.string());
            now_list();
            file_sort();
        }
	}
    void now_list(){
        try{
            file_lists.clear();
            for(const auto& entry : fs::directory_iterator(p)){
                file_enum fil;
                fil.rel_indent = 0;
                fil.parent_path = p;
                fil.subs_path = entry.path();
                file_lists.push_back(fil);
            }
        }catch(const fs::filesystem_error& e){
            file_lists[0].subs_path = "Error accessing directory:";
            std::cerr << "Error accessing directory: " << e.what() << std::endl;
        }
    }
    void file_sort(){
        std::sort(file_lists.begin(), file_lists.end(), [](const file_enum& a, const file_enum& b) {
            if (fs::is_directory(a.subs_path) && !fs::is_directory(b.subs_path)) return true;
            if (!fs::is_directory(a.subs_path) && fs::is_directory(b.subs_path)) return false;
            return a.subs_path.filename() < b.subs_path.filename();
        });
	}
    bool hitscan(SDL_Point mousePos,bool click,bool doubleclick) {
		if (file_lists.empty()) return false;
        SDL_Rect hit = {F_X,F_Y,F_W,F_H};
        bool before = false;
        for (size_t i = 0; i < file_lists.size(); ++i) {
            hit = { F_X + PADDING,int(F_Y + i * 20 - scrollRow * 20 + ed.TX_H),F_W - PADDING,20 };
            file_lists[i].rect = hit;
			before = file_lists[i].selected;
            if(doubleclick && SDL_PointInRect(&mousePos, &hit)){
                path_set(true,file_lists[i].subs_path);
                return true;
            }
            if(click){
                file_lists[i].selected = SDL_PointInRect(&mousePos, &hit);
            }
            else if(before){
                file_lists[i].selected = true;
			}else {
                file_lists[i].selected = false;
            }
        }
        return false;
    }
    void back_path(){
        fs::path temp = ed.buf.line(0);
        ed.buf.setAllText(temp.parent_path().string());
        path_set(false,"");
    }
};

class Renderer {
    SDL_Window* win = nullptr;
    TTF_Font* font = nullptr;
	TTF_Font* font_sml = nullptr;

    SDL_Color colBg = { 30,  30,  35,  255 };
    SDL_Color colText = { 220, 220, 210, 255 };
    SDL_Color colSel = { 60,  100, 170, 180 };
    SDL_Color colCaret = { 230, 200, 100, 255 };
    SDL_Color colLineno = { 80,  80,  100, 255 };
    SDL_Color colIme = { 100, 200, 255, 255 };
    SDL_Color colImeBg = { 50,  60,  80,  200 };

    int linenoW = 50;

public:
    SDL_Renderer* ren = nullptr;
    int lineH = 0;
    int PADDING = PG;
    bool init(const char* fontPath) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
        if (TTF_Init() < 0) return false;
        
        win = SDL_CreateWindow("SDL2",SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
        if (!win) return false;
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) return false;

        SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);

        font = TTF_OpenFont(fontPath, FONT_SIZE);
        font_sml = TTF_OpenFont(fontPath, FONT_SIZE);
        if (!font) {
            const char* fb[] = {
                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                "/System/Library/Fonts/Menlo.ttc",
                "/System/Library/Fonts/Monaco.ttf",
                //"fonts\\misaki_gothic_2nd.ttf",
                "fonts\\PixelMplus10-Bold.ttf",
                "C:\\Windows\\Fonts\\consola.ttf",
                nullptr
            };
            for (int i = 0; fb[i] && !font; ++i) font = TTF_OpenFont(fb[i], FONT_SIZE);
            for (int i = 0; fb[i] && !font_sml; ++i) font_sml = TTF_OpenFont(fb[i], FONTSML_SIZE);
        }
        if (!font) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Font: %s", TTF_GetError());
            return false;
        }
        int tw, th; TTF_SizeUTF8(font, "M", &tw, &th);
        lineH = th + LINE_SPACING;
        SDL_StartTextInput();
        return true;
    }

    void destroy() {
        if (font) TTF_CloseFont(font);
        if (ren)  SDL_DestroyRenderer(ren);
        if (win)  SDL_DestroyWindow(win);
        TTF_Quit(); SDL_Quit();
    }

    TTF_Font* getFont() { return font; }
    TTF_Font* getFont_sml() { return font_sml; }

    int textWidth(const std::string& s) {
        if (s.empty()) return 0;
        int w = 0, h = 0; TTF_SizeUTF8(font, s.c_str(), &w, &h); return w;
    }
    int smltextWidth(const std::string& s) {
        if (s.empty()) return 0;
        int w = 0, h = 0; TTF_SizeUTF8(font_sml, s.c_str(), &w, &h); return w;
    }
    void drawText(const std::string& s, int x, int y, SDL_Color col) {
        if (s.empty()) return;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), col);
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
        SDL_FreeSurface(surf);
        if (!tex) return;
        int w, h; SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
        SDL_Rect dst = { x,y,w,h };
        SDL_RenderCopy(ren, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    void drawsmlText(const std::string& s, int x, int y, SDL_Color col) {
        if (s.empty()) return;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font_sml, s.c_str(), col);
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
        SDL_FreeSurface(surf);
        if (!tex) return;
        int w, h; SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
        SDL_Rect dst = { x,y,w,h };
        SDL_RenderCopy(ren, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    void adjustHorizontalScroll(Editor& ed) {
        const std::string& ln = ed.buf.line(ed.cursor.row);
        int cx = smltextWidth(ln.substr(0, ed.cursor.col));

        int left = ed.scrollX;
        int right = ed.scrollX + ed.viewW;

        if (cx < left) {
            ed.scrollX = cx;
        }
        else if (cx > right - 50) {
            ed.scrollX = cx - ed.viewW + 50;
        }

        if (ed.scrollX < 0) ed.scrollX = 0;
    }
    void adjustHorizontalScroll_sh(Editor& ed) {
        const std::string& ln = ed.buf.line(ed.cursor.row);
        int cx = textWidth(ln.substr(0, ed.cursor.col));

        int left = ed.scrollX;
        int right = ed.scrollX + ed.viewW;

        if (cx < left) {
            ed.scrollX = cx;
        }
        else if (cx > right - 2) {
            ed.scrollX = cx - ed.viewW + 2;
        }

        if (ed.scrollX < 0) ed.scrollX = 0;
    }
    void cls(int r, int g, int b, int a) {
        SDL_SetRenderDrawColor(ren, r, g, b, a); // R,G,B,A（ここでは青）
        SDL_RenderClear(ren);                       // 背景を青で塗る
    }
    void draw_bg(SDL_Color back){
        cls(0,0,0,255);
        SDL_SetRenderDrawColor(ren,back.r,back.g,back.b,back.a);
        SDL_Rect r = {0,0,WIN_W,WIN_H};
        SDL_RenderFillRect(ren,&r);
    }
    void rend() {
        SDL_RenderPresent(ren);
    }
    void btn_draw(SDL_Rect r,SDL_Point p,std::string text) {
        if (SDL_PointInRect(&p, &r)) {
			SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
        }
        else {
            SDL_SetRenderDrawColor(ren, 250, 250, 250, 255);
        }
		SDL_RenderFillRect(ren, &r);
        if (!text.empty()) {
            int tw = smltextWidth(text);
            int th = lineH - 10;
            int tx = r.x + (r.w - tw) / 2;
            int ty = r.y + (r.h - th) / 2;
            drawsmlText(text, tx, ty, {0, 0, 0, 255});
        }
    }
    void TextBox(Editor& ed) {
        int winW, winH; SDL_GetWindowSize(win, &winW, &winH);
        ed.lineH = lineH;
        ed.viewRows = (ed.TX_H - PADDING * 2) / lineH;
        ed.viewW = ed.TX_W - linenoW - PADDING * 2;
        SDL_Rect bgrect = { ed.TX_X, ed.TX_Y, ed.TX_W, ed.TX_H };
        SDL_SetRenderDrawColor(ren, colBg.r, colBg.g, colBg.b, 255);
        SDL_RenderFillRect(ren, &bgrect);

        SDL_Rect clip = { linenoW + PADDING + ed.TX_X, PADDING + ed.TX_Y, ed.TX_W - (linenoW + PADDING), (ed.TX_Y + ed.TX_H) - PADDING * 2 };
        SDL_RenderSetClipRect(ren, &clip);

        int x0 = linenoW + PADDING - ed.scrollX;
        int last = std::min(ed.scrollRow + ed.viewRows + 1, ed.buf.numLines());

        for (int row = ed.scrollRow; row < last; ++row) {
            int y = PADDING + (row - ed.scrollRow) * lineH;
            const std::string& ln = ed.buf.line(row);

            // Selection
            if (ed.hasSelection) {
                auto a = ed.selMin(), b = ed.selMax();
                if (row >= a.row && row <= b.row) {
                    int xf = (row == a.row) ? textWidth(ln.substr(0, a.col)) : 0;
                    int xt = (row == b.row) ? textWidth(ln.substr(0, b.col)) : textWidth(ln) + 8;
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, colSel.r, colSel.g, colSel.b, colSel.a);
                    SDL_Rect sr = { x0 + xf + ed.TX_X,y + ed.TX_Y,xt - xf,lineH };
                    SDL_RenderFillRect(ren, &sr);
                }
            }

            if (!ln.empty()) drawText(ln, x0 + ed.TX_X, y + ed.TX_Y, colText);

            // IME preedit
            if (!ed.imeComposing.empty() && row == ed.cursor.row) {
                int cx = textWidth(ln.substr(0, ed.cursor.col));
                int pw = textWidth(ed.imeComposing);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, colImeBg.r, colImeBg.g, colImeBg.b, colImeBg.a);
                SDL_Rect pr = { x0 + cx + ed.TX_X,y + ed.TX_Y,pw,lineH }; SDL_RenderFillRect(ren, &pr);
                SDL_SetRenderDrawColor(ren, colIme.r, colIme.g, colIme.b, 220);
                //アンダーライン
                SDL_RenderDrawLine(ren, x0 + cx + ed.TX_X, y + lineH - 2 + ed.TX_Y, x0 + cx + pw + ed.TX_X, y + lineH - 2 + ed.TX_Y);
                drawText(ed.imeComposing, x0 + cx + ed.TX_X, y + ed.TX_Y, colIme);
                //int icx=cx+textWidth(ed.imeComposing.substr(0,ed.imeCursor));
                int icx = cx + textWidth(utf8_substr(ed.imeComposing, ed.imeCursor));
                SDL_SetRenderDrawColor(ren, colIme.r, colIme.g, colIme.b, 255);
                SDL_Rect cr = { x0 + icx + ed.TX_X,y + ed.TX_Y,CURSOR_WIDTH,lineH }; SDL_RenderFillRect(ren, &cr);
            }

            // Caret
            if (ed.caretOn && row == ed.cursor.row && ed.imeComposing.empty()) {
                int cx = textWidth(ln.substr(0, ed.cursor.col));
                SDL_SetRenderDrawColor(ren, colCaret.r, colCaret.g, colCaret.b, 255);
                SDL_Rect cr = { x0 + cx + ed.TX_X,y + ed.TX_Y,CURSOR_WIDTH,lineH }; SDL_RenderFillRect(ren, &cr);
            }
        }

        SDL_RenderSetClipRect(ren, nullptr);

        // Line numbers
        SDL_SetRenderDrawColor(ren, 40, 40, 48, 255);
        SDL_Rect lnbg = { ed.TX_X, ed.TX_Y, 50, ed.TX_H }; SDL_RenderFillRect(ren, &lnbg);
        for (int r = ed.scrollRow; r < last; ++r)
            drawText(std::to_string(r + 1), ed.TX_X + 5 + PADDING, ed.TX_Y + (r - ed.scrollRow) * lineH + PADDING, colLineno);

        // Status bar
        SDL_SetRenderDrawColor(ren, 20, 20, 25, 255);
        SDL_Rect sb = { ed.TX_X,ed.TX_Y + ed.TX_H - 24,ed.TX_W, 24 }; SDL_RenderFillRect(ren, &sb);
        std::string status =
            "Ln " + std::to_string(ed.cursor.row + 1) +
            " Col " + std::to_string(utf8::countChars(ed.buf.line(ed.cursor.row), ed.cursor.col) + 1) +
            " Undo:" + std::to_string(ed.history.undoCount()) +
            " Redo:" + std::to_string(ed.history.redoCount());
        drawText(status, PADDING + ed.TX_X, ed.TX_Y + ed.TX_H - PADDING - 4, colLineno);
        adjustHorizontalScroll(ed);
    }
    void TextBoxsh(Editor& ed) {
        int winW, winH; SDL_GetWindowSize(win, &winW, &winH);
        ed.lineH = lineH;
        ed.viewRows = (ed.TX_H - ed.PADDING * 2) / lineH;
        ed.viewW = ed.TX_W - linenoW - ed.PADDING * 2;
        SDL_Rect bgrect = { ed.TX_X, ed.TX_Y, ed.TX_W, ed.TX_H };
        SDL_SetRenderDrawColor(ren, colBg.r, colBg.g, colBg.b, 255);
        SDL_RenderFillRect(ren, &bgrect);

        SDL_Rect clip = { linenoW + ed.PADDING + ed.TX_X, ed.PADDING + ed.TX_Y, ed.TX_W - (linenoW + ed.PADDING), (ed.TX_Y + ed.TX_H) - ed.PADDING * 2 };
        SDL_RenderSetClipRect(ren, &clip);

        int x0 = linenoW + ed.PADDING - ed.scrollX;
        int last = std::min(ed.scrollRow + ed.viewRows + 1, ed.buf.numLines());

        for (int row = ed.scrollRow; row < last; ++row) {
            int y = ed.PADDING + (row - ed.scrollRow) * lineH;
            const std::string& ln = ed.buf.line(row);
            int sp = FONTSML_SIZE;
            // Selection
            if (ed.hasSelection) {
                auto a = ed.selMin(), b = ed.selMax();
                if (row >= a.row && row <= b.row) {
                    int xf = (row == a.row) ? smltextWidth(ln.substr(0, a.col)) : 0;
                    int xt = (row == b.row) ? smltextWidth(ln.substr(0, b.col)) : smltextWidth(ln) + 8;
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, colSel.r, colSel.g, colSel.b, colSel.a);
                    SDL_Rect sr = { x0 + xf + ed.TX_X,y + ed.TX_Y,xt - xf,sp };
                    SDL_RenderFillRect(ren, &sr);
                }
            }

            if (!ln.empty()) drawsmlText(ln, x0 + ed.TX_X, y + ed.TX_Y, colText);

            // IME preedit
            if (!ed.imeComposing.empty() && row == ed.cursor.row) {
                int cx = smltextWidth(ln.substr(0, ed.cursor.col));
                int pw = smltextWidth(ed.imeComposing);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, colImeBg.r, colImeBg.g, colImeBg.b, colImeBg.a);
                SDL_Rect pr = { x0 + cx + ed.TX_X,y + ed.TX_Y,pw,FONTSML_SIZE }; SDL_RenderFillRect(ren, &pr);
                SDL_SetRenderDrawColor(ren, colIme.r, colIme.g, colIme.b, 220);
                //アンダーライン
                
                SDL_RenderDrawLine(ren, x0 + cx + ed.TX_X, y + ed.TX_Y + sp, x0 + cx + pw + ed.TX_X, y + ed.TX_Y + sp);
                drawsmlText(ed.imeComposing, x0 + cx + ed.TX_X, y + ed.TX_Y, colIme);
                int icx = cx + smltextWidth(utf8_substr(ed.imeComposing, ed.imeCursor));
                SDL_SetRenderDrawColor(ren, colIme.r, colIme.g, colIme.b, 255);
                SDL_Rect cr = { x0 + icx + ed.TX_X,y + ed.TX_Y,CURSOR_WIDTH,FONTSML_SIZE }; SDL_RenderFillRect(ren, &cr);
            }

            // Caret
            if (ed.caretOn && row == ed.cursor.row && ed.imeComposing.empty()) {
                int cx = smltextWidth(ln.substr(0, ed.cursor.col));
                SDL_SetRenderDrawColor(ren, colCaret.r, colCaret.g, colCaret.b, 255);
                SDL_Rect cr = { x0 + cx + ed.TX_X,y + ed.TX_Y,CURSOR_WIDTH,FONTSML_SIZE }; SDL_RenderFillRect(ren, &cr);
            }
        }
        SDL_RenderSetClipRect(ren, nullptr);
        adjustHorizontalScroll_sh(ed);
    }
    void drawFilledTriangle(SDL_Point p1, SDL_Point p2, SDL_Point p3)
    {
        // y 座標でソート
        if (p2.y < p1.y) { SDL_Point tmp = p1; p1 = p2; p2 = tmp; }
        if (p3.y < p1.y) { SDL_Point tmp = p1; p1 = p3; p3 = tmp; }
        if (p3.y < p2.y) { SDL_Point tmp = p2; p2 = p3; p3 = tmp; }

        float dx1 = 0, dx2 = 0, dx3 = 0;

        if (p2.y - p1.y != 0) dx1 = (float)(p2.x - p1.x) / (p2.y - p1.y);
        if (p3.y - p1.y != 0) dx2 = (float)(p3.x - p1.x) / (p3.y - p1.y);
        if (p3.y - p2.y != 0) dx3 = (float)(p3.x - p2.x) / (p3.y - p2.y);

        float sx = p1.x;
        float ex = p1.x;

        // 上半分
        for (int y = p1.y; y <= p2.y; y++) {
            SDL_RenderDrawLine(ren, (int)sx, y, (int)ex, y);
            sx += dx1;
            ex += dx2;
        }

        sx = p2.x;

        // 下半分
        for (int y = p2.y; y <= p3.y; y++) {
            SDL_RenderDrawLine(ren, (int)sx, y, (int)ex, y);
            sx += dx3;
            ex += dx2;
        }
    }
    void dir_icon(int x, int y, float size) {
        SDL_SetRenderDrawColor(ren, 255, 200, 82, 255);
        SDL_Rect r = { x,y + 3 * size,17 * size,12 * size };
        SDL_RenderFillRect(ren, &r);
        r = { x,y, int(5 * size) , int(3 * size) };
        SDL_RenderFillRect(ren, &r);
    }
    void file_icon(int x, int y, float size) {
        SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
        SDL_Rect r = { x,y,12 * size,15 * size };
        r = {x,y + int(4 * size),int(12 * size),int(15 * size - 4 * size)};
        SDL_RenderFillRect(ren, &r);
        r = { x,y,int(12 * size - 4 * size),int(15 * size )};
        SDL_RenderFillRect(ren, &r);
		drawFilledTriangle({ int(x + 12 * size - 4 * size),y }, { int(x + 12 * size - 4 * size),int(y + 4 * size )}, {int( x + 11 * size),int(y + 4 * size )});
		SDL_SetRenderDrawColor(ren, 5, 5, 5, 5);
		SDL_RenderDrawLine(ren, x + 12 * size - 4 * size, y, x + 12 * size - 4 * size, y + 4 * size);
		SDL_RenderDrawLine(ren, x + 12 * size - 4 * size, y + 4 * size, x + 11 * size, y + 4 * size);
	}
    void update_fs_explorer(file_explorer& fi_ex) {
        std::vector<file_enum> p = fi_ex.file_lists;
        SDL_Rect r = {fi_ex.F_X,fi_ex.F_Y,fi_ex.F_W,fi_ex.F_H};
        SDL_Rect clip = {fi_ex.F_X,fi_ex.F_Y + fi_ex.ed.TX_H ,fi_ex.F_W,fi_ex.F_H - fi_ex.ed.TX_H};
        SDL_Color colFsBg = { 200, 200, 200, 255 };
		SDL_SetRenderDrawColor(ren, colFsBg.r, colFsBg.g, colFsBg.b, 255);
        SDL_RenderFillRect(ren, &r);
        TextBoxsh(fi_ex.ed);
        drawText("<-",fi_ex.F_X,fi_ex.F_Y,{220,220,220,255});
		SDL_RenderSetClipRect(ren, &clip);
        for (size_t i = 0;i < fi_ex.file_lists.size(); i++){
            
            if(fi_ex.file_lists[i].selected) {
			    SDL_SetRenderDrawColor(ren, 71, 190, 255, 100);
			    SDL_RenderFillRect(ren, &fi_ex.file_lists[i].rect);
            }
            
            drawText(fi_ex.file_lists[i].subs_path.filename().string(),r.x + PADDING + 20,r.y + PADDING + 5 + i * 20 - fi_ex.scrollRow * 20, {30,30,30,255});
            if(fs::is_directory(fi_ex.file_lists[i].subs_path)){
                dir_icon(r.x + PADDING,r.y + PADDING + i * 20 - fi_ex.scrollRow * 20 + 7,1);
            }else{
                file_icon(r.x + PADDING,r.y + PADDING + i * 20 - fi_ex.scrollRow * 20 + 8,1);
            }
        }
        SDL_RenderSetClipRect(ren, nullptr);
    }

};

void textEditEvent(SDL_Event& e, Editor& ed, Renderer& renderer, bool& mouseDown, int mox, int moy);
void textEditEvent_sh(SDL_Event& e, Editor& ed, Renderer& renderer, bool& mouseDown, int mox, int moy);
void file_explorer_event(SDL_Event& e,file_explorer& fi);
