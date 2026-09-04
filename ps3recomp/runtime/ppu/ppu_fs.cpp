/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Only the calls the boot actually imports are implemented:
 *   cellFsOpen/Close/Read/Write/Lseek/Stat/Fstat/Opendir/Readdir/Closedir/
 *   Mkdir/Rmdir/Unlink/Fsync.
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#include "ppu_recomp.h"      /* ppu_context */
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include <stdint.h>
#include <ps3emu/host_platform.h>
#ifdef _WIN32
#include <windows.h>         /* GetCurrentThreadId: tag fs opens with the thread */
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <algorithm>
#include <mutex>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void     ydkj_parse_yield(void);
extern "C" void     ppu_dump_guest_stack(ppu_context* ctx, const char* tag);
/* Title-local ATRAC can acknowledge compressed preview refills without
 * copying bytes that its in-process decoder has already consumed. Weak keeps
 * the generic runtime independent of Taiko when linked by another title. */
extern "C" __attribute__((weak)) int
taiko_atrac_discard_stream_read(uint32_t buffer, uint64_t bytes);
/* Titles may transparently provide a read-only file overlay. Returning null
 * leaves the translated host path untouched. The returned FILE belongs to the
 * VFS and is closed normally by cellFsClose. */
extern "C" __attribute__((weak)) FILE*
taiko_fs_open_overlay(const char* guest_path, const char* host_path,
                      uint32_t flags);
/* Let the same overlay adjust pathname-based stat results before an open.
 * Returning zero preserves the real host file size. */
extern "C" __attribute__((weak)) int
taiko_fs_stat_overlay(const char* guest_path, const char* host_path,
                      uint64_t* size);
#ifdef _WIN32
extern "C" char __ImageBase;
extern "C" unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long,unsigned long,void**,unsigned long*);
#endif

/* Host root that the PS3 mount points map into (dir containing PS3_GAME). */
extern "C" const char* ppu_vfs_root = ".";

/* CELL_FS return / mode / flag constants. */
#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418090)   /* 0x80010006 */
#define CELL_FS_EIO          (-2147418111)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
#define CELL_FS_O_CREAT      0x0200
#define CELL_FS_O_TRUNC      0x0400
#define CELL_FS_O_APPEND     0x0100
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */
static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        if (ppu_vm_size && gaddr + i >= ppu_vm_size) break;
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

/* Translate a guest path to a host path under ppu_vfs_root. Known PS3 mount
 * prefixes are stripped; the rest is appended to the root. */
static void host_path(char* out, size_t cap, const char* guest)
{
    const char* layout = getenv("PS3_VFS_LAYOUT");
    const bool usrdir_layout = layout && strcmp(layout, "usrdir") == 0;

    /* Taiko's Boost/XML reader currently aborts after materializing the retail
     * 869-entry forbidden-name table.  The list is not needed for offline
     * play, so feed the title a schema-compatible empty list while retaining
     * the original game asset unchanged for a future parser fix. */
    if (strcmp(guest, "/data/config/common/forbidden.xml") == 0) {
        snprintf(out, cap,
                 "%s/data/config/common/forbidden-recomp.xml",
                 ppu_vfs_root);
        return;
    }

    /* Green expects its per-build DATA00000.BIN on a USB stick.  Zucchini
     * deliberately keeps the file beside the game instead, and redirects
     * both the VERSIONUP directory probe and the eventual file open.  Mirror
     * that layout in the recomp VFS so stat/opendir/open all agree. */
    if (strncmp(guest, "/dev_usb", 8) == 0) {
        const char* versionup = strstr(guest, "/VERSIONUP");
        if (versionup &&
            (strcmp(versionup, "/VERSIONUP") == 0 ||
             strcmp(versionup, "/VERSIONUP/") == 0)) {
            snprintf(out, cap, usrdir_layout ? "%s" :
                     "%s/game/SCEEXE001/USRDIR", ppu_vfs_root);
            return;
        }
        if (versionup &&
            strcmp(versionup, "/VERSIONUP/DATA00000.BIN") == 0) {
            snprintf(out, cap, usrdir_layout ? "%s/DATA00000.BIN" :
                     "%s/game/SCEEXE001/USRDIR/DATA00000.BIN", ppu_vfs_root);
            return;
        }
    }

    const char* rel = guest;
    if (usrdir_layout) {
        /* Standalone releases live directly in the user's USRDIR. Collapse all
         * spellings of that directory to the executable directory instead of
         * requiring a synthetic PS3 mount tree or Windows junctions. */
        static const char disc_usrdir[] = "/dev_bdvd/PS3_GAME/USRDIR/";
        if (strncmp(guest, disc_usrdir, sizeof(disc_usrdir) - 1) == 0) {
            rel = guest + sizeof(disc_usrdir) - 1;
        } else if (const char* usr = strstr(guest, "/USRDIR/")) {
            rel = usr + 8;
        } else if (size_t length = strlen(guest);
                   length >= 7 && strcmp(guest + length - 7, "/USRDIR") == 0) {
            rel = guest + length;
        } else if (const char* app = strstr(guest, "app_home/")) {
            rel = app + 9;
        } else if (strcmp(guest, "/app_home") == 0 ||
                   strcmp(guest, "app_home") == 0) {
            rel = guest + strlen(guest);
        } else if (guest[0] == '/') {
            rel = guest + 1;
        }
        snprintf(out, cap, "%s/%s", ppu_vfs_root, rel);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    static const char* mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    for (size_t i = 0; i < sizeof(mounts)/sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(guest, mounts[i], n) == 0) { rel = guest + n; break; }
    }
    if (rel == guest && guest[0] == '/') rel = guest + 1;   /* strip leading '/' */
    snprintf(out, cap, "%s/%s", ppu_vfs_root, rel);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
}

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
static FILE* g_files[FS_MAX];
static DIR*  g_dirs[FS_MAX];
static char  g_file_path[FS_MAX][1024];  /* guest path per open file */
static char  g_dir_path[FS_MAX][1024];   /* host path per open dir (for readdir stat) */
/* Guest PPU threads call cellFs concurrently.  Protect both handle allocation
 * and each stdio/DIR operation: FILE positions and the shared slot tables are
 * not safe against a simultaneous close/reuse. */
static std::recursive_mutex g_fs_mutex;

static int fd_alloc_file(FILE* f)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_files[i] = f; return i; }
    return -1;
}
static int fd_alloc_dir(DIR* d)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_dirs[i] = d; return i; }
    return -1;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Song Select reversing aid: a solo fumen open is the first unambiguous
     * gameplay-preload boundary after the UI commits song and course.  Keep
     * this opt-in because the stack walk is intentionally expensive. */
    if (getenv("TAIKO_FUMEN_OPEN_TRACE") &&
        strstr(gpath, "/data/fumen/") && strstr(gpath, "/solo/") &&
        strstr(gpath, ".bin")) {
        fprintf(stderr,
                "[fumen-open] path='%s' path-ptr=%08X flags=%08X "
                "fd-ptr=%08X lr=%08X\n",
                gpath, (uint32_t)ctx->gpr[3], flags, fd_ptr,
                (uint32_t)ctx->lr);
        ppu_dump_guest_stack(ctx, "fumen-open");
    }

    /* fopen() mode strings can't express the PS3/POSIX open semantics (e.g.
     * O_WRONLY without create+truncate, or O_CREAT without O_TRUNC), so build
     * real open() flags from the access mode (low 2 bits) + the modifiers, then
     * wrap the fd in a FILE* with fdopen() (which neither creates nor truncates
     * -- that was already decided by open()). */
    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    FILE* f = taiko_fs_open_overlay
        ? taiko_fs_open_overlay(gpath, hpath, flags) : nullptr;
    if (!f) {
        int hfd = open(hpath, oflags | O_BINARY, 0666);
        if (hfd < 0) {
            fprintf(stderr, "[fs] open FAIL '%s' -> '%s' tid=%lu\n", gpath, hpath,
                    (unsigned long)ps3_host_thread_id());
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
        }
        const char* fmode = (acc == CELL_FS_O_RDWR)
                            ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                            : (acc == CELL_FS_O_WRONLY)
                              ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                              : "rb";
        f = fdopen(hfd, fmode);
        if (!f) {
            close(hfd);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    strncpy(g_file_path[fd], gpath, sizeof g_file_path[fd] - 1);
    g_file_path[fd][sizeof g_file_path[fd] - 1] = 0;
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    /* tid: identifies the asset-loader thread so a stalled load can be matched
     * against [STUCKSEM] / thread-state output. */
    fprintf(stderr, "[fs] open '%s' -> fd %d tid=%lu\n", gpath, fd,
            (unsigned long)ps3_host_thread_id());
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) {
        fclose(g_files[fd]);
        g_files[fd] = nullptr;
        g_file_path[fd][0] = 0;
    }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsRead(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    uint64_t raw_nbytes = ctx->gpr[5];
    long fpos_before = ftell(g_files[fd]);
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    size_t n = 0;
    const bool discard_atrac = taiko_atrac_discard_stream_read &&
        taiko_atrac_discard_stream_read(buf, nbytes);
    if (discard_atrac) {
        /* Preserve the exact fread position/count/EOF contract, but omit both
         * the host read and the guest-memory copy. cellAtracAddStreamData only
         * acknowledges these bytes; the title-local decoder already owns the
         * complete immutable RIFF. Use stdio seeks for Windows/Linux parity. */
        const long current = ftell(g_files[fd]);
        long end = -1;
        if (current >= 0 && fseek(g_files[fd], 0, SEEK_END) == 0)
            end = ftell(g_files[fd]);
        if (current >= 0 && end >= current &&
            fseek(g_files[fd], current, SEEK_SET) == 0) {
            n = (size_t)std::min<uint64_t>(
                nbytes, static_cast<uint64_t>(end - current));
            if (fseek(g_files[fd], current + (long)n, SEEK_SET) != 0)
                n = fread(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
        } else {
            if (current >= 0) fseek(g_files[fd], current, SEEK_SET);
            n = fread(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
        }
    } else {
        n = fread(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
    }
    const char* taiko_fs_trace = getenv("TAIKO_FS_TRACE");
    if (taiko_fs_trace && taiko_fs_trace[0] != '0' &&
        (strstr(g_file_path[fd], "packeddata.ddp") ||
         strstr(g_file_path[fd], "tuning.bin"))) {
        fprintf(stderr,
                "[TAIKO-LOAD] read path='%s' pos=%ld request=%llu result=%zu "
                "tid=%llu lr=0x%08X\n",
                g_file_path[fd], fpos_before,
                (unsigned long long)nbytes, n,
                (unsigned long long)ctx->thread_id, (uint32_t)ctx->lr);
        fflush(stderr);
    }
    /* Host cache reads return so quickly that Taiko's catalog thread can starve
     * the sibling loader which feeds the object graph it is parsing.  The real
     * PS3 I/O call naturally yields.  Recreate that scheduling point for this
     * title without retaining the very noisy per-4K diagnostic logging that
     * first revealed (and happened to mask) the race. */
    const char* taiko_fs_yield = getenv("TAIKO_FS_YIELD");
    if (taiko_fs_yield && taiko_fs_yield[0] != '0' &&
        (strstr(g_file_path[fd], "packeddata.ddp") ||
         strstr(g_file_path[fd], "tuning.bin")))
        for (int i = 0; i < 10; ++i)
            ydkj_parse_yield();
    if (strstr(g_file_path[fd], "/data/config/") &&
        strstr(g_file_path[fd], ".xml")) {
        fprintf(stderr,
                "[fs-xml] read '%s' pos=%ld request=%llu result=%zu eof=%d err=%d\n",
                g_file_path[fd], fpos_before, (unsigned long long)nbytes, n,
                feof(g_files[fd]), ferror(g_files[fd]));
    }
    if (getenv("YDKJ_FSDBG")) { static int _fd=0; if(_fd++<20) fprintf(stderr,"[FSDBG] fd=%d raw_nbytes=0x%llX clamped=0x%llX buf=0x%08X fpos_before=%ld n=%zu eof=%d err=%d\n", fd,(unsigned long long)raw_nbytes,(unsigned long long)nbytes,buf,fpos_before,n,feof(g_files[fd]),ferror(g_files[fd])); }
#ifdef _WIN32
    if (getenv("YDKJ_FSDBG") && buf==0 && raw_nbytes>0x10000) { static int _b=0; if(_b++<2){ void* fr[30]; unsigned short nn=RtlCaptureStackBackTrace(0,30,fr,0); uintptr_t mb=(uintptr_t)&__ImageBase; fprintf(stderr,"[FSBT] null-buf read caller rvas:"); for(unsigned short i=0;i<nn&&i<16;i++) fprintf(stderr," %llX",(unsigned long long)((uintptr_t)fr[i]-mb)); fprintf(stderr,"\n"); } }
#endif
    { static uint64_t tot=0; static int _n=0; tot+=n; if(_n++<50) fprintf(stderr,"[fs] read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)\n",fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],(unsigned long long)tot); }
    if (getenv("YDKJ_TOCTRACE") && nbytes >= 50000) {  /* data.toc read -> who parses it? */
        fprintf(stderr, "[TOC] data.toc read into buf=0x%08X n=%zu; lr=0x%08llX; guest-stack RAs:\n", buf, n, (unsigned long long)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t i = 0; i < 128 && sp + i*4 + 4 <= ppu_vm_size; i++) {
            uint32_t a = sp + i*4; uint32_t w = (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];
            if (w >= 0x10000u && w < 0x600000u) fprintf(stderr, "[TOC]   ra 0x%08X (@sp+0x%X)\n", w, i*4);
        }
    }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    size_t n = fwrite(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
    /* DIAGNOSTIC (FLOW_CFGBT=1): dump the guest back-chain when the game logs the
     * render-config failure, to locate setScreenRenderTargetInternal & the config obj. */
    if (getenv("FLOW_CFGBT") && buf && nbytes > 0 && nbytes < 4096 && vm_base) {
        char tmp[256]; uint32_t nn = (uint32_t)(nbytes < 255 ? nbytes : 255);
        memcpy(tmp, vm_base + buf, nn); tmp[nn] = 0;
        if (strstr(tmp,"config") || strstr(tmp,"Config") || strstr(tmp,"Mystery") ||
            strstr(tmp,"downsample") || strstr(tmp,"RenderTarget")) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            fprintf(stderr, "[cfgbt] write \"%.60s\" lr=0x%08X sp=0x%08X\n", tmp, (uint32_t)ctx->lr, sp);
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                if (nsp <= sp || nsp >= 0x10000000u) break;
                uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                fprintf(stderr, "[cfgbt]   #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
            fflush(stderr);
        }
    }
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsLseek(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    int seek_result = fseek(g_files[fd], (long)off, worigin);
    long p = ftell(g_files[fd]);
    if (strstr(g_file_path[fd], "/data/config/") &&
        strstr(g_file_path[fd], ".xml")) {
        fprintf(stderr,
                "[fs-xml] seek '%s' off=%lld wh=%u result=%d pos=%ld\n",
                g_file_path[fd], (long long)off, wh, seek_result, p);
    }
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, 0);            /* atime */
    vm_write64(sb + 0x14, 0);            /* mtime */
    vm_write64(sb + 0x1C, 0);            /* ctime */
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    struct stat st;
    if (stat(hpath, &st) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    uint64_t size = (uint64_t)st.st_size;
    if (taiko_fs_stat_overlay)
        taiko_fs_stat_overlay(gpath, hpath, &size);
    if (sb) write_stat(sb, mode, size);
    if (getenv("YDKJ_FSDBG") && strstr(gpath,".toc")) fprintf(stderr,"[FSDBG] cellFsStat('%s') -> size=0x%llX\n",gpath,(unsigned long long)size);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    long cur = ftell(g_files[fd]);
    fseek(g_files[fd], 0, SEEK_END);
    long sz = ftell(g_files[fd]);
    fseek(g_files[fd], cur, SEEK_SET);
    if (sb) write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz);
    if (getenv("YDKJ_FSDBG")) { static int _n=0; if(_n++<12) fprintf(stderr,"[FSDBG] cellFsFstat(fd=%d) -> size=0x%lX\n",fd,sz); }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { closedir(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    strncpy(g_dir_path[fd], hpath, sizeof g_dir_path[fd] - 1);
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    if (fd < 0 || fd >= FS_MAX || !g_dirs[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    struct dirent* e = readdir(g_dirs[fd]);
    if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", g_dir_path[fd], e->d_name);
    uint8_t type = (stat(full, &st) == 0 && (st.st_mode & S_IFDIR))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(e->d_name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)e->d_name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_dirs[fd]) { closedir(g_dirs[fd]); g_dirs[fd] = nullptr; }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsMkdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsFsync(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> fs_guard(g_fs_mutex);
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) fflush(g_files[fd]);
    ctx->gpr[3] = CELL_OK;
}

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
}
