// Kernel32/advapi32/version subset for the real-WinMain build.
//
// Files live in the Emscripten filesystem (lower-case paths, see runtime.cpp). Modules loaded with
// LoadLibraryA are the game's own PE files; their resources (strings, bitmaps, WAVs) are read from the
// PE resource directory, so the language DLLs from the supplied folder supply all text.
#include "port_prefix.h"
#include <emscripten.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <string>
#include <vector>

#ifndef GENERIC_WRITE
#define GENERIC_WRITE 0x40000000u
#endif
#ifndef CREATE_NEW
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#endif
#ifndef TRUNCATE_EXISTING
#define TRUNCATE_EXISTING 5
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2
#endif
#ifndef ERROR_NO_MORE_FILES
#define ERROR_NO_MORE_FILES 18
#endif

static DWORD last_error;

// Program path as the CRT reports it; the game logs this file's date (FindFirstFileA).
extern "C" { char* _pgmptr = (char*) "empiresx.exe"; }

// ---------------------------------------------------------------- paths
static std::string win_path(const char* path) {
    std::string p = path ? path : "";
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\') p[i] = '/';
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
    }
    // C: is /c (as in runtime.cpp's fopen mapping); D:, the game CD, is the game folder at the root.
    if (p.size() > 1 && p[1] == ':') p = p[0] == 'd' ? p.substr(2) : "/" + p.substr(0, 1) + p.substr(2);
    if (p.empty()) p = ".";
    return p;
}

// ---------------------------------------------------------------- handles
struct KHandle {
    enum Kind { File, Mapping, Find, Mutex, Event } kind;
    int fd;
    std::vector<unsigned char> data;      // Mapping: snapshot of the file
    DIR* dir;                              // Find
    std::string folder, pattern;           // Find
    KHandle(Kind k) : kind(k), fd(-1), dir(0) {}
};
static std::vector<KHandle*> handles;
static KHandle* lookup(HANDLE h) {
    for (size_t i = 0; i < handles.size(); i++) if (handles[i] == h) return handles[i];
    return 0;
}
static HANDLE track(KHandle* h) { handles.push_back(h); return (HANDLE) h; }

static void unix_to_filetime(time_t t, FILETIME* out) {
    unsigned long long v = ((unsigned long long) t + 11644473600ULL) * 10000000ULL;
    out->dwLowDateTime = (DWORD) v;
    out->dwHighDateTime = (DWORD) (v >> 32);
}

static bool fill_find_data(KHandle* h, WIN32_FIND_DATAA* out) {
    dirent* entry;
    while ((entry = readdir(h->dir))) {
        if (fnmatch(h->pattern.c_str(), entry->d_name, 0)) continue;
        std::string name = h->folder + "/" + entry->d_name;
        struct stat st;
        if (stat(name.c_str(), &st)) continue;
        memset(out, 0, sizeof(*out));
        out->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        out->nFileSizeLow = (DWORD) st.st_size;
        unix_to_filetime(st.st_mtime, &out->ftLastWriteTime);
        out->ftCreationTime = out->ftLastAccessTime = out->ftLastWriteTime;
        strncpy(out->cFileName, entry->d_name, sizeof(out->cFileName) - 1);
        return true;
    }
    return false;
}

// Recovered code also calls POSIX open/unlink/chdir directly with Windows paths ("scenario\\x.scn").
// The game build links with --wrap for these, so every path goes through the same normalization.
extern "C" {
int __real_open(const char*, int, ...);
int __real_unlink(const char*);
int __real_chdir(const char*);

int __wrap_open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    int fd = __real_open(path, flags, mode);
    if (fd >= 0 || !path) return fd;
    std::string p = win_path(path);
    return p == path ? fd : __real_open(p.c_str(), flags, mode);
}
int __wrap_unlink(const char* path) { int r = __real_unlink(path); return r == 0 || !path ? r : __real_unlink(win_path(path).c_str()); }
int __wrap_chdir(const char* path) { int r = __real_chdir(path); return r == 0 || !path ? r : __real_chdir(win_path(path).c_str()); }
}

extern "C" {

DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE) {
    std::string path = win_path(name);
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
        case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
        default: break; // OPEN_EXISTING
    }
    int fd = open(path.c_str(), flags, 0666);
    if (fd < 0) { last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    KHandle* h = new KHandle(KHandle::File);
    h->fd = fd;
    last_error = 0;
    return track(h);
}

BOOL ReadFile(HANDLE file, LPVOID out, DWORD size, LPDWORD done, LPVOID) {
    KHandle* h = lookup(file);
    if (!h || h->kind != KHandle::File) return FALSE;
    ssize_t n = read(h->fd, out, size);
    if (done) *done = n < 0 ? 0 : (DWORD) n;
    return n >= 0;
}

BOOL WriteFile(HANDLE file, LPCVOID in, DWORD size, LPDWORD done, LPVOID) {
    KHandle* h = lookup(file);
    if (!h || h->kind != KHandle::File) return FALSE;
    ssize_t n = write(h->fd, in, size);
    if (done) *done = n < 0 ? 0 : (DWORD) n;
    return n >= 0;
}

DWORD GetFileSize(HANDLE file, LPDWORD high) {
    KHandle* h = lookup(file);
    if (high) *high = 0;
    struct stat st;
    if (!h || h->kind != KHandle::File || fstat(h->fd, &st)) return 0xffffffffu;
    return (DWORD) st.st_size;
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR) {
    KHandle* f = lookup(file);
    if (!f || f->kind != KHandle::File) return 0;
    struct stat st;
    if (fstat(f->fd, &st)) return 0;
    KHandle* m = new KHandle(KHandle::Mapping);
    m->data.resize(st.st_size);
    if (st.st_size && pread(f->fd, &m->data[0], st.st_size, 0) != st.st_size) { delete m; return 0; }
    return track(m);
}

// Views are private read-only copies; the game maps its archives read-only (FILE_MAP_READ).
static std::map<void*, size_t> views;
LPVOID MapViewOfFile(HANDLE mapping, DWORD, DWORD, DWORD offset, SIZE_T size) {
    KHandle* m = lookup(mapping);
    if (!m || m->kind != KHandle::Mapping || offset > m->data.size()) return 0;
    if (!size) size = m->data.size() - offset;
    void* view = malloc(size ? size : 1);
    if (!view) return 0;
    if (size) memcpy(view, &m->data[offset], size);
    views[view] = size;
    return view;
}

BOOL UnmapViewOfFile(LPCVOID view) {
    std::map<void*, size_t>::iterator it = views.find((void*) view);
    if (it == views.end()) return FALSE;
    free(it->first);
    views.erase(it);
    return TRUE;
}

// The archives are decommitted page ranges inside a view; the copy stays valid until unmapped.
BOOL VirtualFree(LPVOID, SIZE_T, DWORD) { return TRUE; }

BOOL CloseHandle(HANDLE handle) {
    KHandle* h = lookup(handle);
    if (!h) return FALSE;
    if (h->kind == KHandle::File) close(h->fd);
    if (h->kind == KHandle::Find && h->dir) closedir(h->dir);
    for (size_t i = 0; i < handles.size(); i++) if (handles[i] == h) { handles.erase(handles.begin() + i); break; }
    delete h;
    return TRUE;
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* out) {
    std::string p = win_path(pattern);
    size_t slash = p.find_last_of('/');
    KHandle* h = new KHandle(KHandle::Find);
    h->folder = slash == std::string::npos ? "." : (slash == 0 ? "/" : p.substr(0, slash));
    h->pattern = slash == std::string::npos ? p : p.substr(slash + 1);
    h->dir = opendir(h->folder.c_str());
    if (!h->dir || !fill_find_data(h, out)) {
        if (h->dir) closedir(h->dir);
        delete h;
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return track(h);
}

BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA* out) {
    KHandle* h = lookup(handle);
    if (!h || h->kind != KHandle::Find) return FALSE;
    if (fill_find_data(h, out)) return TRUE;
    last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}

BOOL FindClose(HANDLE handle) { return CloseHandle(handle); }

BOOL FileTimeToSystemTime(const FILETIME* in, SYSTEMTIME* out) {
    unsigned long long v = ((unsigned long long) in->dwHighDateTime << 32) | in->dwLowDateTime;
    time_t t = (time_t) (v / 10000000ULL - 11644473600ULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    memset(out, 0, sizeof(*out));
    out->wYear = tmv.tm_year + 1900;
    out->wMonth = tmv.tm_mon + 1;
    out->wDayOfWeek = tmv.tm_wday;
    out->wDay = tmv.tm_mday;
    out->wHour = tmv.tm_hour;
    out->wMinute = tmv.tm_min;
    out->wSecond = tmv.tm_sec;
    return TRUE;
}

// One game instance per browser worker: the mutex never pre-exists.
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR) { last_error = 0; return track(new KHandle(KHandle::Mutex)); }
BOOL ReleaseMutex(HANDLE h) { return lookup(h) != 0; }

void GlobalMemoryStatus(MEMORYSTATUS* out) {
    memset(out, 0, sizeof(*out));
    out->dwLength = sizeof(*out);
    out->dwTotalPhys = out->dwAvailPhys = 256u << 20;
    out->dwTotalPageFile = out->dwAvailPageFile = 512u << 20;
    out->dwTotalVirtual = out->dwAvailVirtual = 1024u << 20;
}

// Reported as Windows 98 (4.10.1998), the platform the game shipped for.
BOOL GetVersionExA(OSVERSIONINFOA* out) {
    if (!out) return FALSE;
    out->dwMajorVersion = 4;
    out->dwMinorVersion = 10;
    out->dwBuildNumber = 1998;
    out->dwPlatformId = 1; // VER_PLATFORM_WIN32_WINDOWS
    out->szCSDVersion[0] = 0;
    return TRUE;
}

void OutputDebugStringA(LPCSTR text) { if (text) printf("[debug] %s", text); }

// The supplied game folder is presented as the game CD: drive D:, label "AOE" (prog_info.vol_name).
UINT GetDriveTypeA(LPCSTR root) {
    return root && (root[0] == 'd' || root[0] == 'D') ? 5 /* DRIVE_CDROM */ : 3 /* DRIVE_FIXED */;
}

BOOL GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_size, LPDWORD serial, LPDWORD max_component,
                           LPDWORD flags, LPSTR fs_name, DWORD fs_size) {
    bool cd = GetDriveTypeA(root) == 5;
    if (name && name_size) { strncpy(name, cd ? "AOE" : "", name_size - 1); name[name_size - 1] = 0; }
    if (serial) *serial = 0;
    if (max_component) *max_component = 255;
    if (flags) *flags = 0;
    if (fs_name && fs_size) { strncpy(fs_name, cd ? "CDFS" : "FAT32", fs_size - 1); fs_name[fs_size - 1] = 0; }
    return TRUE;
}

int _getdrive(void) { return 3; } // C:

void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext) {
    std::string p = path ? path : "", d, rest = p;
    if (p.size() > 1 && p[1] == ':') { d = p.substr(0, 2); rest = p.substr(2); }
    size_t slash = rest.find_last_of("\\/");
    std::string folder = slash == std::string::npos ? "" : rest.substr(0, slash + 1);
    std::string file = slash == std::string::npos ? rest : rest.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    std::string base = dot == std::string::npos ? file : file.substr(0, dot);
    std::string extension = dot == std::string::npos ? "" : file.substr(dot);
    if (drive) strcpy(drive, d.c_str());
    if (dir) strcpy(dir, folder.c_str());
    if (fname) strcpy(fname, base.c_str());
    if (ext) strcpy(ext, extension.c_str());
}

int CompareStringA(LCID, DWORD flags, LPCSTR a, int na, LPCSTR b, int nb) {
    std::string x(a, na < 0 ? strlen(a) : na), y(b, nb < 0 ? strlen(b) : nb);
    int r = (flags & 1 /* NORM_IGNORECASE */) ? strcasecmp(x.c_str(), y.c_str()) : strcmp(x.c_str(), y.c_str());
    return r < 0 ? 1 : r > 0 ? 3 : 2; // CSTR_LESS_THAN / CSTR_GREATER_THAN / CSTR_EQUAL
}

// Launching other programs (password tool, MS Gaming Zone) is not possible in the browser.
UINT WinExec(LPCSTR command, UINT) { printf("[aoe] WinExec ignored: %s\n", command ? command : ""); return 2; }

HRESULT CoInitialize(LPVOID) { return 0; }
void CoUninitialize(void) {}

}

// ---------------------------------------------------------------- PE modules and resources
struct PeModule {
    std::string name;
    std::vector<unsigned char> image;
    struct Section { DWORD rva, size, raw; };
    std::vector<Section> sections;
    DWORD resource_rva;
    const unsigned char* at(DWORD rva, DWORD size) const {
        for (size_t i = 0; i < sections.size(); i++) {
            const Section& s = sections[i];
            if (rva >= s.rva && rva + size <= s.rva + s.size && s.raw + (rva - s.rva) + size <= image.size())
                return &image[s.raw + (rva - s.rva)];
        }
        return 0;
    }
    DWORD u32(DWORD rva) const { const unsigned char* p = at(rva, 4); return p ? p[0] | p[1] << 8 | p[2] << 16 | (DWORD) p[3] << 24 : 0; }
    WORD u16(DWORD rva) const { const unsigned char* p = at(rva, 2); return p ? p[0] | p[1] << 8 : 0; }
};
struct ResourceBlock { PeModule* module; DWORD rva, size; };

static std::vector<PeModule*> modules;
static std::vector<ResourceBlock*> resource_blocks;

static PeModule* load_pe(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    PeModule* m = new PeModule;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    m->image.resize(size > 0 ? size : 0);
    if (size <= 0 || fread(&m->image[0], 1, size, f) != (size_t) size) { fclose(f); delete m; return 0; }
    fclose(f);
    const std::vector<unsigned char>& b = m->image;
    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') { delete m; return 0; }
    DWORD pe = b[60] | b[61] << 8 | b[62] << 16 | (DWORD) b[63] << 24;
    if (pe + 248 > b.size() || memcmp(&b[pe], "PE\0\0", 4)) { delete m; return 0; }
    WORD nsec = b[pe + 6] | b[pe + 7] << 8, optlen = b[pe + 20] | b[pe + 21] << 8;
    DWORD opt = pe + 24;
    m->resource_rva = b[opt + 112] | b[opt + 113] << 8 | b[opt + 114] << 16 | (DWORD) b[opt + 115] << 24;
    for (int i = 0; i < nsec; i++) {
        DWORD s = opt + optlen + i * 40;
        if (s + 40 > b.size()) break;
        PeModule::Section sec;
        DWORD vsize = b[s + 8] | b[s + 9] << 8 | b[s + 10] << 16 | (DWORD) b[s + 11] << 24;
        sec.rva = b[s + 12] | b[s + 13] << 8 | b[s + 14] << 16 | (DWORD) b[s + 15] << 24;
        DWORD rsize = b[s + 16] | b[s + 17] << 8 | b[s + 18] << 16 | (DWORD) b[s + 19] << 24;
        sec.raw = b[s + 20] | b[s + 21] << 8 | b[s + 22] << 16 | (DWORD) b[s + 23] << 24;
        sec.size = rsize < vsize || !vsize ? rsize : vsize;
        m->sections.push_back(sec);
    }
    return m;
}

static std::string upper_ascii(const char* s) { std::string r = s; for (size_t i = 0; i < r.size(); i++) if (r[i] >= 'a' && r[i] <= 'z') r[i] -= 32; return r; }

// Match one resource directory level by integer id or (case-insensitive) name.
static DWORD find_entry(const PeModule* m, DWORD dir, const char* name_or_id) {
    WORD named = m->u16(dir + 12), ids = m->u16(dir + 14);
    bool by_id = (uintptr_t) name_or_id <= 0xffff;
    std::string wanted = by_id ? "" : upper_ascii(name_or_id[0] == '#' ? name_or_id : name_or_id);
    if (!by_id && name_or_id[0] == '#') { by_id = true; name_or_id = (const char*) (uintptr_t) atoi(name_or_id + 1); }
    for (int i = 0; i < named + ids; i++) {
        DWORD e = dir + 16 + i * 8, id = m->u32(e), target = m->u32(e + 4);
        bool match = false;
        if (by_id && !(id & 0x80000000u)) match = id == (DWORD) (uintptr_t) name_or_id;
        if (!by_id && (id & 0x80000000u)) {
            DWORD s = m->resource_rva + (id & 0x7fffffffu);
            WORD len = m->u16(s);
            std::string got;
            for (int k = 0; k < len; k++) { WORD c = m->u16(s + 2 + 2 * k); got += (char) (c < 128 && c >= 'a' && c <= 'z' ? c - 32 : c); }
            match = got == wanted;
        }
        if (match) return target;
    }
    return 0xffffffffu;
}

// Returns the RVA of the IMAGE_RESOURCE_DATA_ENTRY (type/name, first language).
static DWORD find_resource(const PeModule* m, const char* type, const char* name) {
    if (!m || !m->resource_rva) return 0;
    DWORD t = find_entry(m, m->resource_rva, type);
    if (t == 0xffffffffu || !(t & 0x80000000u)) return 0;
    DWORD n = find_entry(m, m->resource_rva + (t & 0x7fffffffu), name);
    if (n == 0xffffffffu || !(n & 0x80000000u)) return 0;
    DWORD lang_dir = m->resource_rva + (n & 0x7fffffffu);
    if (m->u16(lang_dir + 12) + m->u16(lang_dir + 14) == 0) return 0;
    DWORD leaf = m->u32(lang_dir + 16 + 4);
    return leaf & 0x80000000u ? 0 : m->resource_rva + leaf;
}

static PeModule* module_of(HINSTANCE h);
// HINSTANCE 1 is the program itself (EMPIRESX.EXE from the supplied folder).
static PeModule* program_module() {
    static PeModule* program = 0;
    static bool tried = false;
    if (!tried) { tried = true; program = load_pe("empiresx.exe"); if (program) program->name = "empiresx.exe"; }
    return program;
}
static PeModule* module_of(HINSTANCE h) {
    if (h == (HINSTANCE) 1 || h == 0) return program_module();
    for (size_t i = 0; i < modules.size(); i++) if (modules[i] == (PeModule*) h) return modules[i];
    return 0;
}

// Windows-1252 conversion for UTF-16 resource strings.
static unsigned char to_cp1252(WORD c) {
    static const WORD high[32] = {0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
                                  0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178};
    if (c < 0x80 || (c >= 0xa0 && c <= 0xff)) return (unsigned char) c;
    for (int i = 0; i < 32; i++) if (high[i] == c && c) return 0x80 + i;
    return '?';
}

extern "C" {

HMODULE GetModuleHandleA(LPCSTR name) { return name ? 0 : (HMODULE) 1; }

// Only real files from the game folder load; system DLLs (DDRAW, DINPUT, ...) are reported missing.
HMODULE LoadLibraryA(LPCSTR name) {
    std::string path = win_path(name);
    for (size_t i = 0; i < modules.size(); i++) if (modules[i]->name == path) return (HMODULE) modules[i];
    PeModule* m = load_pe(path);
    if (!m) { last_error = 126; return 0; } // ERROR_MOD_NOT_FOUND
    m->name = path;
    modules.push_back(m);
    return (HMODULE) m;
}

BOOL FreeLibrary(HMODULE) { return TRUE; } // modules stay cached for the lifetime of the worker
FARPROC GetProcAddress(HMODULE, LPCSTR) { return 0; }

int LoadStringA(HINSTANCE instance, UINT id, LPSTR out, int capacity) {
    if (!out || capacity <= 0) return 0;
    out[0] = 0;
    PeModule* m = module_of(instance);
    id &= 0xffff;
    DWORD entry = find_resource(m, (const char*) 6 /* RT_STRING */, (const char*) (uintptr_t) (id / 16 + 1));
    if (!entry) return 0;
    DWORD rva = m->u32(entry), size = m->u32(entry + 4), p = rva;
    for (UINT i = 0; i < 16 && p < rva + size; i++) {
        WORD len = m->u16(p);
        if (i == (id & 15)) {
            int n = 0;
            for (; n < len && n < capacity - 1; n++) out[n] = (char) to_cp1252(m->u16(p + 2 + 2 * n));
            out[n] = 0;
            return n;
        }
        p += 2 + 2 * len;
    }
    return 0;
}

HRSRC FindResourceA(HMODULE module, LPCSTR name, LPCSTR type) {
    PeModule* m = module_of(module);
    DWORD entry = find_resource(m, type, name);
    if (!entry) return 0;
    ResourceBlock* r = new ResourceBlock;
    r->module = m;
    r->rva = m->u32(entry);
    r->size = m->u32(entry + 4);
    resource_blocks.push_back(r);
    return (HRSRC) r;
}

HGLOBAL LoadResource(HMODULE, HRSRC resource) { return (HGLOBAL) resource; }

LPVOID LockResource(HGLOBAL resource) {
    ResourceBlock* r = (ResourceBlock*) resource;
    for (size_t i = 0; i < resource_blocks.size(); i++)
        if (resource_blocks[i] == r) return (LPVOID) r->module->at(r->rva, r->size);
    return 0;
}

// Only consulted by the DirectX version check, which is skipped with NODXCHECK.
DWORD GetFileVersionInfoSizeA(LPCSTR, LPDWORD handle) { if (handle) *handle = 0; return 0; }
BOOL GetFileVersionInfoA(LPCSTR, DWORD, DWORD, LPVOID) { return FALSE; }
BOOL VerQueryValueA(LPCVOID, LPCSTR, LPVOID*, UINT*) { return FALSE; }

}

// ---------------------------------------------------------------- registry
// HKEY_CURRENT_USER / HKEY_LOCAL_MACHINE values, seeded with what the installer wrote. Every change is
// written to /aoe-registry.dat, which the host persists with the game's other files.
struct RegValue { DWORD type; std::vector<BYTE> bytes; };
struct RegKey { std::map<std::string, RegValue> values; };
static std::map<std::string, RegKey> registry;

static std::string reg_name(HKEY root, LPCSTR name) {
    char prefix[32];
    sprintf(prefix, "%lu/", (unsigned long) (uintptr_t) root);
    std::string key = std::string(prefix) + (name ? name : "");
    for (size_t i = 0; i < key.size(); i++) if (key[i] >= 'a' && key[i] <= 'z') key[i] -= 32;
    return key;
}

static const char* REGISTRY_FILE = "/aoe-registry.dat";

// Format: one value per line: key \t name \t type \t hex bytes.
static void save_registry() {
    FILE* f = fopen(REGISTRY_FILE, "wb");
    if (!f) return;
    for (std::map<std::string, RegKey>::iterator k = registry.begin(); k != registry.end(); ++k)
        for (std::map<std::string, RegValue>::iterator v = k->second.values.begin(); v != k->second.values.end(); ++v) {
            fprintf(f, "%s\t%s\t%lu\t", k->first.c_str(), v->first.c_str(), (unsigned long) v->second.type);
            for (size_t i = 0; i < v->second.bytes.size(); i++) fprintf(f, "%02x", v->second.bytes[i]);
            fputc('\n', f);
        }
    fclose(f);
}

static void load_registry() {
    FILE* f = fopen(REGISTRY_FILE, "rb");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* fields[4] = {line, 0, 0, 0};
        for (int i = 1; i < 4; i++) { fields[i] = strchr(fields[i - 1], '\t'); if (!fields[i]) break; *fields[i]++ = 0; }
        if (!fields[3]) continue;
        RegValue v;
        v.type = strtoul(fields[2], 0, 10);
        for (char* p = fields[3]; p[0] && p[1] && p[0] != '\n'; p += 2) { char hex[3] = {p[0], p[1], 0}; v.bytes.push_back((BYTE) strtoul(hex, 0, 16)); }
        registry[fields[0]].values[fields[1]] = v;
    }
    fclose(f);
}

static void seed_registry() {
    static bool seeded = false;
    if (seeded) return;
    seeded = true;
    load_registry();
    // HKEY_LOCAL_MACHINE\Software\Microsoft\Games\Age of Empires\1.00: the installer's CD path.
    RegKey& key = registry[reg_name((HKEY) 0x80000002u, "Software\\Microsoft\\Games\\Age of Empires\\1.00")];
    if (key.values.count("CDPATH")) return;
    const char cd[] = "D:\\";
    RegValue v;
    v.type = 1; // REG_SZ
    v.bytes.assign((const BYTE*) cd, (const BYTE*) cd + sizeof(cd));
    key.values["CDPATH"] = v;
}

extern "C" {

LONG RegCreateKeyExA(HKEY root, LPCSTR name, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD disposition) {
    seed_registry();
    std::string key = reg_name(root, name);
    bool exists = registry.count(key) != 0;
    *out = (HKEY) &registry[key];
    if (disposition) *disposition = exists ? 2 : 1;
    return 0;
}

LONG RegOpenKeyExA(HKEY root, LPCSTR name, DWORD, REGSAM, PHKEY out) {
    seed_registry();
    std::map<std::string, RegKey>::iterator it = registry.find(reg_name(root, name));
    if (it == registry.end()) return 2;
    *out = (HKEY) &it->second;
    return 0;
}

LONG RegCloseKey(HKEY) { return 0; }

LONG RegSetValueExA(HKEY h, LPCSTR name, DWORD, DWORD type, const BYTE* data, DWORD size) {
    if (!h) return 87;
    std::string n = name ? name : "";
    for (size_t i = 0; i < n.size(); i++) if (n[i] >= 'a' && n[i] <= 'z') n[i] -= 32;
    RegValue& v = ((RegKey*) h)->values[n];
    if (v.type == type && v.bytes.size() == size && (!size || !memcmp(&v.bytes[0], data, size))) return 0;
    v.type = type;
    v.bytes.assign(data, data + size);
    save_registry();
    return 0;
}

LONG RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (!h || !size) return 87;
    std::string n = name ? name : "";
    for (size_t i = 0; i < n.size(); i++) if (n[i] >= 'a' && n[i] <= 'z') n[i] -= 32;
    RegKey* k = (RegKey*) h;
    std::map<std::string, RegValue>::iterator it = k->values.find(n);
    if (it == k->values.end()) return 2;
    RegValue& v = it->second;
    if (type) *type = v.type;
    DWORD required = v.bytes.size();
    if (data && *size < required) { *size = required; return 234; }
    if (data && required) memcpy(data, &v.bytes[0], required);
    *size = required;
    return 0;
}

}
