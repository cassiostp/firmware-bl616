#include "menu_manager.h"
#include <memory>
#include <vector>

#include "utils.h"
#include "console.h"
#include "game_controls.h"
#include "dbg_log.h"

std::vector<std::unique_ptr<Menu>> menu_stack;

void menu_clear() {
    DEBUG("Menu clear\n");
    menu_stack = std::vector<std::unique_ptr<Menu>>();
    DEBUG("Menu clear done\n");
}

void push_menu(std::unique_ptr<Menu> menu) {
    menu_stack.push_back(std::move(menu));
}

void pop_menu() {
    menu_stack.pop_back();
}

Menu* menu_current() {
    return menu_stack.back().get();
}

bool menu_is_active() {
    return !menu_stack.empty();
} 

// After a scrolling menu took an Up/Down step. Only a step that moved the
// window redraws (overlay_clear blanks the screen, so redrawing on every
// step made these menus blink); otherwise the > marker just moves, drawn
// by the loop like any menu's. A moved window redraws at once: the cursor
// can sit still while the list moves, so without the redraw one tap would
// step twice. Its debounce is here, as the loop's only covers a cursor
// that changed row.
static void scroll_menu_moved(std::vector<int> &options, int &active) {
    Menu *m = menu_current();
    if (m->redraw) {
        m->render();
        m->redraw = false;
        options = m->get_options();
        pad_wait(100);
    }
    active = m->cursor_index();
    suppress_held_buttons();            // one press, one step
}

void menu_input_loop() {
    if (!menu_is_active())
        return;

    dbg_log("menu loop: enter, depth=%d", (int)menu_stack.size());
    DEBUG("Menu input loop\n");
    std::vector<int> options = menu_current()->get_options();
    int last = 0, active = 0;
    while (menu_is_active() && overlay_on() && pending_action == ACTION_NONE) {
        if (menu_current()->redraw) {
            menu_current()->render();
            menu_current()->redraw = false;
        }

        uint16_t joy1=0, joy2=0, hid1=0, hid2=0;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        draw_diag_line(joy1, joy2, hid1, hid2);   // render() cleared rows 0-1
        joy1 |= hid1;
        joy2 |= hid2;

        if (controls_poll(joy1, joy2, false, game_loaded()))   // menu combo: back to the game
            break;
        if (combo_in_progress(joy1, joy2)) {    // its buttons aren't navigation
            delay(10);
            continue;
        }

        if ((joy1 & 0x10) || (joy2 & 0x10)) {   // up: wrap to the last item
            if (menu_current()->move_cursor(-1)) {
                scroll_menu_moved(options, active);
            } else if (active > 0) {
                active--;
                suppress_held_buttons();    // one press, one step
            } else if (!options.empty()) {
                active = options.size() - 1;
                suppress_held_buttons();    // one press, one step
            }
        }
        if ((joy1 & 0x20) || (joy2 & 0x20)) {   // down: wrap to the first item
            if (menu_current()->move_cursor(+1)) {
                scroll_menu_moved(options, active);
            } else if (active < (int)options.size() - 1) {
                active++;
                suppress_held_buttons();    // one press, one step
            } else {
                active = 0;
                suppress_held_buttons();    // one press, one step
            }
        }

        for (int i = 0; i < options.size(); i++) {
            overlay_cursor(0, options[i]);
            if (i == active)
                overlay_printf(">");
            else
                overlay_printf(" ");
        }

        if ((joy1 & 0x100) || (joy2 & 0x100) || // button A pressed
            (joy1 & 0x1) || (joy2 & 0x1))       // button B pressed
        {
            int depth = menu_stack.size();
            suppress_held_buttons();            // one press, one choice
            bool r = menu_current()->on_choose(active);
            if (r) {
                pop_menu();
                if (!menu_is_active()) {
                    dbg_log("menu loop: exit, stack empty, pending=%d",
                            (int)pending_action);
                    delay(100);
                    return;
                }
                menu_current()->do_redraw();
            }
            if (menu_stack.size() != depth) {   // menu changed: start at its top
                options = menu_current()->get_options();
                active = last = 0;
                // ...but a scrolling menu keeps its position when coming
                // back from a submenu.
                int ci = menu_current()->cursor_index();
                if (ci >= 0 && ci < (int)options.size())
                    active = last = ci;
                menu_current()->do_redraw();
            }
        }

        if (key_input() == '`') {
            // enter command console
            command_console();
            menu_current()->do_redraw();
        }

        if (last != active)
            pad_wait(100);
        last = active;
    }
    dbg_log("menu loop: exit, depth=%d overlay=%d pending=%d",
            (int)menu_stack.size(), overlay_on() ? 1 : 0, (int)pending_action);
}

void ScrollMenu::render() {
    int n = item_count();
    if (selected < 0 || selected >= n) selected = 0;
    if (scroll > selected) scroll = selected;
    if (scroll + MENU_LIST_ROWS <= selected) scroll = selected - MENU_LIST_ROWS + 1;
    if (scroll < 0) scroll = 0;
    overlay_clear();
    overlay_cursor(0, 6);
    //              01234567890123456789012345678901
    overlay_printf("%s", menu_title());
    if (scroll > 0) {                 // more above
        overlay_cursor(30, MENU_LIST_TOP - 1);
        overlay_printf("^");
    }
    for (int i = 0; i < MENU_LIST_ROWS && scroll + i < n; i++) {
        overlay_cursor(2, MENU_LIST_TOP + i);
        overlay_printf("%s", item_text(scroll + i).c_str());
    }
    if (scroll + MENU_LIST_ROWS < n) {  // more below
        overlay_cursor(30, MENU_LIST_TOP + MENU_LIST_ROWS);
        overlay_printf("v");
    }
    render_footer();
}

std::vector<int> ScrollMenu::get_options() {
    std::vector<int> rows;
    int n = item_count();
    for (int i = 0; i < MENU_LIST_ROWS && scroll + i < n; i++)
        rows.push_back(MENU_LIST_TOP + i);
    return rows;
}

bool ScrollMenu::on_choose(int idx) {
    return item_chosen(scroll + idx);
}

bool ScrollMenu::move_cursor(int dir) {
    int n = item_count();
    if (n <= 0)
        return true;
    int old_scroll = scroll;
    selected += dir;
    if (selected < 0) selected = n - 1;    // wrap
    if (selected >= n) selected = 0;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + MENU_LIST_ROWS) scroll = selected - MENU_LIST_ROWS + 1;
    if (scroll != old_scroll)
        do_redraw();                        // the window moved
    return true;
}

int ScrollMenu::cursor_index() {
    return selected - scroll;
}
