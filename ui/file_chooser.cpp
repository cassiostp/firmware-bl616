#include <vector>
#include <string>

extern "C" {
#include "ff.h"
}

#include "file_chooser.h"
#include "game_controls.h"
#include "overlay.h"
#include "utils.h"

// The file list scrolls inside rows 2..22 (21 entries): rows 25-26 sit
// under the cores' logo, so row 23 carries the scroll markers instead.
#define PAGESIZE 21
#define TOPLINE 2

// void FileChooser::set_fs(FATFS *fs) {
//     this->fs = fs;
// }

// list files in dir, starting from number `start` and return at most `len` files. `count` is set to total number of files.
bool FileChooser::list_files(string dir, vector<FileEntry> &files, int start, int len, int *count) {
    DIR d;
    
    if (f_opendir(&d, dir.c_str()) != 0)
        return false;
    files.clear();

    // ".." entry at the top
    if (start == 0 && len > 0) {
        files.push_back({dir == rootdir ? msg_return : "..", true});
    }
    if (start > 0) start--;
    *count = 1;

    FILINFO fno;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0] != 0) {
        heartbeat_bump();           // big directories take a while to list
        bool is_dir = fno.fattrib & AM_DIR;
        // Hide files no loader accepts (e.g. gba_bios.bin in the GBA folder).
        // Directories are always shown so the user can navigate.
        if (!is_dir && !has_any_ext(fno.fname, filter_exts.c_str()))
            continue;
        (*count)++;
        if (start > 0) {
            start--;
        } else if (files.size() < len)
            files.push_back({fno.fname, is_dir});
    }
    f_closedir(&d);
    return true;
}

bool FileChooser::choose_file(string &res) {
    int page = 0, pages, total;
    int active = 0;
    vector<FileEntry> files;

    while (1) {
        overlay_clear();
        bool r = list_files(this->curdir, files, page*PAGESIZE, PAGESIZE, &total);
        if (r) {
            pages = (total+PAGESIZE-1) / PAGESIZE;
            overlay_status("Page ");
            overlay_printf("%d/%d", page+1, pages);
            if (active > files.size()-1)
                active = files.size()-1;
            for (int i = 0; i < PAGESIZE; i++) {
                int idx = page*PAGESIZE + i;
                overlay_cursor(2, i+TOPLINE);
                if (i < files.size()) {
                    overlay_printf(files[i].name.c_str());
                    if (idx != 0 && files[i].is_dir)
                        overlay_printf("/");
                }
            }
            // Scroll markers at the window's bottom edge: more above/below.
            if (page > 0) {
                overlay_cursor(2, TOPLINE+PAGESIZE);
                overlay_printf("^ more");
            }
            if (page < pages-1) {
                overlay_cursor(24, TOPLINE+PAGESIZE);
                overlay_printf("more v");
            }
            // The cursor sits on the current row through the settle delay
            // below, so the next press never snapshots a cursor-less list
            // (its tap would end on the cursor reappearing, selecting
            // nothing).
            overlay_cursor(0, TOPLINE + active);
            overlay_printf(">");
            delay(300);
            while (1) {
                int wrapped = 0;
                int r = joy_choice(TOPLINE, files.size(), &active, &wrapped);
                if (pending_action != ACTION_NONE)              // combo or MODE in game
                    return false;
                if (wrapped < 0) {              // up past the top: previous page,
                    page = (page > 0) ? page-1 : pages-1;   // wrapping to the last
                    active = PAGESIZE;          // (clamped to its last row below)
                    break;
                } else if (wrapped > 0) {       // down past the bottom: next page,
                    page = (page < pages-1) ? page+1 : 0;   // wrapping to the first
                    active = 0;
                    break;
                }
                if (r == 1 || r == 4) {
                    if (curdir == rootdir && page == 0 && active == 0) {// return to main menu
                        return false;
                    } else if (files[active].is_dir) {          // enter dir
                        if (page == 0 && active == 0) {         // return to parent dir
                            curdir = curdir.substr(0, curdir.find_last_of("/"));
                        } else {								// enter sub dir
                            curdir += "/" + files[active].name;
                        }
                        active = 0;
                        page = 0;
                        break;
                    } else {                                    // file chosen
                        res = curdir + "/" + files[active].name;
                        return true;
                    }
                } else if (r == 2 && page < pages-1) {
                    page++;
                    break;
                } else if (r == 3 && page > 0) {
                    page--;
                    break;
                }
                delay(10);
            }
        } else {
            overlay_status("Error opening directory");
            overlay_printf(" %d", r);
            return false;
        }
    }        
}