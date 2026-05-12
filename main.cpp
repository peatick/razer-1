#include "sdl2/include/SDL.h"
#include "sdl2/include/SDL_ttf.h"
#include "sdlutil.h"
#include <algorithm>
#include <climits>
#include <deque>
#include <sstream>
#include <string>
#include <vector>
bool buttonClicked(SDL_Event& e, SDL_Rect r,SDL_Point p) {
    if (e.type != SDL_MOUSEBUTTONDOWN) return false;

    return SDL_PointInRect(&p, &r);
}

int main(int argc, char* argv[]) {
    const char* fontPath = (argc > 1) ? argv[1] : "";

    Renderer renderer;
    if (!renderer.init(fontPath)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Init: %s | %s", SDL_GetError(), TTF_GetError());
        return 1;
    }

    Editor ed;
    file_explorer nm;
	nm.F_X = 10; nm.F_Y = 20;
    nm.F_W = 200;nm.F_H = 190;
    ed.lineH = renderer.lineH;
    ed.init();
    ed.buf.setAllText(
        "SDL2 テキストエディタ  (差分方式 Undo/Redo)\n"
        "IME入力（日本語・中国語・韓国語）対応\n"
        "\n"
        "ショートカット:\n"
        "  Ctrl+A            全選択\n"
        "  Ctrl+C            コピー\n"
        "  Ctrl+X            カット\n"
        "  Ctrl+V            ペースト\n"
        "  Ctrl+Z            アンドゥ\n"
        "  Ctrl+Y / Ctrl+Shift+Z  リドゥ\n"
        "  Shift+矢印        選択\n"
        "  Ctrl+矢印         単語単位移動\n"
        "  Home/End          行頭/行末\n"
        "  Ctrl+Home/End     文書頭/末尾\n"
        "  ダブルクリック    単語選択\n"
    );
    nm.init(renderer.lineH);
    int mx = 0;
    int my = 0;
	int mousex = 0, mousey = 0;
    bool running = true, mouseDown = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            mousex = e.button.x;
            mousey = e.button.y;
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    nm.path_set(false,"");
                    break;
                }
                break;
            }
            textEditEvent(e, ed, renderer, mouseDown, mx, my);
            textEditEvent_sh(e, nm.ed, renderer, mouseDown, mx, my);
            file_explorer_event(e,nm);
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                mx = e.button.x;
                my = e.button.y;
            }
            if (buttonClicked(e, {0,0,70,20}, {e.button.x,e.button.y})) {
                printf("Click!\n");
                
            }
            
        }
        renderer.draw_bg({250,250,250,255});
        //renderer.TextBox(ed);
        //renderer.btn_draw({ 0,0,70,20 }, { mousex,mousey }, "クリック");
        renderer.update_fs_explorer(nm);
        renderer.rend();
    }
    renderer.destroy();
    return 0;
}

