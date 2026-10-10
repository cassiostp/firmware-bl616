// Host sim FatFs-over-host-directory implementation. Both "sd:" and "usb:"
// drives map to the same host directory (the virtual SD card); there is no
// USB MSC in the sim.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
// The firmware's FatFs DIR type clashes with libc's DIR from <dirent.h>:
// include dirent under an alias, then declare our own DIR via ff.h.
#define DIR SYS_DIR
#include <dirent.h>
#undef DIR
#include <vector>
#include <algorithm>

#include "ff.h"

namespace {
std::string g_root = ".";

std::string host_path(const char *p) {
    std::string s = p ? p : "";
    size_t c = s.find(':');
    std::string rel = (c == std::string::npos) ? s : s.substr(c + 1);
    while (!rel.empty() && rel[0] == '/')
        rel.erase(rel.begin());
    if (rel.empty())
        return g_root;
    return g_root + "/" + rel;
}

struct DirState {
    std::vector<std::string> names;
    std::vector<bool> is_dir;
    size_t idx = 0;
};

const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    const char *c = strrchr(p, ':');
    const char *s = b ? b + 1 : p;
    if (c && c + 1 > s)
        s = c + 1;
    return s;
}
} // namespace

void ffsim_set_root(const char *dir) {
    g_root = dir ? dir : ".";
}

extern "C" {

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt) {
    (void)fs;
    (void)path;
    (void)opt;
    return FR_OK;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
    if (!fp || !path)
        return FR_INVALID_PARAMETER;
    std::string hp = host_path(path);
    bool rd = mode & FA_READ;
    bool wr = mode & FA_WRITE;
    const char *m = nullptr;
    if ((mode & FA_CREATE_ALWAYS) != 0) {
        m = rd ? "w+b" : "wb";
    } else if ((mode & FA_OPEN_ALWAYS) != 0) {
        FILE *f = fopen(hp.c_str(), "r+b");
        if (!f)
            f = fopen(hp.c_str(), "w+b");
        if (!f)
            return FR_NO_FILE;
        fp->_sim_fp = f;
        return FR_OK;
    } else if (wr) {
        m = "r+b"; // existing file, read/write (floppies)
    } else {
        m = "rb";
    }
    FILE *f = fopen(hp.c_str(), m);
    if (!f) {
        if (!wr)
            return FR_NO_FILE;
        // FA_WRITE on a missing file without a create flag: create it, since
        // nothing in the firmware depends on the failure.
        f = fopen(hp.c_str(), "w+b");
        if (!f)
            return FR_DENIED;
    }
    fp->_sim_fp = f;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    if (!fp || !fp->_sim_fp)
        return FR_INVALID_OBJECT;
    size_t n = fread(buff, 1, btr, fp->_sim_fp);
    if (br)
        *br = (UINT)n;
    if (n < btr && ferror(fp->_sim_fp))
        return FR_DISK_ERR;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    if (!fp || !fp->_sim_fp)
        return FR_INVALID_OBJECT;
    size_t n = fwrite(buff, 1, btw, fp->_sim_fp);
    if (bw)
        *bw = (UINT)n;
    fflush(fp->_sim_fp);
    if (n < btw)
        return FR_DISK_ERR;
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, DWORD ofs) {
    if (!fp || !fp->_sim_fp)
        return FR_INVALID_OBJECT;
    if (fseek(fp->_sim_fp, (long)ofs, SEEK_SET) != 0)
        return FR_DISK_ERR;
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    if (!fp || !fp->_sim_fp)
        return FR_INVALID_OBJECT;
    FILE *f = fp->_sim_fp;
    fp->_sim_fp = nullptr;
    return fclose(f) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_sync(FIL *fp) {
    if (!fp || !fp->_sim_fp)
        return FR_INVALID_OBJECT;
    return fflush(fp->_sim_fp) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_stat(const char *path, FILINFO *fno) {
    if (!path || !fno)
        return FR_INVALID_PARAMETER;
    struct stat st;
    if (stat(host_path(path).c_str(), &st) != 0)
        return FR_NO_FILE;
    memset(fno, 0, sizeof(*fno));
    snprintf(fno->fname, sizeof(fno->fname), "%s", base_name(path));
    fno->fsize = S_ISDIR(st.st_mode) ? 0 : (DWORD)st.st_size;
    fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    return FR_OK;
}

FRESULT f_opendir(DIR *dp, const char *path) {
    if (!dp || !path)
        return FR_INVALID_PARAMETER;
    std::string hp = host_path(path);
    SYS_DIR *sys = opendir(hp.c_str());
    if (!sys)
        return FR_NO_PATH;
    DirState *st = new DirState();
    struct dirent *e;
    while ((e = readdir(sys)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        std::string full = hp + "/" + e->d_name;
        struct stat sbuf;
        bool dir = (stat(full.c_str(), &sbuf) == 0 && S_ISDIR(sbuf.st_mode));
        st->names.push_back(e->d_name);
        st->is_dir.push_back(dir);
    }
    closedir(sys);
    // Deterministic order (real FAT returns directory order, which varies).
    std::vector<size_t> order(st->names.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return st->names[a] < st->names[b]; });
    std::vector<std::string> sn;
    std::vector<bool> sd;
    for (size_t i : order) {
        sn.push_back(st->names[i]);
        sd.push_back(st->is_dir[i]);
    }
    st->names = std::move(sn);
    st->is_dir = std::move(sd);
    dp->_sim_opaque = st;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    if (!dp || !dp->_sim_opaque || !fno)
        return FR_INVALID_PARAMETER;
    DirState *st = (DirState *)dp->_sim_opaque;
    memset(fno, 0, sizeof(*fno));
    if (st->idx >= st->names.size()) {
        fno->fname[0] = 0;
        return FR_OK;
    }
    snprintf(fno->fname, sizeof(fno->fname), "%s", st->names[st->idx].c_str());
    fno->fattrib = st->is_dir[st->idx] ? AM_DIR : 0;
    fno->fsize = 0;
    st->idx++;
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    if (!dp || !dp->_sim_opaque)
        return FR_INVALID_PARAMETER;
    delete (DirState *)dp->_sim_opaque;
    dp->_sim_opaque = nullptr;
    return FR_OK;
}

FRESULT f_mkdir(const char *path) {
    if (!path)
        return FR_INVALID_PARAMETER;
    if (mkdir(host_path(path).c_str(), 0777) != 0) {
        if (errno == EEXIST)
            return FR_EXIST;
        return FR_DENIED;
    }
    return FR_OK;
}

FRESULT f_unlink(const char *path) {
    if (!path)
        return FR_INVALID_PARAMETER;
    if (unlink(host_path(path).c_str()) != 0)
        return FR_NO_FILE;
    return FR_OK;
}

FRESULT f_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path)
        return FR_INVALID_PARAMETER;
    if (rename(host_path(old_path).c_str(), host_path(new_path).c_str()) != 0)
        return FR_DENIED;
    return FR_OK;
}

} // extern "C"
