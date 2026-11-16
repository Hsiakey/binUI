// guard_alloc.cpp —— "电篱笆"分配器：给每个 C++ 堆块末尾挂一个不可访问页。
//
// 本机 mingw 工具链没有 libasan（`g++ -print-file-name=libasan.a` 只回一个裸名字），
// 也没有管理员权限开 page heap，所以自己做一个够用的替代品：
//
//   · operator new 分配 VirtualAlloc 数据页 + 紧跟一个 PAGE_NOACCESS 守护页，
//     用户指针**从数据页末尾往回退**，越界写立刻 0xC0000005（而不是几小时后的随机崩）；
//   · 末尾到守护页之间最多 15 字节的对齐空档填 0xAB 哨兵 —— 微小越界不会立刻炸，
//     但 free 时能查出来，并打印**这块内存的分配栈**（知道是哪个缓冲区被写坏）；
//   · 表里查不到指针 = 释放了不认识的指针（跨 allocator 混用），也报出来。
//
// 用法（见 Makefile 的 `check-guard` 目标）：
//   g++ ... tools/check_examples.cpp tools/guard_alloc.cpp libbinui.a
// 它只接管 operator new/delete，不动 malloc —— CRT 堆的 malloc/free 必须自洽，
// 混着替换会误伤。因此它查的是 C++ 堆（vector/string/filesystem::path 都在这里）。
//
// 代价：每次分配一对 VirtualAlloc/VirtualFree，比正常堆慢两个数量级（本例几十倍），
// 只适合跑门禁/复现，不适合当常规构建。
#include <cstddef>
#include <cstdint>
#include <new>
#include <windows.h>

namespace {

const unsigned char kSentinel = 0xAB;
const int kFrames = 6;
void* const kDeleted = (void*)(uintptr_t)-1;   // 开放寻址的墓碑

struct Rec {
    void* user;   // 交给调用方的指针
    void* base;   // VirtualAlloc 基址（free 用）
    size_t size;  // 请求的字节数
    void* bt[kFrames];   // 分配栈（模块相对偏移），越界时打印
};

Rec* g_tab = nullptr;
size_t g_cap = 0;   // 2 的幂
size_t g_cnt = 0;

// ---- 不分配的报告通道（崩溃点上不能再用堆）----
void Emit(const char* s) {
    DWORD w = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)lstrlenA(s), &w, nullptr);
}
void EmitHex(unsigned long long v) {
    char b[24];
    int i = 23;
    b[23] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15u]; v >>= 4; } while (v);
    Emit("0x");
    Emit(b + i);
}
void EmitU(unsigned long long v) {
    char b[24];
    int i = 23;
    b[23] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    Emit(b + i);
}
void EmitStack(const char* tag, const void* const* frames) {
    const unsigned long long base = (unsigned long long)(uintptr_t)GetModuleHandleA(nullptr);
    for (int i = 0; i < kFrames; i++) {
        if (!frames[i]) break;
        Emit(tag);
        EmitHex((unsigned long long)(uintptr_t)frames[i] - base);
        Emit("\n");
    }
}

unsigned HashPtr(void* p) {
    uint64_t v = (uint64_t)(uintptr_t)p >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return (unsigned)(v >> 32);
}

void TabGrow() {
    const size_t nc = g_cap ? g_cap * 2 : (size_t)1 << 16;
    Rec* nt = (Rec*)VirtualAlloc(nullptr, nc * sizeof(Rec), MEM_RESERVE | MEM_COMMIT,
                                 PAGE_READWRITE);
    if (!nt) { Emit("GA_NOMEM\n"); ExitProcess(3); }
    for (size_t i = 0; i < g_cap; i++) {
        if (!g_tab[i].user || g_tab[i].user == kDeleted) continue;
        unsigned h = HashPtr(g_tab[i].user) & (unsigned)(nc - 1);
        while (nt[h].user) h = (h + 1) & (unsigned)(nc - 1);
        nt[h] = g_tab[i];
    }
    if (g_tab) VirtualFree(g_tab, 0, MEM_RELEASE);
    g_tab = nt;
    g_cap = nc;
}

Rec* TabFind(void* user) {
    if (!g_cap) return nullptr;
    unsigned h = HashPtr(user) & (unsigned)(g_cap - 1);
    for (size_t probe = 0; probe < g_cap; probe++) {
        if (!g_tab[h].user) return nullptr;
        if (g_tab[h].user == user) return &g_tab[h];
        h = (h + 1) & (unsigned)(g_cap - 1);
    }
    return nullptr;
}

void TabInsert(const Rec& r) {
    if ((g_cnt + 1) * 10 >= g_cap * 7) TabGrow();
    unsigned h = HashPtr(r.user) & (unsigned)(g_cap - 1);
    size_t tomb = (size_t)-1;
    for (;;) {
        if (!g_tab[h].user) {
            if (tomb != (size_t)-1) h = (unsigned)tomb;
            break;
        }
        if (g_tab[h].user == kDeleted && tomb == (size_t)-1) tomb = h;
        h = (h + 1) & (unsigned)(g_cap - 1);
    }
    g_tab[h] = r;
    g_cnt++;
}

size_t PageBytes(size_t n) {
    const size_t pages = (n + 4095) / 4096;
    return (pages ? pages : 1) * 4096;
}

void* GuardNew(size_t n, size_t align) {
    if (align < 16) align = 16;   // operator new 的常规对齐（含 SSE 类型）
    const size_t dataBytes = PageBytes(n);
    void* base = VirtualAlloc(nullptr, dataBytes + 4096, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (!base) { Emit("GA_VIRTUALALLOC_FAIL n="); EmitU(n); Emit("\n"); ExitProcess(4); }
    DWORD old = 0;
    VirtualProtect((char*)base + dataBytes, 4096, PAGE_NOACCESS, &old);

    // 用户指针从数据页末尾往回退 n 字节，再向下对齐到 align：越界写撞守护页
    const uintptr_t end = (uintptr_t)base + dataBytes;
    void* user = (void*)((end - n) & ~(uintptr_t)(align - 1));
    for (const unsigned char* p = (const unsigned char*)user + n;
         p < (const unsigned char*)end; ++p)
        *(unsigned char*)p = kSentinel;

    Rec r{};
    r.user = user;
    r.base = base;
    r.size = n;
    RtlCaptureStackBackTrace(2, kFrames, r.bt, nullptr);
    TabInsert(r);
    return user;
}

void GuardDel(void* user) {
    if (!user) return;
    Rec* r = TabFind(user);
    if (!r || r->user == kDeleted) {
        Emit("GA_FREE_UNKNOWN ptr=");
        EmitHex((unsigned long long)(uintptr_t)user);
        Emit("\n");
        ExitProcess(9);
    }
    // 哨兵检查：末尾空档被写过 = 发生了不超过 15 字节的越界
    const unsigned char* p = (const unsigned char*)user + r->size;
    const unsigned char* end = (const unsigned char*)r->base + PageBytes(r->size);
    bool bad = false;
    size_t off = 0;
    for (const unsigned char* q = p; q < end; ++q, ++off) {
        if (*q != kSentinel) { bad = true; break; }
    }
    if (bad) {
        void* bt[kFrames];
        RtlCaptureStackBackTrace(2, kFrames, bt, nullptr);
        Emit("GA_OVERFLOW size=");
        EmitU(r->size);
        Emit(" firstBadOff=");
        EmitU(off);
        Emit(" ptr=");
        EmitHex((unsigned long long)(uintptr_t)user);
        Emit("\n  alloc-stack:\n");
        EmitStack("  A ", r->bt);
        Emit("  free-stack:\n");
        EmitStack("  F ", bt);
        ExitProcess(9);
    }
    void* base = r->base;
    r->user = kDeleted;
    g_cnt--;
    VirtualFree(base, 0, MEM_RELEASE);   // 立刻归还：读已释放内存也会立刻炸（UAF 检测）
}

} // namespace

// ---- 替换全局 operator new/delete ----
void* operator new(size_t n) { return GuardNew(n, 16); }
void* operator new[](size_t n) { return GuardNew(n, 16); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return GuardNew(n, 16); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return GuardNew(n, 16); }
void* operator new(size_t n, std::align_val_t a) { return GuardNew(n, (size_t)a); }
void* operator new[](size_t n, std::align_val_t a) { return GuardNew(n, (size_t)a); }
void operator delete(void* p) noexcept { GuardDel(p); }
void operator delete[](void* p) noexcept { GuardDel(p); }
void operator delete(void* p, size_t) noexcept { GuardDel(p); }
void operator delete[](void* p, size_t) noexcept { GuardDel(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { GuardDel(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { GuardDel(p); }
void operator delete(void* p, std::align_val_t) noexcept { GuardDel(p); }
void operator delete[](void* p, std::align_val_t) noexcept { GuardDel(p); }
