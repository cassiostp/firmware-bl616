#pragma once

#include <vector>
#include <string>
#include "menu_manager.h"
#include "usb_config.h"
#include "ff.h"

struct core_info {
    uint16_t id;                    // 1: NES, 2: SNES, 3: GB, 4: GENESIS, 0: end
    const char *display_name;
    const char *rom_dir;            // nes, snes, etc.
    const char *core_file;          // core file in cores/
    const char *rom_exts;           // ';'-separated ROM extensions, e.g. ".bin;.md;.gen;.smd"
    int (*load_rom)(const char *fname);
    Menu *(*create_menu)(const char *imgdir);
};

extern struct core_info *find_core_by_id(uint16_t id);

// True if `fname` has one of the core's ROM extensions (suffix match).
bool core_supports_file(const struct core_info *core, const char *fname);

extern std::vector<core_info> core_info_list;
extern std::vector<int16_t> main_menu_config;
extern void init_core_list();

extern int loadnes(const char *fname);
extern int loadsnes(const char *fname);
extern int loadgba(const char *fname);
extern bool gba_bios_loaded;            // the BIOS is in the GBA bitstream's memory
extern int loadmd(const char *fname);
extern int loadsms(const char *fname);
extern int loadpc(const char *fname);

extern bool find_core_for_board(std::string &fname, const char *core_name);

struct PcxtMenu: Menu {
    std::string imgdir;
    PcxtMenu(const char *imgdir);
    virtual void render() override;
    virtual std::vector<int> get_options() override;
    virtual bool on_choose(int idx) override;
};

Menu *create_default_menu(const char *imgdir);
Menu *create_pcxt_menu(const char *imgdir);

extern bool floppy[2];
extern std::string floppy_fname[2];
extern std::string floppy_path[2];
extern bool mount_floppy(int drive, const char *fname);
extern void forget_floppies();
extern USB_NOCACHE_RAM_SECTION FIL f_floppy[2];