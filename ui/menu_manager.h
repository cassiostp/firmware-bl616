#pragma once

#include <memory>
#include <string>
#include <vector>
#include "overlay.h"

// In core popup menu

struct Menu {
    bool redraw = false;
    virtual void do_redraw() {
        redraw = true;
    }
    virtual ~Menu() = default;
    virtual void render() = 0;
    virtual std::vector<int> get_options() = 0;   // return list of choice rows
    virtual bool on_choose(int idx) = 0;          // called when a choice is selected
                                                  // return true if menu should be popped
    // Up/Down navigation. The input loop wraps by default (Up on the first
    // item goes to the last, Down on the last goes to the first); a menu
    // with more items than visible rows overrides move_cursor to scroll
    // instead (ScrollMenu below), returning true when it handled the step.
    virtual bool move_cursor(int dir) { (void)dir; return false; }
    virtual int cursor_index() { return -1; }
};

void menu_clear();
void push_menu(std::unique_ptr<Menu> menu);
void pop_menu();
Menu* menu_current();
void menu_render();
bool menu_is_active();     // true if the menu stack is not empty
void menu_input_loop();    // return when OSD is turned off or "<< Main Menu" is selected

// The OSD is 32x28. Row 0-1 are the diagnostics, rows 25-26 sit under the
// cores' logo (textdisp.v: LOGO_Y 201, 14 pixels tall, so rows 25 and 26),
// and row 27 is the transient status line. Menu content stays in 2..24.
#define MENU_FIRST_ROW 2
#define MENU_LAST_ROW 24

// A menu whose items scroll inside a fixed window: the title above, the
// visible items in [MENU_LIST_TOP, MENU_LIST_TOP + MENU_LIST_ROWS), a ^
// marker at the window's top edge when there is more above and a v marker
// at its bottom edge when there is more below, and a footer (help or
// message lines) below the window. Up on the first item wraps to the last
// and Down on the last wraps to the first, scrolling to keep the cursor
// visible. Both markers are in the overlay font (font.vh U+005E, U+0076).
#define MENU_LIST_TOP 9
#define MENU_LIST_ROWS 12
#define MENU_FOOTER_ROW 22

struct ScrollMenu: Menu {
    int selected = 0;   // index into the items
    int scroll = 0;     // first visible item
    virtual const char *menu_title() = 0;
    virtual int item_count() = 0;
    virtual std::string item_text(int i) = 0;   // row content, without the cursor
    virtual bool item_chosen(int i) = 0;        // true: pop this menu
    virtual void render_footer() {}             // rows MENU_FOOTER_ROW..MENU_LAST_ROW
    void render() override;
    std::vector<int> get_options() override;
    bool on_choose(int idx) override;
    bool move_cursor(int dir) override;
    int cursor_index() override;
};

struct DefaultMenu: Menu {
    DefaultMenu() {
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 13);
        overlay_printf("<< Main Menu\n");
    }

    std::vector<int> get_options() override {
        return {13};
    }

    bool on_choose(int idx) override {
        return true;
    }
};

