#include "sdl2/include/SDL.h"
#include "sdl2/include/SDL_ttf.h"
#include "sdlutil.h"
#include <algorithm>
#include <climits>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

size_t utf8_length(const std::string& s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = s[i];
        size_t char_len;

        if (c < 0x80) char_len = 1;                 // 1 byte (ASCII)
        else if ((c >> 5) == 0x6) char_len = 2;     // 110xxxxx
        else if ((c >> 4) == 0xE) char_len = 3;     // 1110xxxx
        else if ((c >> 3) == 0x1E) char_len = 4;    // 11110xxx
        else throw std::runtime_error("Invalid UTF-8");

        i += char_len;
        count++;
    }
    return count;
}

std::string utf8_substr(const std::string& s, size_t char_count) {
    size_t i = 0;
    size_t chars = 0;

    while (i < s.size() && chars < char_count) {
        unsigned char c = s[i];
        size_t char_len = 1;

        if ((c & 0x80) == 0x00) char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;

        i += char_len;
        chars++;
    }
    return s.substr(0, i);
}

void textEditEvent(SDL_Event& e, Editor& ed, Renderer& renderer, bool& mouseDown, int mox, int moy)
{
    bool select = ed.TX_X < mox && mox < ed.TX_X + ed.TX_W && ed.TX_Y < moy && moy < ed.TX_Y + ed.TX_H;
    int nm_x = e.button.x;
    int nm_y = e.button.y;
    bool hv = false;
    switch (e.type) {
    case SDL_KEYDOWN: {
        if (!select) {
            break;
        }
        int key = e.key.keysym.sym, mod = e.key.keysym.mod;
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool shift = (mod & KMOD_SHIFT) != 0;
        ed.resetBlink();

        if (ctrl) {
            switch (key) {
            case SDLK_a: ed.selectAll(); break;
            case SDLK_c: ed.copy(); break;
            case SDLK_x: ed.cut(); break;
            case SDLK_v: ed.paste(); break;
            case SDLK_z: shift ? ed.doRedo() : ed.doUndo(); break;
            case SDLK_y: ed.doRedo(); break;
            case SDLK_LEFT:  ed.moveLeft(shift, true); break;
            case SDLK_RIGHT: ed.moveRight(shift, true); break;
            case SDLK_HOME:  ed.moveHome(shift, true); break;
            case SDLK_END:   ed.moveEnd(shift, true); break;
            }
        }
        else {
            switch (key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (ed.imeComposing.empty()) ed.insertNewline();
                break;
            case SDLK_BACKSPACE:
                if (ed.imeComposing.empty()) ed.backspace();
                break;
            case SDLK_DELETE:
                if (ed.imeComposing.empty()) ed.deleteForward();
                break;
            case SDLK_LEFT:  ed.moveLeft(shift, false); break;
            case SDLK_RIGHT: ed.moveRight(shift, false); break;
            case SDLK_UP:    ed.moveUp(shift); break;
            case SDLK_DOWN:  ed.moveDown(shift); break;
            case SDLK_HOME:  ed.moveHome(shift, false); break;
            case SDLK_END:   ed.moveEnd(shift, false); break;
            }
        }
        break;
    }
    case SDL_TEXTEDITING:
        if (!select) {
            break;
        }
        ed.imeComposing = e.edit.text;
        ed.imeCursor = e.edit.start;
        break;
    case SDL_TEXTINPUT:
        if (!select) {
            break;
        }
        ed.imeComposing.clear();
        ed.imeCursor = 0;
        ed.insertText(e.text.text);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (!select) {
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            auto p = ed.hitTest(e.button.x, e.button.y, renderer.getFont());
            if (e.button.clicks == 2) {
                ed.cursor = p;
                ed.hasSelection = true;
                ed.selAnchor = p;
                ed.selAnchor.col = utf8::prevWord(ed.buf.line(p.row), p.col);
                ed.cursor.col = utf8::nextWord(ed.buf.line(p.row), p.col);
            }
            else {
                bool sh = (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (sh) ed.startSelection(); else ed.clearSelection();
                ed.cursor = p;
                if (!sh) ed.selAnchor = p;
                ed.resetBlink();
            }
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (!select) {
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT)
            mouseDown = false;
        break;

    case SDL_MOUSEMOTION:
        if (!select) {
            break;
        }
        if (mouseDown) {
            ed.hasSelection = true;
            ed.cursor = ed.hitTest(e.motion.x, e.motion.y, renderer.getFont());
        }
        break;

    case SDL_MOUSEWHEEL:
        if (!select) {
            break;
        }
        ed.scrollRow = std::clamp(ed.scrollRow - e.wheel.y, 0, ed.buf.numLines() - 1);
        break;

    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_RESIZED)
            ed.viewRows = (e.window.data2 - ed.PADDING * 2) / renderer.lineH;
        break;
    }
    if (select) {
        ed.tickBlink();
    }
}
void textEditEvent_sh(SDL_Event& e, limit_Editor& ed, Renderer& renderer, bool& mouseDown, int mox, int moy)
{
    bool select = ed.TX_X < mox && mox < ed.TX_X + ed.TX_W && ed.TX_Y < moy && moy < ed.TX_Y + ed.TX_H;
    int nm_x = e.button.x;
    int nm_y = e.button.y;
    bool hv = false;

    switch (e.type) {
    case SDL_KEYDOWN: {
        if (!select) {
            break;
        }
        int key = e.key.keysym.sym, mod = e.key.keysym.mod;
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool shift = (mod & KMOD_SHIFT) != 0;
        ed.resetBlink();

        if (ctrl) {
            switch (key) {
            case SDLK_a: ed.selectAll(); break;
            case SDLK_c: ed.copy(); break;
            case SDLK_x: ed.cut(); break;
            case SDLK_v: ed.paste(); break;
            case SDLK_z: shift ? ed.doRedo() : ed.doUndo(); break;
            case SDLK_y: ed.doRedo(); break;
            case SDLK_LEFT:  ed.moveLeft(shift, true); break;
            case SDLK_RIGHT: ed.moveRight(shift, true); break;
            case SDLK_HOME:  ed.moveHome(shift, true); break;
            case SDLK_END:   ed.moveEnd(shift, true); break;
            }
        }
        else {
            switch (key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                break;
            case SDLK_BACKSPACE:
                if (ed.imeComposing.empty()) ed.backspace();
                break;
            case SDLK_DELETE:
                if (ed.imeComposing.empty()) ed.deleteForward();
                break;
            case SDLK_LEFT:  ed.moveLeft(shift, false); break;
            case SDLK_RIGHT: ed.moveRight(shift, false); break;
            case SDLK_UP:    ed.moveUp(shift); break;
            case SDLK_DOWN:  ed.moveDown(shift); break;
            case SDLK_HOME:  ed.moveHome(shift, false); break;
            case SDLK_END:   ed.moveEnd(shift, false); break;
            }
        }
        break;
    }
    case SDL_TEXTEDITING:
        if (!select) {
            break;
        }
        ed.imeComposing = e.edit.text;
        ed.imeCursor = e.edit.start;
        break;
    case SDL_TEXTINPUT:
        if (!select) {
            break;
        }
        ed.imeComposing.clear();
        ed.imeCursor = 0;
        ed.insertText(e.text.text);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (!select) {
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            auto p = ed.hitTest(e.button.x, e.button.y, renderer.getFont_sml());
            if (e.button.clicks == 2) {
                ed.cursor = p;
                ed.hasSelection = true;
                ed.selAnchor = p;
                ed.selAnchor.col = utf8::prevWord(ed.buf.line(p.row), p.col);
                ed.cursor.col = utf8::nextWord(ed.buf.line(p.row), p.col);
            }
            else {
                bool sh = (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (sh) ed.startSelection(); else ed.clearSelection();
                ed.cursor = p;
                if (!sh) ed.selAnchor = p;
                ed.resetBlink();
            }
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (!select) {
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT)
            mouseDown = false;
        break;

    case SDL_MOUSEMOTION:
        if (!select) {
            break;
        }
        if (mouseDown) {
            ed.hasSelection = true;
            ed.cursor = ed.hitTest(e.motion.x, e.motion.y, renderer.getFont_sml());
        }
        break;

    case SDL_MOUSEWHEEL:
        if (!select) {
            break;
        }
        ed.scrollRow = std::clamp(ed.scrollRow - e.wheel.y, 0, ed.buf.numLines() - 1);
        break;

    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_RESIZED)
            ed.viewRows = (e.window.data2 - ed.PADDING * 2) / renderer.lineH;
        break;
    }
    if (select) {
        ed.tickBlink();
    }
}

void file_explorer_event(SDL_Event& e,file_explorer& fi){
    int nm_x = 0;
    int nm_y = 0;
    SDL_GetMouseState(&nm_x,&nm_y);
    bool hv = fi.F_X < nm_x && nm_x < fi.F_X + fi.F_W && fi.F_Y < nm_y && nm_y < fi.F_Y + fi.F_H;
    switch (e.type) {
    case SDL_MOUSEWHEEL:
        if(!hv) { break; }
        fi.scrollRow = std::clamp(fi.scrollRow - e.wheel.y, 0, int(fi.file_lists.size() - 1));
        break;
    }
	bool click = e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 1 && e.button.button == SDL_BUTTON_LEFT;
    fi.hitscan({nm_x,nm_y}, click);
    bool dclick = e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2 && e.button.button == SDL_BUTTON_LEFT;
}