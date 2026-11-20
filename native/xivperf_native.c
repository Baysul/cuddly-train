/* Native half of the XIVPerf plugin: engine patches that run on game threads, in code that is never unloaded.
 *
 * Havok child table
 * -----------------
 * hkaPose::accessBoneModelSpace(boneIdx, DONT_PROPAGATE) finds the bone's direct children by scanning every later
 * bone's parent index (game 2026.11.03 at 0x141f01580; decompilation and samples in
 * docs/research/client-crowd-performance.md, "The Havok child scan"). Reading a whole skeleton bone by bone is
 * therefore O(bones^2). This replacement does the same work with a per-skeleton table of direct children: the same
 * children, in the same order, with the same calls to calculateBoneModelSpace and the same flag writes. The
 * PROPAGATE case, and anything unusual, goes to the original.
 *
 * Patch: the original's first four instructions (mov rax,rsp / mov [rax+10h],edx / push rbx / push rdi) stay, so
 * the entry bytes that FFXIVClientStructs signature-scans for are untouched. The fifth instruction (sub rsp,0B8h, at
 * +8, 8-byte aligned) becomes a jmp rel32 to a stub that pops rdi and rbx and jumps to the replacement, which then
 * runs as if the caller had called it. The jmp is written with one atomic 8-byte compare-exchange, so threads
 * running the function meanwhile see either the old or the new code. The stub page and this DLL are never freed
 * or rewritten, so a thread still inside them after an uninstall is safe.
 *
 * Struct offsets (FFXIVClientStructs Havok/Animation/Rig/hkaPose.cs, hkaSkeleton.cs):
 *   hkaPose:     +0x00 Skeleton, +0x18 ModelPose.Data (hkQsTransformf, 0x30 bytes each), +0x28 BoneFlags.Data
 *                (uint32 per bone; 1 = BoneLocalDirty, 2 = BoneModelDirty), +0x39 LocalInSync
 *   hkaSkeleton: +0x18 ParentIndices.Data (int16), +0x30 Bones.Length
 *
 * Kernel::Notifier list
 * ---------------------
 * DeviceDX11.PostTick walks the Notifier list twice a frame (EndFrame, then BeginFrame) and calls a virtual on every
 * node. Nearly all nodes are textures and buffers whose callbacks return at once because of flags set at creation
 * (docs/research/engine-patches.md, "Notifier list"). This keeps those nodes off the list. Notifier.Link and
 * Notifier.Unlink are replaced, not wrapped: both are short, verified byte for byte, and reimplemented here exactly
 * under the game's own list lock, with the skip decision added. So no trampoline is needed, and every change to the
 * list and to the set of skipped nodes happens under that one lock.
 *
 * TempAllocator fast slab cache
 * -----------------------------
 * Client.Graphics.Temp.Allocator (AllocatorManager+0x10, an AllocatorHybrid) takes ~156k allocs/s + ~156k frees/s
 * across Main, all 14 JobPool workers (bone-matrix SRB staging in RenderView), and RenderThread (PreprocessCommands).
 * Both its AllocatorPool and AllocatorHeap use spin-0 CRITICAL_SECTIONs, and AllocatorPool.Free holds Pool->Lock
 * while calling AllocatorHeap.Free (nested global locks), causing a 15-thread lock convoy (6.22 ms/frame Main sleep,
 * 5.68 ms/frame in >=5 sleeps/ms bursts, capture 20261119-180344). All calls go virtually through TempAllocator's
 * vtable. We swap its vtable pointer atomically to serve <=16 KiB, <=16-byte-aligned allocations from a 256 MiB
 * VirtualAlloc slab arena using 2-magazine per-thread caches (direct TEB TLS read + FlsAlloc thread-exit cleanup)
 * backed by per-class central batch depots. Ownership is a single unsigned range check; everything else forwards to
 * the game's original vtable.
 *
 * Command sort
 * ------------
 * Device.GatherCommands calls Context.GatherCommands once per context to flatten and stably sort each frame's
 * 16-byte render commands by their unsigned 32-bit key. The original copies every 0x4000-byte block twice and runs
 * a recursive merge sort (FUN_140216550). We redirect that call to xp_gather_commands, which copies once (predicting
 * radix pass parity per context/list) and runs an in-place insertion sort (<64 records) or stable 4-byte LSD radix
 * sort (skipping uniform key bytes), leaving output[0, blocks*1024) byte-for-byte identical.
 *
 * Build: plugin/native/build.sh (mingw-w64). Exports are plain C (Windows x64 calling convention).
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define XP_VERSION 5

enum {
    XP_OK = 0,
    XP_ERR_ARGS = -1,
    XP_ERR_BYTES = -2,        /* not the code this replacement mirrors (patched by someone else, or a new build) */
    XP_ERR_CALC = -3,         /* the given calculateBoneModelSpace differs from the one the function calls */
    XP_ERR_NEAR = -4,         /* no free memory within +-2 GB for the stub */
    XP_ERR_PROTECT = -5,
    XP_ERR_RACE = -6,         /* the bytes changed while patching */
    XP_ERR_STATE = -7,        /* already installed / not installed / a different function than before */
    XP_ERR_GLOBALS = -8,      /* Notifier.Link and Unlink disagree on the list lock or head */
    XP_ERR_WAIT = -9,         /* too soon after installing; try again next frame */
    XP_ERR_MEMORY = -10,
    XP_ERR_CHAIN = -11,       /* Allocator vtable chain depth limit reached */
};

__declspec(dllexport) int xp_version(void) { return XP_VERSION; }

/* ---- shared helpers ---- */
static int matches(const uint8_t *code, const int16_t *expected, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (expected[i] >= 0 && code[i] != (uint8_t)expected[i])
            return 0;
    return 1;
}

static inline uint8_t *rel32_target(uint8_t *insn_end, const uint8_t *rel)
{
    int32_t d;
    memcpy(&d, rel, 4);
    return insn_end + d;
}

static void *alloc_near(uint8_t *target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t t = (uintptr_t)target;
    uintptr_t lo = t > 0x7ff00000u ? t - 0x7ff00000u : gran;
    uintptr_t hi = t + 0x7ff00000u;
    /* Upward from the target, then downward. */
    for (uintptr_t a = (t + gran) & ~(gran - 1); a < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)))
            break;
        uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            uintptr_t start = ((uintptr_t)mbi.BaseAddress + gran - 1) & ~(gran - 1);
            if (start + size <= end && start + size < hi) {
                void *p = VirtualAlloc((void *)start, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p)
                    return p;
            }
        }
        a = (end + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (t & ~(gran - 1)) - gran; a > lo; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)))
            break;
        if (mbi.State == MEM_FREE) {
            void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p)
                return p;
        } else if ((uintptr_t)mbi.AllocationBase <= lo) {
            break;                                                      /* the rest of the range is this allocation */
        } else if ((uintptr_t)mbi.AllocationBase < a) {
            a = ((uintptr_t)mbi.AllocationBase & ~(gran - 1)) + gran;   /* the loop steps below the allocation */
        }
    }
    return NULL;
}

static int near_rel32(const uint8_t *insn_end, const uint8_t *dest, int32_t *out)
{
    int64_t rel = (int64_t)(dest - insn_end);
    if (rel != (int32_t)rel)
        return 0;
    *out = (int32_t)rel;
    return 1;
}

static int write_qword(uint8_t *at, uint64_t expected, uint64_t value)
{
    DWORD old;
    if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old))
        return XP_ERR_PROTECT;
    LONG64 prev = InterlockedCompareExchange64((volatile LONG64 *)at, (LONG64)value, (LONG64)expected);
    VirtualProtect(at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 8);
    return (uint64_t)prev == expected ? XP_OK : XP_ERR_RACE;
}

static uint64_t qword_of(const int16_t *bytes)   /* 8 concrete pattern bytes */
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)bytes[i];
    uint64_t q;
    memcpy(&q, b, 8);
    return q;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "40 53 ?? ..." into bytes, -1 for ??; returns the length or -1. */
static int parse_pattern(const char *s, int16_t *out, int max)
{
    int n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (n >= max)
            return -1;
        if (s[0] == '?' && s[1] == '?') {
            out[n++] = -1;
        } else {
            int hi = hexval(s[0]), lo = s[0] ? hexval(s[1]) : -1;
            if (hi < 0 || lo < 0)
                return -1;
            out[n++] = (int16_t)(hi << 4 | lo);
        }
        s += 2;
    }
    return n;
}

/* ======================================================================================== Havok child table == */

typedef void *(*access_fn)(void *pose, int bone, int propagate);
typedef void *(*calc_fn)(void *pose, int bone);

static inline void *pose_skeleton(void *pose) { return *(void **)((uint8_t *)pose + 0x00); }
static inline uint8_t *pose_model(void *pose) { return *(uint8_t **)((uint8_t *)pose + 0x18); }
static inline uint32_t *pose_flags(void *pose) { return *(uint32_t **)((uint8_t *)pose + 0x28); }
static inline void pose_clear_local_in_sync(void *pose) { *((volatile uint8_t *)pose + 0x39) = 0; }
static inline const int16_t *skel_parents(const void *skel) { return *(const int16_t **)((const uint8_t *)skel + 0x18); }
static inline int32_t skel_bones(const void *skel) { return *(const int32_t *)((const uint8_t *)skel + 0x30); }

/* Statistics: plain increments, approximate when several threads call at once (a count can even step back). */
typedef struct XpStats {
    uint64_t fast;         /* DONT_PROPAGATE calls served from the table */
    uint64_t fallback;     /* DONT_PROPAGATE calls sent to the original (no table, odd input, stale table) */
    uint64_t propagate;    /* PROPAGATE calls, always the original */
    uint64_t verified;     /* fast calls whose children were checked against the scan */
    uint64_t mismatches;   /* checks that disagreed (exact count); those calls went to the original */
    uint64_t tables;       /* tables built */
    uint64_t rebuilds;     /* tables replaced because the skeleton at that address changed */
    uint64_t bytes;        /* memory held by tables, live and retired-but-not-yet-freed */
    uint64_t evictions;    /* tables of other skeletons replaced because their probe window was full */
    int32_t installed;
    int32_t verify;
    int32_t last_error;
    int32_t version;
} XpStats;

static XpStats g_stats;
static volatile LONG g_epoch = 1;
static volatile LONG g_verify;

typedef struct ChildTable {
    const void *skeleton;
    const int16_t *parents;   /* ParentIndices.Data when built */
    int32_t bones;
    uint32_t checksum;        /* of the parents the table was built from */
    volatile LONG checked_epoch;
    LONG retired_epoch;
    uint32_t bytes;
    uint16_t *first;          /* children of bone b: child[first[b] .. first[b+1]) */
    int16_t *child;
    struct ChildTable *retired_next;
} ChildTable;

#define SLOT_BITS 12
#define SLOTS (1u << SLOT_BITS)
#define MAX_PROBE 32
#define MAX_BYTES (64u << 20)
/* A replaced table is freed this many frames later. A reader holds a table only within one call of the replacement
 * (no locks, no waits, only calculateBoneModelSpace), so a minute of frames is far more than any call takes. */
#define RETIRE_EPOCHS 3600

static ChildTable *volatile g_slots[SLOTS];
static ChildTable *g_retired;   /* newest first */
static SRWLOCK g_lock = SRWLOCK_INIT;

static inline unsigned slot_of(const void *skel)
{
    uint64_t x = (uint64_t)(uintptr_t)skel;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return (unsigned)(x >> (64 - SLOT_BITS));
}

static uint32_t checksum16(const int16_t *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h ^= (uint16_t)p[i];
        h *= 16777619u;
    }
    return h;
}

/* Children of p are the bones c > p with parents[c] == p, in ascending order, exactly the bones the original's
 * loop (c from p+1 to bones-1) visits. A parent index that is negative, not below its child, or out of range never
 * matches a valid boneIdx in the original either. Works from one copy of the parent array, so the counts and the
 * fill always agree. */
static ChildTable *build_table(const void *skel, const int16_t *parents, int n)
{
    size_t bytes = sizeof(ChildTable) + sizeof(uint16_t) * (size_t)(n + 1) + sizeof(int16_t) * (size_t)n;
    if (g_stats.bytes + bytes > MAX_BYTES)
        return NULL;
    HANDLE heap = GetProcessHeap();
    ChildTable *t = (ChildTable *)HeapAlloc(heap, HEAP_ZERO_MEMORY, bytes);
    uint16_t *scratch = (uint16_t *)HeapAlloc(heap, 0, sizeof(uint16_t) * 2 * (size_t)n);
    if (!t || !scratch) {
        if (t) HeapFree(heap, 0, t);
        if (scratch) HeapFree(heap, 0, scratch);
        return NULL;
    }
    uint16_t *cursor = scratch;
    int16_t *snap = (int16_t *)(scratch + n);
    memcpy(snap, parents, sizeof(int16_t) * (size_t)n);
    t->first = (uint16_t *)(t + 1);
    t->child = (int16_t *)(t->first + n + 1);
    for (int c = 1; c < n; c++) {
        int p = snap[c];
        if (p >= 0 && p < c)
            t->first[p + 1]++;
    }
    for (int b = 0; b < n; b++) {
        t->first[b + 1] = (uint16_t)(t->first[b + 1] + t->first[b]);
        cursor[b] = t->first[b];
    }
    for (int c = 1; c < n; c++) {
        int p = snap[c];
        if (p >= 0 && p < c)
            t->child[cursor[p]++] = (int16_t)c;
    }
    t->skeleton = skel;
    t->parents = parents;
    t->bones = n;
    t->checksum = checksum16(snap, n);
    t->checked_epoch = g_epoch;
    t->bytes = (uint32_t)bytes;
    HeapFree(heap, 0, scratch);
    g_stats.bytes += bytes;
    g_stats.tables++;
    return t;
}

static inline int table_current(ChildTable *t, const int16_t *parents, int n)
{
    if (t->parents != parents || t->bones != n)
        return 0;
    LONG epoch = g_epoch;
    if (t->checked_epoch == epoch)
        return 1;
    /* Once per frame per table: a skeleton freed and another loaded at the same address must not reuse it. */
    if (checksum16(parents, n) != t->checksum)
        return 0;
    t->checked_epoch = epoch;
    return 1;
}

/* Under the lock. */
static void retire_locked(ChildTable *t)
{
    t->retired_epoch = g_epoch;
    t->retired_next = g_retired;
    g_retired = t;
}

/* Under the lock. */
static void free_retired_locked(void)
{
    LONG epoch = g_epoch;
    for (ChildTable **pp = &g_retired; *pp;) {
        ChildTable *t = *pp;
        if ((LONG)(epoch - t->retired_epoch) > RETIRE_EPOCHS) {
            *pp = t->retired_next;
            g_stats.bytes -= t->bytes;
            HeapFree(GetProcessHeap(), 0, t);
        } else {
            pp = &t->retired_next;
        }
    }
}

/* Under the lock: find or (re)build the table for skel. Slots are never emptied (lookups stop at an empty slot): a
 * full probe window gives its least recently validated table (a skeleton no longer in use) to the new skeleton. */
static ChildTable *get_table_slow(const void *skel, const int16_t *parents, int n)
{
    ChildTable *result = NULL;
    AcquireSRWLockExclusive(&g_lock);
    unsigned s = slot_of(skel), victim = SLOTS;
    LONG oldest = -1, epoch = g_epoch;
    for (unsigned i = 0; i < MAX_PROBE; i++) {
        unsigned k = (s + i) & (SLOTS - 1);
        ChildTable *t = g_slots[k];
        if (t == NULL) {
            victim = k;
            break;
        }
        if (t->skeleton == skel) {
            if (table_current(t, parents, n))
                result = t;
            else
                victim = k;
            break;
        }
        LONG age = (LONG)(epoch - t->checked_epoch);
        if (age > oldest) {
            oldest = age;
            victim = k;
        }
    }
    if (!result && victim < SLOTS) {
        free_retired_locked();
        result = build_table(skel, parents, n);
        if (result) {
            ChildTable *old = g_slots[victim];
            __atomic_store_n(&g_slots[victim], result, __ATOMIC_RELEASE);
            if (old) {
                retire_locked(old);   /* other threads may still be reading it */
                if (old->skeleton == skel)
                    g_stats.rebuilds++;
                else
                    g_stats.evictions++;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

static inline ChildTable *get_table(const void *skel, const int16_t *parents, int n)
{
    unsigned s = slot_of(skel);
    for (unsigned i = 0; i < MAX_PROBE; i++) {
        ChildTable *t = __atomic_load_n(&g_slots[(s + i) & (SLOTS - 1)], __ATOMIC_ACQUIRE);
        if (t == NULL)
            break;
        if (t->skeleton == skel) {
            if (table_current(t, parents, n))
                return t;
            break;
        }
    }
    return get_table_slow(skel, parents, n);
}

static access_fn g_original;   /* trampoline: the original's first instructions, then a jump back into it */
static calc_fn g_calc;         /* hkaPose::calculateBoneModelSpace */

static int children_match_scan(const ChildTable *t, const int16_t *parents, int n, int bone)
{
    int k = t->first[bone], end = t->first[bone + 1];
    for (int c = bone + 1; c < n; c++) {
        if (parents[c] == bone) {
            if (k >= end || t->child[k] != c)
                return 0;
            k++;
        }
    }
    return k == end;
}

/* Exported only so that profiles can name it. */
__declspec(dllexport) void *xp_havok_access_bone_model_space(void *pose, int bone, int propagate)
{
    if (propagate != 0) {
        g_stats.propagate++;
        return g_original(pose, bone, propagate);
    }
    void *skel = pose_skeleton(pose);
    int n = skel ? skel_bones(skel) : 0;
    const int16_t *parents = skel ? skel_parents(skel) : NULL;
    if (n <= 0 || n > 32767 || (unsigned)bone >= (unsigned)n || parents == NULL) {
        g_stats.fallback++;
        return g_original(pose, bone, propagate);
    }
    ChildTable *t = get_table(skel, parents, n);
    if (t == NULL) {
        g_stats.fallback++;
        return g_original(pose, bone, propagate);
    }
    if (g_verify) {
        if (!children_match_scan(t, parents, n, bone)) {
            InterlockedIncrement64((volatile LONG64 *)&g_stats.mismatches);
            return g_original(pose, bone, propagate);
        }
        g_stats.verified++;
    }
    /* Cheap staleness check, before any side effect: every listed child still names this bone as its parent. A
     * table found stale is revalidated (and rebuilt) on its next use instead of next frame. */
    for (int i = t->first[bone], end = t->first[bone + 1]; i < end; i++) {
        if (parents[t->child[i]] != bone) {
            t->checked_epoch = 0;
            g_stats.fallback++;
            return g_original(pose, bone, propagate);
        }
    }
    g_stats.fast++;
    /* The original's DONT_PROPAGATE loop, over the bone's children only. */
    for (int i = t->first[bone], end = t->first[bone + 1]; i < end; i++) {
        int c = t->child[i];
        if (pose_flags(pose)[c] & 2)
            g_calc(pose, c);
        pose_flags(pose)[c] = 1;
        pose_clear_local_in_sync(pose);
    }
    /* The original's tail, for the bone itself. */
    if (pose_flags(pose)[bone] & 2)
        g_calc(pose, bone);
    pose_flags(pose)[bone] = 1;
    pose_clear_local_in_sync(pose);
    return pose_model(pose) + (size_t)bone * 0x30;
}

/* 2026.11.03 bytes of the DONT_PROPAGATE path: entry to `jmp tail` (0x8a bytes) and the tail (0x4d bytes). -1 marks
 * the rel32 of a call or jump, checked separately. The replacement mirrors exactly this code. */
static const int16_t k_head[0x8a] = {
    0x48, 0x8b, 0xc4, 0x89, 0x50, 0x10, 0x53, 0x57, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00, 0x48,
    0x89, 0x68, 0x18, 0x48, 0x8b, 0xd9, 0x48, 0x89, 0x70, 0xe8, 0x48, 0x8b, 0x01, 0x4c, 0x63, 0xca,
    0x45, 0x85, 0xc0, 0x75, 0x65, 0x48, 0x63, 0x68, 0x30, 0x41, 0x8d, 0x71, 0x01, 0x48, 0x63, 0xfe,
    0x48, 0x3b, 0xfd, 0x0f, 0x8d,   -1,   -1,   -1,   -1, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0x03, 0x48, 0x8b, 0x48, 0x18, 0x0f, 0xbf, 0x04, 0x79, 0x41, 0x3b, 0xc1, 0x75, 0x2b,
    0x48, 0x8b, 0x43, 0x28, 0xf6, 0x04, 0xb8, 0x02, 0x74, 0x12, 0x8b, 0xd6, 0x48, 0x8b, 0xcb, 0xe8,
      -1,   -1,   -1,   -1, 0x44, 0x8b, 0x8c, 0x24, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x43, 0x28,
    0xc7, 0x04, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x43, 0x39, 0x00, 0xff, 0xc6, 0x48, 0xff, 0xc7,
    0x48, 0x3b, 0xfd, 0x7c, 0xbb, 0xe9,   -1,   -1,   -1,   -1,
};
static const int16_t k_tail[0x4d] = {
    0x48, 0x8b, 0x43, 0x28, 0x48, 0x8b, 0xb4, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xac, 0x24,
    0xe0, 0x00, 0x00, 0x00, 0x49, 0x63, 0xf9, 0xf6, 0x04, 0xb8, 0x02, 0x74, 0x0b, 0x41, 0x8b, 0xd1,
    0x48, 0x8b, 0xcb, 0xe8,   -1,   -1,   -1,   -1, 0x48, 0x8b, 0x43, 0x28, 0xc7, 0x04, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x48, 0x8d, 0x04, 0x7f, 0x48, 0xc1, 0xe0, 0x04, 0xc6, 0x43, 0x39, 0x00, 0x48,
    0x03, 0x43, 0x18, 0x48, 0x81, 0xc4, 0xb8, 0x00, 0x00, 0x00, 0x5f, 0x5b, 0xc3,
};
#define PATCH_AT 8            /* sub rsp,0B8h */
#define TRAMPOLINE_COPY 0x13  /* through mov [rax+18h],rbp: whole, position-independent instructions */

static uint8_t *g_target;     /* accessBoneModelSpace while installed */
static uint8_t *g_page;       /* stub and trampoline: written once, never freed */
static uint8_t *g_page_for;   /* the function g_page was written for */
static uint64_t g_patched;
static int g_installed;

/* target: hkaPose::accessBoneModelSpace. calc: hkaPose::calculateBoneModelSpace as resolved by the caller (checked
 * against the call inside the function), or NULL to take it from there. */
__declspec(dllexport) int xp_havok_install(void *target, void *calc)
{
    int r;
    AcquireSRWLockExclusive(&g_lock);
    if (g_installed) { r = XP_ERR_STATE; goto out; }
    uint8_t *f = (uint8_t *)target;
    if (!f || ((uintptr_t)(f + PATCH_AT) & 7)) { r = XP_ERR_ARGS; goto out; }
    if (g_page && g_page_for != f) { r = XP_ERR_STATE; goto out; }
    if (!matches(f, k_head, sizeof(k_head) / sizeof(k_head[0]))) { r = XP_ERR_BYTES; goto out; }
    uint8_t *tail = rel32_target(f + 0x8a, f + 0x86);
    if (!matches(tail, k_tail, sizeof(k_tail) / sizeof(k_tail[0]))) { r = XP_ERR_BYTES; goto out; }
    uint8_t *calc_loop = rel32_target(f + 0x64, f + 0x60);
    uint8_t *calc_tail = rel32_target(tail + 0x28, tail + 0x24);
    if (calc_loop != calc_tail || (calc && (uint8_t *)calc != calc_loop)) { r = XP_ERR_CALC; goto out; }

    uint8_t *site = f + PATCH_AT;
    if (!g_page) {
        uint8_t *page = (uint8_t *)alloc_near(f, 4096);
        int32_t probe;
        if (!page || !near_rel32(site + 5, page, &probe)) { r = XP_ERR_NEAR; goto out; }
        /* stub (page+0):  pop rdi; pop rbx; jmp [rip+0]; dq access_bone_model_space
         * tramp (page+64): the original's first 0x13 bytes; jmp [rip+0]; dq target+0x13 */
        uint8_t *stub = page, *tramp = page + 64;
        uint64_t dest = (uint64_t)(uintptr_t)&xp_havok_access_bone_model_space;
        stub[0] = 0x5f; stub[1] = 0x5b;
        stub[2] = 0xff; stub[3] = 0x25; memset(stub + 4, 0, 4); memcpy(stub + 8, &dest, 8);
        memcpy(tramp, f, TRAMPOLINE_COPY);
        uint64_t back = (uint64_t)(uintptr_t)(f + TRAMPOLINE_COPY);
        tramp[TRAMPOLINE_COPY] = 0xff; tramp[TRAMPOLINE_COPY + 1] = 0x25; memset(tramp + TRAMPOLINE_COPY + 2, 0, 4);
        memcpy(tramp + TRAMPOLINE_COPY + 6, &back, 8);
        DWORD old;
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), page, 4096);
        g_original = (access_fn)(void *)tramp;
        g_calc = (calc_fn)(void *)calc_loop;
        g_page_for = f;
        g_page = page;
    }
    int32_t rel32;
    if (!near_rel32(site + 5, g_page, &rel32)) { r = XP_ERR_NEAR; goto out; }
    uint64_t saved = qword_of(k_head + PATCH_AT);   /* the analysed bytes, not a second read of the live ones */
    uint8_t bytes[8];
    memcpy(bytes, &saved, 8);
    bytes[0] = 0xe9;
    memcpy(bytes + 1, &rel32, 4);   /* bytes 5-7 keep the original's (unreachable after the jmp) */
    memcpy(&g_patched, bytes, 8);
    r = write_qword(site, saved, g_patched);
    if (r == XP_OK) {
        g_target = f;
        g_installed = 1;
    }
out:
    g_stats.installed = g_installed;
    g_stats.last_error = r;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

__declspec(dllexport) int xp_havok_uninstall(void)
{
    int r;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_installed) {
        r = XP_ERR_STATE;
    } else {
        r = write_qword(g_target + PATCH_AT, g_patched, qword_of(k_head + PATCH_AT));
        if (r == XP_OK)
            g_installed = 0;
    }
    g_stats.installed = g_installed;
    g_stats.last_error = r;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

__declspec(dllexport) void xp_havok_set_verify(int on) { InterlockedExchange(&g_verify, on ? 1 : 0); }

/* Once per frame from the main thread: tables are revalidated against their skeleton once per epoch, and replaced
 * tables are freed RETIRE_EPOCHS later (also when tables are built). */
__declspec(dllexport) void xp_new_frame(void)
{
    LONG epoch = InterlockedIncrement(&g_epoch);
    if ((epoch & 1023) == 0 && g_retired && TryAcquireSRWLockExclusive(&g_lock)) {
        free_retired_locked();
        ReleaseSRWLockExclusive(&g_lock);
    }
}

__declspec(dllexport) void xp_get_stats(XpStats *out)
{
    if (!out)
        return;
    XpStats s = g_stats;
    s.verify = g_verify;
    s.version = XP_VERSION;
    *out = s;
}

/* ==================================================================================== Kernel::Notifier list == */

/* A node is the Kernel::Notifier base of a resource: +0x00 vtable (+0x08 BeginFrame, +0x10 EndFrame), +0x08 Next
 * (the newer node; 0 at the head), +0x10 Prev (the older node). g_Notifier_ListHead is the newest node, and the
 * walks follow Prev. */
typedef struct Node {
    void **vtbl;
    struct Node *next;
    struct Node *prev;
} Node;

enum { S_UNKNOWN, S_NOP, S_SB_BEGIN, S_SB_END, S_VB_BEGIN, S_VB_END, S_IB_BEGIN, S_IB_END, S_TEX_BEGIN, S_TEX_END,
       S_CB_END };
enum { G_ACTIVE, G_ALWAYS, G_BUFFER, G_INDEX, G_TEXTURE, G_CONSTANT };

/* Notifier.Link (0x140216750, 0x57 bytes) and Notifier.Unlink (0x1402167b0, 0x75 bytes), 2026.11.03, RIP-relative
 * fields wildcarded. Generated from the analysed bytes; see docs/research/engine-patches.md. */
static const char k_link_text[] =
    "40 53 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 33 C9 48 "
    "85 C0 74 11 48 89 58 08 48 8B 05 ?? ?? ?? ?? 48 89 43 10 EB 04 48 89 4B 10 48 89 4B 08 48 8D 0D "
    "?? ?? ?? ?? 48 89 1D ?? ?? ?? ?? 48 83 C4 20 5B 48 FF 25 ?? ?? ?? ??";
static const char k_unlink_text[] =
    "40 53 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 4B 08 33 D2 48 8B 43 10 "
    "48 85 C9 74 06 48 89 41 10 EB 1E 48 85 C0 74 09 48 89 05 ?? ?? ?? ?? EB 10 48 39 1D ?? ?? ?? ?? "
    "75 20 48 89 15 ?? ?? ?? ?? 48 8B 4B 10 48 85 C9 74 08 48 8B 43 08 48 89 41 08 48 89 53 08 48 89 "
    "53 10 48 8D 0D ?? ?? ?? ?? 48 83 C4 20 5B 48 FF 25 ?? ?? ?? ??";

/* BeginFrame (vf1) and EndFrame (vf2) of the shader-resource buffer (0x14021e040/0x14021e0d0), VertexBuffer
 * (0x14021e7b0/0x14021e840), IndexBuffer (0x14021eee0/0x14021ef70), TextureDx11 (0x14021a600/0x14021a710) and
 * ConstantBuffer EndFrame (0x14021f7c0), whole functions with RIP-relative fields and call targets wildcarded. Each
 * returns at once, with no writes and no D3D calls, when its gate (node_idle) fails. A slot that is not one of these
 * (or `ret`, `ret 0`) makes its node active: never skipped. */
static const struct { int slot; const char *code; } k_slot_text[] = {
    { S_SB_BEGIN,
      "40 53 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 54 C1 20 48 85 D2 74 61 8B 49 1C F6 C1 11 74 "
      "59 8B C1 48 89 7C 24 50 83 E0 01 4C 8D 44 24 30 F6 C1 10 4C 89 44 24 28 44 8D 0C 45 02 00 00 00 "
      "B8 01 00 00 00 44 0F 45 C8 48 8B 05 ?? ?? ?? ?? 33 FF 45 33 C0 89 7C 24 20 48 8B 88 B0 0A 0E 00 "
      "48 8B 01 FF 50 70 48 8B 4C 24 30 85 C0 48 0F 48 CF 48 8B 7C 24 50 48 89 4B 70 48 83 C4 40 5B C3" },
    { S_SB_END,
      "40 53 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 54 C1 20 48 85 D2 74 2C F6 41 1C 11 74 26 48 "
      "83 79 70 00 74 1F 48 8B 05 ?? ?? ?? ?? 45 33 C0 48 8B 88 B0 0A 0E 00 48 8B 01 FF 50 78 48 C7 43 "
      "70 00 00 00 00 48 83 C4 20 5B C3" },
    { S_VB_BEGIN,
      "40 53 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 54 C1 20 48 85 D2 74 61 8B 49 1C F6 C1 11 74 "
      "59 8B C1 48 89 7C 24 50 83 E0 01 4C 8D 44 24 30 F6 C1 10 4C 89 44 24 28 44 8D 0C 45 02 00 00 00 "
      "B8 01 00 00 00 44 0F 45 C8 48 8B 05 ?? ?? ?? ?? 33 FF 45 33 C0 89 7C 24 20 48 8B 88 B0 0A 0E 00 "
      "48 8B 01 FF 50 70 48 8B 4C 24 30 85 C0 48 0F 48 CF 48 8B 7C 24 50 48 89 4B 40 48 83 C4 40 5B C3" },
    { S_VB_END,
      "40 53 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 54 C1 20 48 85 D2 74 2C F6 41 1C 11 74 26 48 "
      "83 79 40 00 74 1F 48 8B 05 ?? ?? ?? ?? 45 33 C0 48 8B 88 B0 0A 0E 00 48 8B 01 FF 50 78 48 C7 43 "
      "40 00 00 00 00 48 83 C4 20 5B C3" },
    { S_IB_BEGIN,
      "40 53 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 4C 8B 54 C1 28 4D 85 D2 74 69 8B 51 20 F6 C2 11 74 "
      "61 F6 C2 40 75 5C 8B C2 48 89 7C 24 50 83 E0 01 F6 C2 10 48 8D 54 24 30 48 89 54 24 28 49 8B D2 "
      "44 8D 0C 45 02 00 00 00 B8 01 00 00 00 44 0F 45 C8 48 8B 05 ?? ?? ?? ?? 33 FF 45 33 C0 89 7C 24 "
      "20 48 8B 88 B0 0A 0E 00 48 8B 01 FF 50 70 48 8B 4C 24 30 85 C0 48 0F 48 CF 48 8B 7C 24 50 48 89 "
      "4B 48 48 83 C4 40 5B C3" },
    { S_IB_END,
      "40 53 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 54 C1 28 48 85 D2 74 31 8B 41 20 A8 11 74 2A "
      "A8 40 75 26 48 83 79 48 00 74 1F 48 8B 05 ?? ?? ?? ?? 45 33 C0 48 8B 88 B0 0A 0E 00 48 8B 01 FF "
      "50 78 48 C7 43 48 00 00 00 00 48 83 C4 20 5B C3" },
    { S_TEX_BEGIN,
      "40 53 48 83 EC 40 8B 41 3C 48 8B D9 25 10 00 10 00 3D 10 00 10 00 0F 85 E6 00 00 00 0F B6 41 34 "
      "48 89 6C 24 50 4C 89 74 24 68 45 33 F6 41 8B EE 84 C0 0F 84 C0 00 00 00 0F B6 49 40 48 89 74 24 "
      "58 48 89 7C 24 60 66 66 0F 1F 84 00 00 00 00 00 41 8B F6 84 C9 0F 84 86 00 00 00 0F 1F 44 00 00 "
      "48 8B 05 ?? ?? ?? ?? 4C 8D 44 24 30 0F B6 7B 34 41 B9 01 00 00 00 8B 15 ?? ?? ?? ?? 4C 89 44 24 "
      "28 48 8B 88 B0 0A 0E 00 0F AF FE 48 8B 54 D3 68 44 89 74 24 20 48 8B 01 03 FD 44 8B C7 FF 50 70 "
      "8B 44 24 38 FF C6 48 8B 8B 98 00 00 00 8B D7 48 03 D2 89 04 D1 48 8B 8B 98 00 00 00 8B 44 24 3C "
      "89 44 D1 04 48 8B 8B 98 00 00 00 48 8B 44 24 30 48 89 44 D1 08 0F B6 4B 40 3B F1 7C 83 0F B6 43 "
      "34 0F B6 C0 FF C5 3B E8 0F 8C 62 FF FF FF 48 8B 7C 24 60 48 8B 74 24 58 48 8B 6C 24 50 4C 8B 74 "
      "24 68 48 83 C4 40 5B C3" },
    { S_TEX_END,
      "48 8B C4 53 48 83 EC 60 48 83 79 48 00 48 8B D9 0F 84 03 02 00 00 8B 51 3C 48 89 68 08 48 89 70 "
      "10 48 89 78 18 4C 89 68 E8 8B C2 25 10 00 10 00 45 33 ED 3D 10 00 10 00 0F 85 8C 00 00 00 0F B6 "
      "49 34 41 8B ED 84 C9 0F 84 7D 00 00 00 0F B6 D1 41 8B F5 40 38 73 40 76 65 0F B6 D1 0F 1F 40 00 "
      "48 8B 83 98 00 00 00 44 0F B6 C1 44 0F AF C6 44 03 C5 48 85 C0 74 3D 41 8B F8 48 03 FF 44 39 2C "
      "F8 74 31 48 8B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 48 8B 88 B0 0A 0E 00 48 8B 54 D3 68 48 8B 01 FF "
      "50 78 48 8B 83 98 00 00 00 44 89 2C F8 0F B6 4B 34 0F B6 D1 0F B6 43 40 FF C6 3B F0 7C A2 FF C5 "
      "0F B6 C2 3B E8 7C 89 8B 53 3C 0F BA E2 0D 0F 83 2E 01 00 00 0F B6 6B 34 F6 C2 10 4C 89 64 24 58 "
      "B8 04 00 00 00 41 BC 01 00 00 00 4C 89 74 24 48 44 0F 44 E0 41 8B F5 48 8B 05 ?? ?? ?? ?? 4C 8B "
      "B0 B0 0A 0E 00 85 ED 0F 84 EB 00 00 00 49 8B FD 4C 89 7C 24 40 66 66 66 0F 1F 84 00 00 00 00 00 "
      "48 8B 83 90 00 00 00 4C 39 6C 07 08 0F 84 B3 00 00 00 B8 AB AA AA AA 48 8D 4B E0 F7 E6 C1 EA 02 "
      "8D 04 52 8B D6 03 C0 2B D0 E8 ?? ?? ?? ?? 48 8B 53 48 48 8D 4C 24 30 48 89 4C 24 28 45 8B CC 44 "
      "8B F8 44 8B C6 49 8B 06 49 8B CE 44 89 6C 24 20 FF 50 70 85 C0 78 29 48 8B 93 90 00 00 00 45 8B "
      "C7 48 8B 4C 24 30 48 8B 54 17 08 E8 ?? ?? ?? ?? 49 8B 06 44 8B C6 48 8B 53 48 49 8B CE FF 50 78 "
      "48 8B 83 90 00 00 00 44 89 2C 07 48 8B 83 90 00 00 00 44 89 6C 07 04 48 8B 83 90 00 00 00 48 8B "
      "54 07 08 48 85 D2 74 1D 48 8B 05 ?? ?? ?? ?? 48 8B 48 10 48 8B 01 FF 50 20 48 8B 83 90 00 00 00 "
      "4C 89 6C 07 08 FF C6 48 83 C7 10 3B F5 0F 82 2D FF FF FF 4C 8B 7C 24 40 4C 8B 64 24 58 4C 8B 74 "
      "24 48 48 8B BC 24 80 00 00 00 48 8B 74 24 78 48 8B 6C 24 70 4C 8B 6C 24 50 48 83 C4 60 5B C3" },
    { S_CB_END,
      "48 89 5C 24 10 57 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 7C C1 18 48 85 FF 74 7C 48 83 79 "
      "30 00 74 75 F7 41 EC 00 40 00 00 74 6C 48 8B 05 ?? ?? ?? ?? 48 8D 4C 24 30 48 89 4C 24 28 41 B9 "
      "04 00 00 00 48 89 74 24 50 45 33 C0 48 8B D7 C7 44 24 20 00 00 00 00 48 8B B0 B0 0A 0E 00 48 8B "
      "CE 48 8B 06 FF 50 70 85 C0 78 21 44 8B 43 FC 48 8B 53 30 48 8B 4C 24 30 E8 ?? ?? ?? ?? 48 8B 06 "
      "45 33 C0 48 8B D7 48 8B CE FF 50 78 48 8B 74 24 50 48 C7 43 30 00 00 00 00 48 8B 5C 24 58 48 83 "
      "C4 40 5F C3" },
};

#define MAX_PATTERN 600
#define SLOT_PATTERNS ((int)(sizeof(k_slot_text) / sizeof(k_slot_text[0])))
static int16_t g_link_code[MAX_PATTERN], g_unlink_code[MAX_PATTERN];
static int16_t g_slot_code[SLOT_PATTERNS][MAX_PATTERN];
static int g_link_len, g_unlink_len, g_slot_len[SLOT_PATTERNS];
static int g_patterns_ready;

typedef struct XpNotifierStats {
    uint64_t links_skipped;    /* Link calls whose node was kept off the list */
    uint64_t links_passed;     /* Link calls that linked */
    uint64_t unlinks;
    uint64_t checked;          /* watchdog checks */
    uint64_t flips;            /* skipped nodes that could do work again and were linked */
    uint64_t stale;            /* skipped nodes no longer recognised, dropped without touching them */
    uint64_t last_flip_node;
    int32_t skipped;           /* nodes off the list now */
    int32_t listed_at_enable;
    int32_t idle_at_enable;    /* taken off by the pass when turned on */
    int32_t working_at_enable;
    int32_t unrecognised_at_enable;
    int32_t installed;
    int32_t active;
    int32_t last_error;
    int32_t last_flip_gate;
    int32_t reserved;
} XpNotifierStats;

static XpNotifierStats g_nstats;
static SRWLOCK g_napi = SRWLOCK_INIT;       /* serialises the exported functions below */
static CRITICAL_SECTION *g_nlock;          /* g_Notifier_ListLock: guards the list, the set and the caches */
static Node **g_nhead;                     /* &g_Notifier_ListHead */
static uint8_t *g_text_lo, *g_text_hi;     /* the game's .text: the only code read and recognised */
static uint8_t *g_image_lo, *g_image_hi;   /* the game's image: where its vtables are */
static uint8_t *g_link, *g_unlink;
static uint8_t *g_npage;                   /* the two jump stubs: written once, never freed */
static uint64_t g_link_saved, g_link_patched, g_unlink_saved, g_unlink_patched;
static int g_ninstalled;
static int g_nactive;                      /* read and written under g_nlock */
static ULONGLONG g_ninstall_tick;
static uint32_t g_watch_cursor;

/* Turning the skip on must wait until no thread can still be inside an Unlink it entered before the patch (between
 * its entry and taking the lock): its node could be taken off and recorded, then freed after its Unlink found
 * nothing to do. Those few instructions take nanoseconds; this is a margin for a descheduled thread. */
#define GRACE_MS 1500

/* ---- the list operations of Notifier.Link and Notifier.Unlink, exactly; under g_nlock ---- */
static void list_link(Node *n)
{
    Node *h = *g_nhead;
    if (h) {
        h->next = n;
        n->prev = h;
    } else {
        n->prev = NULL;
    }
    n->next = NULL;
    *g_nhead = n;
}

static void list_unlink(Node *n)
{
    Node *next = n->next, *prev = n->prev;
    if (next) {
        next->prev = prev;
    } else if (prev) {
        *g_nhead = prev;
    } else {
        if (*g_nhead != n)
            return;   /* not on the list */
        *g_nhead = NULL;
    }
    if (prev)
        prev->next = next;
    n->next = NULL;
    n->prev = NULL;
}

static inline int node_unlisted(const Node *n) { return n->next == NULL && n->prev == NULL && *g_nhead != n; }

/* ---- classification; under g_nlock ---- */
static int slot_of_code(const uint8_t *fn)
{
    if (fn < g_text_lo || fn >= g_text_hi)
        return S_UNKNOWN;   /* not game code (a vtable made elsewhere): never skipped */
    size_t avail = (size_t)(g_text_hi - fn);
    if (fn[0] == 0xc3 || (avail >= 3 && fn[0] == 0xc2 && fn[1] == 0 && fn[2] == 0))
        return S_NOP;
    for (int i = 0; i < SLOT_PATTERNS; i++)
        if ((size_t)g_slot_len[i] <= avail && matches(fn, g_slot_code[i], (size_t)g_slot_len[i]))
            return k_slot_text[i].slot;
    return S_UNKNOWN;
}

#define GATE_BITS 10
#define GATES (1u << GATE_BITS)
static struct { void **vtbl; int gate; } g_gate_cache[GATES];

static int gate_of_vtable(void **vtbl)
{
    uint64_t x = (uint64_t)(uintptr_t)vtbl * 0x9E3779B97F4A7C15ull;
    unsigned s = (unsigned)(x >> (64 - GATE_BITS));
    for (unsigned i = 0; i < 16; i++) {
        unsigned k = (s + i) & (GATES - 1);
        if (g_gate_cache[k].vtbl == vtbl)
            return g_gate_cache[k].gate;
        if (g_gate_cache[k].vtbl == NULL) {
            int begin = slot_of_code((const uint8_t *)vtbl[1]), end = slot_of_code((const uint8_t *)vtbl[2]), g;
            if (begin == S_NOP && end == S_NOP) g = G_ALWAYS;
            else if ((begin == S_SB_BEGIN && end == S_SB_END) || (begin == S_VB_BEGIN && end == S_VB_END)) g = G_BUFFER;
            else if (begin == S_IB_BEGIN && end == S_IB_END) g = G_INDEX;
            else if (begin == S_TEX_BEGIN && end == S_TEX_END) g = G_TEXTURE;
            else if (begin == S_NOP && end == S_CB_END) g = G_CONSTANT;
            else g = G_ACTIVE;
            g_gate_cache[k].vtbl = vtbl;
            g_gate_cache[k].gate = g;
            return g;
        }
    }
    return G_ACTIVE;   /* cache window full: unrecognised, never skipped */
}

/* A live node's vtable is in the game's image; anything else (memory reused by other data) is never classified. */
static inline int vtable_ok(const Node *n)
{
    uint8_t *v = (uint8_t *)n->vtbl;
    return ((uintptr_t)v & 7) == 0 && v >= g_image_lo && v + 0x18 <= g_image_hi;
}

static int gate_of(const Node *n) { return vtable_ok(n) ? gate_of_vtable(n->vtbl) : G_ACTIVE; }

static int node_idle(const Node *n, int gate)
{
    const uint8_t *p = (const uint8_t *)n;
    uint32_t f;
    switch (gate) {
    case G_ALWAYS:
        return 1;
    case G_BUFFER:    /* shader-resource and vertex buffers */
        return (*(const uint32_t *)(p + 0x1c) & 0x11) == 0;
    case G_INDEX:
        f = *(const uint32_t *)(p + 0x20);
        return (f & 0x11) == 0 || (f & 0x40) != 0;
    case G_TEXTURE:
        f = *(const uint32_t *)(p + 0x3c);
        return (f & 0x100010) != 0x100010 && (f & 0x2000) == 0;
    case G_CONSTANT:  /* ConstantBuffer.Flags; the node is at object +0x38 */
        return (*(const uint32_t *)(p - 0x14) & 0x4000) == 0;
    default:
        return 0;
    }
}

/* ---- the set of skipped nodes: a dense array plus an open-addressing index; under g_nlock ---- */
#define TOMB ((Node *)1)
static Node **g_items;
static uint32_t g_count, g_items_cap;
static Node **g_keys;
static uint32_t *g_vals;
static uint32_t g_map_cap, g_map_used, g_map_shift;   /* used: live keys and tombstones */

static inline uint32_t map_home(const Node *n) { return (uint32_t)(((uint64_t)(uintptr_t)n * 0x9E3779B97F4A7C15ull) >> g_map_shift); }

static int64_t map_find(const Node *n)
{
    if (!g_map_cap)
        return -1;
    for (uint32_t i = map_home(n), probes = 0; probes < g_map_cap; i = (i + 1) & (g_map_cap - 1), probes++) {
        if (g_keys[i] == n)
            return i;
        if (g_keys[i] == NULL)
            return -1;
    }
    return -1;
}

static void map_put(Node *n, uint32_t val)   /* n absent; capacity ensured */
{
    uint32_t i = map_home(n);
    while (g_keys[i] != NULL && g_keys[i] != TOMB)
        i = (i + 1) & (g_map_cap - 1);
    if (g_keys[i] == NULL)
        g_map_used++;
    g_keys[i] = n;
    g_vals[i] = val;
}

static int set_reserve(uint32_t more)
{
    HANDLE heap = GetProcessHeap();
    if (g_count + more > g_items_cap) {
        uint32_t cap = g_items_cap ? g_items_cap : 4096;
        while (cap < g_count + more) cap *= 2;
        Node **items = (Node **)(g_items ? HeapReAlloc(heap, 0, g_items, sizeof(Node *) * cap)
                                         : HeapAlloc(heap, 0, sizeof(Node *) * cap));
        if (!items)
            return 0;
        g_items = items;
        g_items_cap = cap;
    }
    if ((uint64_t)(g_map_used + more) * 2 > g_map_cap) {   /* rehash at load 1/4 of live keys, no tombstones */
        uint32_t cap = 8192, shift = 64 - 13;
        while ((uint64_t)(g_count + more) * 4 > cap) { cap *= 2; shift--; }
        Node **keys = (Node **)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Node *) * cap);
        uint32_t *vals = (uint32_t *)HeapAlloc(heap, 0, sizeof(uint32_t) * cap);
        if (!keys || !vals) {
            if (keys) HeapFree(heap, 0, keys);
            if (vals) HeapFree(heap, 0, vals);
            return 0;
        }
        if (g_keys) { HeapFree(heap, 0, g_keys); HeapFree(heap, 0, g_vals); }
        g_keys = keys; g_vals = vals; g_map_cap = cap; g_map_shift = shift; g_map_used = 0;
        for (uint32_t i = 0; i < g_count; i++)
            map_put(g_items[i], i);
    }
    return 1;
}

static int set_add(Node *n)
{
    if (map_find(n) >= 0)
        return 1;
    if (!set_reserve(1))
        return 0;
    g_items[g_count] = n;
    map_put(n, g_count);
    g_count++;
    g_nstats.skipped = (int32_t)g_count;
    return 1;
}

static void set_remove(const Node *n)
{
    if (!g_count)
        return;
    int64_t s = map_find(n);
    if (s < 0)
        return;
    uint32_t i = g_vals[s];
    g_keys[s] = TOMB;
    g_count--;
    if (i != g_count) {
        Node *last = g_items[g_count];
        g_items[i] = last;
        g_vals[map_find(last)] = i;
    }
    g_nstats.skipped = (int32_t)g_count;
}

static void set_clear(void)
{
    g_count = 0;
    if (g_keys)
        memset(g_keys, 0, sizeof(Node *) * g_map_cap);
    g_map_used = 0;
    g_watch_cursor = 0;
    g_nstats.skipped = 0;
}

/* ---- the replacements for Notifier.Link and Notifier.Unlink (exported only so that profiles can name them) ---- */
__declspec(dllexport) void xp_notifier_link(Node *n)
{
    EnterCriticalSection(g_nlock);
    if (g_nactive && node_unlisted(n) && node_idle(n, gate_of(n)) && set_add(n)) {
        g_nstats.links_skipped++;
    } else {
        set_remove(n);   /* only if the game links a node twice without an Unlink */
        list_link(n);
        g_nstats.links_passed++;
    }
    LeaveCriticalSection(g_nlock);
}

__declspec(dllexport) void xp_notifier_unlink(Node *n)
{
    EnterCriticalSection(g_nlock);
    set_remove(n);
    list_unlink(n);   /* does nothing for a node that is not on the list, as the game's */
    g_nstats.unlinks++;
    LeaveCriticalSection(g_nlock);
}

/* ---- turning the skip on and off; under g_nlock ---- */
static void enable_locked(void)
{
    int32_t listed = 0, idle = 0, working = 0, unrecognised = 0;
    g_nactive = 1;
    for (Node *n = *g_nhead, *older; n; n = older) {
        older = n->prev;
        listed++;
        int g = gate_of(n);
        if (g == G_ACTIVE)
            unrecognised++;
        else if (!node_idle(n, g))
            working++;
        else if (set_add(n)) {
            list_unlink(n);
            idle++;
        } else
            working++;   /* out of memory: stays listed */
    }
    g_nstats.listed_at_enable = listed;
    g_nstats.idle_at_enable = idle;
    g_nstats.working_at_enable = working;
    g_nstats.unrecognised_at_enable = unrecognised;
}

static void disable_locked(void)
{
    g_nactive = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        Node *n = g_items[i];
        if (vtable_ok(n) && node_unlisted(n))
            list_link(n);
        else
            g_nstats.stale++;
    }
    set_clear();
}

static int patterns_ready(void)
{
    if (g_patterns_ready)
        return 1;
    g_link_len = parse_pattern(k_link_text, g_link_code, MAX_PATTERN);
    g_unlink_len = parse_pattern(k_unlink_text, g_unlink_code, MAX_PATTERN);
    if (g_link_len != 0x57 || g_unlink_len != 0x75)
        return 0;
    for (int i = 0; i < SLOT_PATTERNS; i++)
        if ((g_slot_len[i] = parse_pattern(k_slot_text[i].code, g_slot_code[i], MAX_PATTERN)) <= 0)
            return 0;
    g_patterns_ready = 1;
    return 1;
}

static uint8_t *rip_ref(uint8_t *fn, int disp_at, int insn_end) { return rel32_target(fn + insn_end, fn + disp_at); }

/* link, unlink: Notifier.Link and Notifier.Unlink. text: the game's .text; image: the whole module. */
__declspec(dllexport) int xp_notifier_install(void *link, void *unlink, void *text_lo, void *text_hi,
                                              void *image_lo, void *image_hi)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_napi);
    uint8_t *l = (uint8_t *)link, *u = (uint8_t *)unlink;
    if (g_ninstalled) { r = XP_ERR_STATE; goto out; }
    if (!patterns_ready()) { r = XP_ERR_ARGS; goto out; }
    if (!l || !u || (uint8_t *)text_lo >= (uint8_t *)text_hi || (uint8_t *)image_lo > (uint8_t *)text_lo
        || (uint8_t *)image_hi < (uint8_t *)text_hi || l < (uint8_t *)text_lo || l + g_link_len > (uint8_t *)text_hi
        || u < (uint8_t *)text_lo || u + g_unlink_len > (uint8_t *)text_hi || ((uintptr_t)l & 7) || ((uintptr_t)u & 7)) {
        r = XP_ERR_ARGS; goto out;
    }
    if (g_npage && (g_link != l || g_unlink != u)) { r = XP_ERR_STATE; goto out; }
    if (!matches(l, g_link_code, (size_t)g_link_len) || !matches(u, g_unlink_code, (size_t)g_unlink_len)) {
        r = XP_ERR_BYTES; goto out;
    }
    /* Every reference to the lock and the head, in both functions, must agree. */
    uint8_t *lock = rip_ref(l, 0x0c, 0x10), *head = rip_ref(l, 0x19, 0x1d);
    if (rip_ref(l, 0x40, 0x44) != lock || rip_ref(u, 0x0c, 0x10) != lock || rip_ref(u, 0x65, 0x69) != lock
        || rip_ref(l, 0x2b, 0x2f) != head || rip_ref(l, 0x47, 0x4b) != head || rip_ref(u, 0x33, 0x37) != head
        || rip_ref(u, 0x3c, 0x40) != head || rip_ref(u, 0x45, 0x49) != head
        || lock < (uint8_t *)image_lo || lock >= (uint8_t *)image_hi || head < (uint8_t *)image_lo
        || head >= (uint8_t *)image_hi) {
        r = XP_ERR_GLOBALS; goto out;
    }
    /* Patch site: the qword at +8. Its only instruction start is +9 (lea rcx,[lock], 7 bytes), which becomes a
     * jmp rel32 to a stub that undoes the prologue already run (push rbx; sub rsp,20h; mov rbx,rcx: rcx is still the
     * node) and jumps to the replacement as if the caller had called it. */
    if (!g_npage) {
        uint8_t *page = (uint8_t *)alloc_near(l, 4096);
        int32_t a, b;
        if (!page || !near_rel32(l + 14, page, &a) || !near_rel32(u + 14, page + 32, &b)) { r = XP_ERR_NEAR; goto out; }
        static const uint8_t undo[] = { 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xff, 0x25, 0, 0, 0, 0 };   /* add rsp,20h; pop rbx; jmp [rip+0] */
        uint64_t dl = (uint64_t)(uintptr_t)&xp_notifier_link, du = (uint64_t)(uintptr_t)&xp_notifier_unlink;
        memcpy(page, undo, sizeof(undo)); memcpy(page + sizeof(undo), &dl, 8);
        memcpy(page + 32, undo, sizeof(undo)); memcpy(page + 32 + sizeof(undo), &du, 8);
        DWORD old;
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), page, 4096);
        g_link = l;
        g_unlink = u;
        g_npage = page;
    }
    g_nlock = (CRITICAL_SECTION *)lock;
    g_nhead = (Node **)head;
    g_text_lo = (uint8_t *)text_lo; g_text_hi = (uint8_t *)text_hi;
    g_image_lo = (uint8_t *)image_lo; g_image_hi = (uint8_t *)image_hi;

    uint8_t bytes[8];
    int32_t rel_l, rel_u;
    if (!near_rel32(l + 14, g_npage, &rel_l) || !near_rel32(u + 14, g_npage + 32, &rel_u)) { r = XP_ERR_NEAR; goto out; }
    memcpy(&g_link_saved, l + 8, 8);   /* byte 8, lea rcx (48 8D 0D) and its displacement: all verified above */
    memcpy(bytes, &g_link_saved, 8);
    bytes[1] = 0xe9; memcpy(bytes + 2, &rel_l, 4);
    memcpy(&g_link_patched, bytes, 8);
    memcpy(&g_unlink_saved, u + 8, 8);
    memcpy(bytes, &g_unlink_saved, 8);
    bytes[1] = 0xe9; memcpy(bytes + 2, &rel_u, 4);
    memcpy(&g_unlink_patched, bytes, 8);

    g_nactive = 0;   /* nothing is skipped until set_active: the replacements behave exactly as the originals */
    r = write_qword(l + 8, g_link_saved, g_link_patched);
    if (r == XP_OK) {
        r = write_qword(u + 8, g_unlink_saved, g_unlink_patched);
        if (r != XP_OK)
            write_qword(l + 8, g_link_patched, g_link_saved);
    }
    if (r == XP_OK) {
        g_ninstalled = 1;
        g_ninstall_tick = GetTickCount64();
    }
out:
    g_nstats.installed = g_ninstalled;
    g_nstats.last_error = r;
    ReleaseSRWLockExclusive(&g_napi);
    return r;
}

__declspec(dllexport) int xp_notifier_set_active(int on)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_napi);
    if (!g_ninstalled) {
        r = on ? XP_ERR_STATE : XP_OK;
    } else if (on) {
        if (GetTickCount64() - g_ninstall_tick < GRACE_MS) {
            r = XP_ERR_WAIT;
        } else {
            EnterCriticalSection(g_nlock);
            if (!g_nactive)
                enable_locked();
            LeaveCriticalSection(g_nlock);
        }
    } else {
        EnterCriticalSection(g_nlock);
        if (g_nactive || g_count)
            disable_locked();
        LeaveCriticalSection(g_nlock);
    }
    g_nstats.active = g_nactive;
    if (r != XP_ERR_WAIT)
        g_nstats.last_error = r;
    ReleaseSRWLockExclusive(&g_napi);
    return r;
}

/* Puts every skipped node back, then the original code. Threads still inside the replacements finish normally. */
__declspec(dllexport) int xp_notifier_uninstall(void)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_napi);
    if (!g_ninstalled) {
        r = XP_ERR_STATE;
    } else {
        EnterCriticalSection(g_nlock);
        if (g_nactive || g_count)
            disable_locked();
        LeaveCriticalSection(g_nlock);
        int a = write_qword(g_link + 8, g_link_patched, g_link_saved);
        int b = write_qword(g_unlink + 8, g_unlink_patched, g_unlink_saved);
        r = a != XP_OK ? a : b;
        if (a == XP_OK && b == XP_OK)
            g_ninstalled = 0;   /* otherwise the replacements stay reachable: inactive, they are the originals */
    }
    g_nstats.installed = g_ninstalled;
    g_nstats.active = g_nactive;
    g_nstats.last_error = r;
    ReleaseSRWLockExclusive(&g_napi);
    return r;
}

/* Once per frame: re-checks up to `count` skipped nodes. One whose flags now let a callback work is linked again
 * (the research found no code that changes them after creation; this is the safety net). */
__declspec(dllexport) void xp_notifier_watch(int count)
{
    AcquireSRWLockExclusive(&g_napi);
    if (g_ninstalled) {
        EnterCriticalSection(g_nlock);
        int limit = count < (int)g_count ? count : (int)g_count;   /* each node at most once a call */
        for (int k = 0; k < limit && g_nactive && g_count; k++) {
            if (g_watch_cursor >= g_count)
                g_watch_cursor = 0;
            Node *n = g_items[g_watch_cursor];
            g_nstats.checked++;
            int g = gate_of(n);
            if (g != G_ACTIVE && node_idle(n, g)) {
                g_watch_cursor++;
                continue;
            }
            set_remove(n);   /* the last node moves to the cursor, which is checked next */
            if (g != G_ACTIVE && node_unlisted(n)) {
                list_link(n);
                g_nstats.flips++;
                g_nstats.last_flip_node = (uint64_t)(uintptr_t)n;
                g_nstats.last_flip_gate = g;
            } else {
                g_nstats.stale++;   /* no longer a recognised node: leave it alone */
            }
        }
        LeaveCriticalSection(g_nlock);
    }
    ReleaseSRWLockExclusive(&g_napi);
}

/* For tests and diagnostics: the gate of a node (G_*) times 2, plus 1 if it is idle. Needs an install. */
__declspec(dllexport) int xp_notifier_classify(void *node)
{
    int r = -1;
    AcquireSRWLockExclusive(&g_napi);
    if (g_ninstalled && node) {
        EnterCriticalSection(g_nlock);
        int g = gate_of((Node *)node);
        r = g * 2 + node_idle((Node *)node, g);
        LeaveCriticalSection(g_nlock);
    }
    ReleaseSRWLockExclusive(&g_napi);
    return r;
}

__declspec(dllexport) void xp_notifier_get_stats(XpNotifierStats *out)
{
    if (out)
        *out = g_nstats;
}

/* ============================================================================ TempAllocator fast slab cache == */

#define XP_ALLOC_MAGIC      0x5850414C4C4F4331ULL  /* "XPALLOC1" */
#define XP_ALLOC_MAX_CHAIN  8
#define XP_ALLOC_SLAB_SHIFT 16
#define XP_ALLOC_SLAB_SIZE  (1u << XP_ALLOC_SLAB_SHIFT)     /* 64 KiB */
#define XP_ALLOC_MAX_SLABS  4096u                            /* 256 MiB virtual reservation */
#define XP_ALLOC_CLASSES    39
#define XP_ALLOC_MAX_SIZE   16384u
#define XP_ALLOC_SPIN_COUNT 4000u
#define XP_TC_EXITED        ((XpThreadCache *)(uintptr_t)1)

static inline void tc_add_u64(uint64_t *p, uint64_t d)
{
    __atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + d, __ATOMIC_RELAXED);
}

static inline void tc_add_i64(int64_t *p, int64_t d)
{
    __atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + d, __ATOMIC_RELAXED);
}

/* Every size class is a multiple of 16 bytes so every carved block in a 64 KiB slab is 16-byte aligned,
 * and every class fits >= batch_sz blocks per 64 KiB slab. Matches TempAllocator's Pool (<=128 B) and
 * bone-matrix Heap allocations (48 B multiples up to 4 KiB = 98%, <=16 KiB = 99.5%). */
static const uint16_t k_class_size[XP_ALLOC_CLASSES] = {
      16,   32,   48,   64,   80,   96,  112,  128,
     144,  192,  240,  288,  336,  384,
     480,  576,  672,  768,  896, 1024,
    1200, 1344, 1536, 1728, 1920, 2048, 2304, 2560, 2816, 3072, 3328, 3584, 4096,
    5120, 6144, 8192, 10240, 12288, 16384
};

static const uint16_t k_class_batch[XP_ALLOC_CLASSES] = {
    32, 32, 32, 32, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32,
    32, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16,  8,  8,  8,  8,  8,  8,  8,
     4,  4,  4,  4,  4,  4
};

static uint8_t g_size_to_class[(XP_ALLOC_MAX_SIZE >> 4) + 1];

/* Verified AllocatorHybrid vtable thunks (game 2026.11.03, vtbl 0x142158798):
 *   [2] Allocate (+0x10), [3] Reallocate (+0x18), [4] Free (+0x20), [9] GetSize (+0x48). */
static const uint8_t k_hybrid_alloc_bytes[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xC1, 0x81, 0x10, 0x02, 0x00, 0x00,
    0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x10
};
static const uint8_t k_hybrid_realloc_bytes[] = {
    0x4D, 0x85, 0xC0, 0x74, 0x0D,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xC1, 0x81, 0x10, 0x02, 0x00, 0x00,
    0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x18
};
static const uint8_t k_hybrid_free_bytes[] = {
    0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x20
};
static const uint8_t k_hybrid_getsize_bytes[] = {
    0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x48
};

typedef void *(*alloc_dtor_fn)(void *self, uint8_t flags);
typedef void (*alloc_cleanup_fn)(void *self);
typedef void *(*alloc_alloc_fn)(void *self, size_t size, size_t align);
typedef void *(*alloc_realloc_fn)(void *self, void *ptr, size_t size, size_t align);
typedef void (*alloc_free_fn)(void *self, void *ptr);
typedef uint64_t (*alloc_u64_fn)(void *self);
typedef uint32_t (*alloc_u32_fn)(void *self);
typedef uint64_t (*alloc_getsize_fn)(void *self, void *ptr);

typedef struct XpAllocVtable {
    uint64_t magic;
    void **orig_vtable;
    uint32_t depth;
    uint32_t reserved;
    void *slots[12];
} XpAllocVtable;

typedef struct XpCentralClass {
    SRWLOCK lock;
    void *batches;         /* stack of full batches (exactly k_class_batch[c] blocks); head[1] = next batch */
    void *partial;         /* leftover individual blocks from exiting threads */
    uint32_t partial_cnt;
    uint32_t pad;
} XpCentralClass;

typedef struct XpThreadCache {
    void *mag[XP_ALLOC_CLASSES];
    void *spare[XP_ALLOC_CLASSES];
    uint16_t count[XP_ALLOC_CLASSES];
    uint64_t fast_allocs;
    uint64_t fast_frees;
    uint64_t central_hits;
    uint64_t central_misses;
    int64_t live_blocks;
    int64_t live_bytes;
    struct XpThreadCache *prev;
    struct XpThreadCache *next;
} XpThreadCache;

typedef struct XpAllocStats {
    uint64_t fast_allocs;
    uint64_t fast_frees;
    uint64_t fallback_allocs;
    uint64_t fallback_frees;
    uint64_t central_hits;
    uint64_t central_misses;
    uint64_t arena_full;
    uint64_t corrupt;
    int64_t live_blocks;
    int64_t live_bytes;
    uint64_t committed_bytes;
    int32_t slabs_used;
    int32_t slabs_max;
    int32_t active_threads;
    int32_t exited_threads;
    int32_t installed;
    int32_t active;
    int32_t chain_depth;
    int32_t last_error;
} XpAllocStats;

typedef BOOLEAN (WINAPI *rtl_shutdown_fn)(void);

static SRWLOCK g_aapi = SRWLOCK_INIT;
static SRWLOCK g_arena_lock = SRWLOCK_INIT;
static SRWLOCK g_tc_lock = SRWLOCK_INIT;

static void *g_temp_alloc;
static void **g_aorig_vt;       /* game's real vtbl_Client::Graphics::AllocatorHybrid */
static void **g_anext_vt;       /* vtable we replaced (either g_aorig_vt or a prior disabled build's XpAllocVtable) */
static CRITICAL_SECTION *g_heap_cs;
static CRITICAL_SECTION *g_pool_cs;
static DWORD g_orig_heap_spin;
static DWORD g_orig_pool_spin;
static int g_ainstalled;
static int g_adestroyed;
static volatile LONG g_aactive;
static int g_alast_error;
static int g_achain_depth;
static rtl_shutdown_fn g_rtl_shutdown;

static void *g_arena_raw;
static uintptr_t g_arena_base;
static size_t g_arena_size;
static uint32_t g_slabs_used;
static uint8_t g_slab_class[XP_ALLOC_MAX_SLABS];
static XpCentralClass g_central[XP_ALLOC_CLASSES];

static DWORD g_tls_idx = TLS_OUT_OF_INDEXES;
static DWORD g_fls_idx = FLS_OUT_OF_INDEXES;
static XpThreadCache *g_tc_head;
static int32_t g_tc_active_count;
static int32_t g_tc_exited_count;

/* Totals folded in from exited threads (protected by g_tc_lock). */
static uint64_t g_ex_fast_allocs;
static uint64_t g_ex_fast_frees;
static uint64_t g_ex_central_hits;
static uint64_t g_ex_central_misses;
static int64_t g_ex_live_blocks;
static int64_t g_ex_live_bytes;

/* Cold-path atomic counters. */
static uint64_t g_afallback_allocs;
static uint64_t g_afallback_frees;
static uint64_t g_aarena_full;
static uint64_t g_acorrupt;

static inline int alloc_is_ours(const void *ptr)
{
    return g_arena_base != 0 && ((uintptr_t)ptr - g_arena_base) < g_arena_size;
}

static inline int ptr_valid_link(const void *p)
{
    return p == NULL || (alloc_is_ours(p) && ((uintptr_t)p & 15u) == 0);
}

/* Direct x64 TEB TLS slot read: avoids TlsGetValue's SetLastError(0) overwrite and kernel32->kernelbase hop.
 * Standard Windows/Wine x64 TEB offsets: TlsSlots[64] at gs:[0x1480], TlsExpansionSlots ptr at gs:[0x1780]. */
static inline void *teb_tls_get(DWORD idx)
{
    if (idx < 64u)
        return (void *)__readgsqword(0x1480u + idx * 8u);
    if (idx < 1088u) {
        void **exp = (void **)__readgsqword(0x1780u);
        return exp ? exp[idx - 64u] : NULL;
    }
    return NULL;
}

static inline void central_push_batch(int c, void *head)
{
    XpCentralClass *cc = &g_central[c];
    AcquireSRWLockExclusive(&cc->lock);
    ((void **)head)[1] = cc->batches;
    cc->batches = head;
    ReleaseSRWLockExclusive(&cc->lock);
}

static void central_push_partial(int c, void *head)
{
    XpCentralClass *cc = &g_central[c];
    uint32_t batch = k_class_batch[c];
    AcquireSRWLockExclusive(&cc->lock);
    while (head) {
        void *next = ((void **)head)[0];
        if (!ptr_valid_link(next)) {
            __atomic_fetch_add(&g_acorrupt, 1, __ATOMIC_RELAXED);
            next = NULL;
        }
        ((void **)head)[0] = cc->partial;
        cc->partial = head;
        cc->partial_cnt++;
        if (cc->partial_cnt == batch) {
            void *full = cc->partial;
            cc->partial = NULL;
            cc->partial_cnt = 0;
            ((void **)full)[1] = cc->batches;
            cc->batches = full;
        }
        head = next;
    }
    ReleaseSRWLockExclusive(&cc->lock);
}

static void *central_pop_batch(int c, uint16_t *out_count)
{
    XpCentralClass *cc = &g_central[c];
    void *res = NULL;
    uint16_t cnt = 0;
    AcquireSRWLockExclusive(&cc->lock);
    if (cc->batches) {
        res = cc->batches;
        void *next_b = ((void **)res)[1];
        if (!ptr_valid_link(next_b)) {
            __atomic_fetch_add(&g_acorrupt, 1, __ATOMIC_RELAXED);
            next_b = NULL;
        }
        cc->batches = next_b;
        ((void **)res)[1] = NULL;
        cnt = k_class_batch[c];
    } else if (cc->partial) {
        res = cc->partial;
        cnt = (uint16_t)cc->partial_cnt;
        cc->partial = NULL;
        cc->partial_cnt = 0;
    }
    ReleaseSRWLockExclusive(&cc->lock);
    *out_count = cnt;
    return res;
}

/* Commit one fresh 64 KiB slab for class c. Returns a magazine for the calling thread and pushes any
 * extra full batches to g_central[c]. Never nests g_arena_lock and g_central[c].lock. Preserves LastError. */
static void *carve_slab(int c, uint16_t *out_count)
{
    DWORD saved_err = GetLastError();
    AcquireSRWLockExclusive(&g_arena_lock);
    if (!__atomic_load_n(&g_aactive, __ATOMIC_ACQUIRE)) {
        ReleaseSRWLockExclusive(&g_arena_lock);
        *out_count = 0;
        return NULL;
    }
    if (g_slabs_used >= XP_ALLOC_MAX_SLABS) {
        ReleaseSRWLockExclusive(&g_arena_lock);
        __atomic_fetch_add(&g_aarena_full, 1, __ATOMIC_RELAXED);
        *out_count = 0;
        return NULL;
    }
    uint32_t idx = g_slabs_used;
    uint8_t *slab = (uint8_t *)(g_arena_base + ((size_t)idx << XP_ALLOC_SLAB_SHIFT));
    if (!VirtualAlloc(slab, XP_ALLOC_SLAB_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
        ReleaseSRWLockExclusive(&g_arena_lock);
        SetLastError(saved_err);
        __atomic_fetch_add(&g_aarena_full, 1, __ATOMIC_RELAXED);
        *out_count = 0;
        return NULL;
    }
    g_slab_class[idx] = (uint8_t)c;
    __atomic_store_n(&g_slabs_used, idx + 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_arena_lock);
    SetLastError(saved_err);

    uint32_t sz = k_class_size[c];
    uint32_t batch = k_class_batch[c];
    uint32_t total = XP_ALLOC_SLAB_SIZE / sz;   /* guaranteed >= batch for all classes */
    uint32_t rem = total % batch;
    uint32_t caller_n = rem ? rem : batch;
    uint32_t pos = 0;

    /* Build caller's magazine [0 .. caller_n) */
    void *caller_head = NULL;
    for (uint32_t i = 0; i < caller_n; i++, pos++) {
        void *blk = slab + (size_t)pos * sz;
        ((void **)blk)[0] = caller_head;
        caller_head = blk;
    }

    /* Build remaining exact full batches and push them to central in one lock acquisition */
    void *extra_head = NULL;
    void *extra_tail = NULL;
    while (pos + batch <= total) {
        void *bhead = NULL;
        for (uint32_t i = 0; i < batch; i++, pos++) {
            void *blk = slab + (size_t)pos * sz;
            ((void **)blk)[0] = bhead;
            bhead = blk;
        }
        ((void **)bhead)[1] = extra_head;
        if (!extra_tail)
            extra_tail = bhead;
        extra_head = bhead;
    }
    if (extra_head) {
        XpCentralClass *cc = &g_central[c];
        AcquireSRWLockExclusive(&cc->lock);
        ((void **)extra_tail)[1] = cc->batches;
        cc->batches = extra_head;
        ReleaseSRWLockExclusive(&cc->lock);
    }

    *out_count = (uint16_t)caller_n;
    return caller_head;
}

static VOID WINAPI xp_alloc_fls_dtor(PVOID data)
{
    XpThreadCache *tc = (XpThreadCache *)data;
    if (!tc || tc == XP_TC_EXITED)
        return;
    /* During process shutdown, other threads may already have been terminated while holding an SRWLOCK. */
    if (g_rtl_shutdown && g_rtl_shutdown())
        return;
    /* Mark this thread as tearing down so any late Free does not re-create a ThreadCache. */
    if (g_tls_idx != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_tls_idx, XP_TC_EXITED);
    for (int c = 0; c < XP_ALLOC_CLASSES; c++) {
        if (tc->spare[c]) {
            central_push_batch(c, tc->spare[c]);
            tc->spare[c] = NULL;
        }
        if (tc->mag[c]) {
            if (tc->count[c] == k_class_batch[c])
                central_push_batch(c, tc->mag[c]);
            else
                central_push_partial(c, tc->mag[c]);
            tc->mag[c] = NULL;
            tc->count[c] = 0;
        }
    }
    AcquireSRWLockExclusive(&g_tc_lock);
    g_ex_fast_allocs += tc->fast_allocs;
    g_ex_fast_frees += tc->fast_frees;
    g_ex_central_hits += tc->central_hits;
    g_ex_central_misses += tc->central_misses;
    g_ex_live_blocks += tc->live_blocks;
    g_ex_live_bytes += tc->live_bytes;
    if (tc->prev)
        tc->prev->next = tc->next;
    else
        g_tc_head = tc->next;
    if (tc->next)
        tc->next->prev = tc->prev;
    g_tc_active_count--;
    g_tc_exited_count++;
    ReleaseSRWLockExclusive(&g_tc_lock);
    HeapFree(GetProcessHeap(), 0, tc);
}

static XpThreadCache *get_thread_cache_slow(void)
{
    DWORD saved_err = GetLastError();
    XpThreadCache *tc = (XpThreadCache *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XpThreadCache));
    if (!tc) {
        SetLastError(saved_err);
        return NULL;
    }
    if (!TlsSetValue(g_tls_idx, tc) || !FlsSetValue(g_fls_idx, tc)) {
        TlsSetValue(g_tls_idx, NULL);
        HeapFree(GetProcessHeap(), 0, tc);
        SetLastError(saved_err);
        return NULL;
    }
    AcquireSRWLockExclusive(&g_tc_lock);
    tc->next = g_tc_head;
    if (g_tc_head)
        g_tc_head->prev = tc;
    g_tc_head = tc;
    g_tc_active_count++;
    ReleaseSRWLockExclusive(&g_tc_lock);
    SetLastError(saved_err);
    return tc;
}

static inline XpThreadCache *get_thread_cache(void)
{
    XpThreadCache *tc = (XpThreadCache *)teb_tls_get(g_tls_idx);
    return tc ? tc : get_thread_cache_slow();
}

static void sum_tc_stats(uint64_t *fast_a, uint64_t *fast_f, uint64_t *chits, uint64_t *cmiss,
                         int64_t *live_b, int64_t *live_by, int32_t *act_t, int32_t *ex_t)
{
    AcquireSRWLockShared(&g_tc_lock);
    uint64_t fa = g_ex_fast_allocs, ff = g_ex_fast_frees, ch = g_ex_central_hits, cm = g_ex_central_misses;
    int64_t lb = g_ex_live_blocks, lby = g_ex_live_bytes;
    for (XpThreadCache *tc = g_tc_head; tc; tc = tc->next) {
        fa += __atomic_load_n(&tc->fast_allocs, __ATOMIC_RELAXED);
        ff += __atomic_load_n(&tc->fast_frees, __ATOMIC_RELAXED);
        ch += __atomic_load_n(&tc->central_hits, __ATOMIC_RELAXED);
        cm += __atomic_load_n(&tc->central_misses, __ATOMIC_RELAXED);
        lb += __atomic_load_n(&tc->live_blocks, __ATOMIC_RELAXED);
        lby += __atomic_load_n(&tc->live_bytes, __ATOMIC_RELAXED);
    }
    int32_t at = g_tc_active_count, et = g_tc_exited_count;
    ReleaseSRWLockShared(&g_tc_lock);
    if (fast_a) *fast_a = fa;
    if (fast_f) *fast_f = ff;
    if (chits) *chits = ch;
    if (cmiss) *cmiss = cm;
    if (live_b) *live_b = lb;
    if (live_by) *live_by = lby;
    if (act_t) *act_t = at;
    if (ex_t) *ex_t = et;
}

/* ---- Vtable replacements (exported so profiles name them) ---- */

__declspec(dllexport) void *xp_temp_alloc(void *self, size_t size, size_t align)
{
    if (!__atomic_load_n(&g_aactive, __ATOMIC_ACQUIRE) || align > 16 || (align & (align - 1)) != 0) {
        __atomic_fetch_add(&g_afallback_allocs, 1, __ATOMIC_RELAXED);
        return ((alloc_alloc_fn)g_anext_vt[2])(self, size, align);
    }
    size_t need = size ? size : 1;
    if (need < align)
        need = align;
    if (need > XP_ALLOC_MAX_SIZE) {
        __atomic_fetch_add(&g_afallback_allocs, 1, __ATOMIC_RELAXED);
        return ((alloc_alloc_fn)g_anext_vt[2])(self, size, align);
    }
    int c = g_size_to_class[(need + 15u) >> 4];
    XpThreadCache *tc = get_thread_cache();
    if (!tc || tc == XP_TC_EXITED) {
        __atomic_fetch_add(&g_afallback_allocs, 1, __ATOMIC_RELAXED);
        return ((alloc_alloc_fn)g_anext_vt[2])(self, size, align);
    }
    void *blk = tc->mag[c];
    if (blk) {
        void *next = ((void **)blk)[0];
        if (!ptr_valid_link(next)) {
            __atomic_fetch_add(&g_acorrupt, 1, __ATOMIC_RELAXED);
            tc->mag[c] = NULL;
            tc->count[c] = 0;
        } else {
            tc->mag[c] = next;
            tc->count[c]--;
        }
    } else if (tc->spare[c]) {
        blk = tc->spare[c];
        tc->spare[c] = NULL;
        void *next = ((void **)blk)[0];
        if (!ptr_valid_link(next)) {
            __atomic_fetch_add(&g_acorrupt, 1, __ATOMIC_RELAXED);
            tc->mag[c] = NULL;
            tc->count[c] = 0;
        } else {
            tc->mag[c] = next;
            tc->count[c] = (uint16_t)(k_class_batch[c] - 1);
        }
    } else {
        uint16_t got = 0;
        void *head = central_pop_batch(c, &got);
        if (head) {
            tc_add_u64(&tc->central_hits, 1);
        } else {
            head = carve_slab(c, &got);
            if (!head) {
                __atomic_fetch_add(&g_afallback_allocs, 1, __ATOMIC_RELAXED);
                return ((alloc_alloc_fn)g_anext_vt[2])(self, size, align);
            }
            tc_add_u64(&tc->central_misses, 1);
        }
        blk = head;
        void *next = ((void **)blk)[0];
        if (!ptr_valid_link(next)) {
            __atomic_fetch_add(&g_acorrupt, 1, __ATOMIC_RELAXED);
            tc->mag[c] = NULL;
            tc->count[c] = 0;
        } else {
            tc->mag[c] = next;
            tc->count[c] = (uint16_t)(got - 1);
        }
    }
    tc_add_u64(&tc->fast_allocs, 1);
    tc_add_i64(&tc->live_blocks, 1);
    tc_add_i64(&tc->live_bytes, (int64_t)k_class_size[c]);
    return blk;
}

__declspec(dllexport) void xp_temp_free(void *self, void *ptr)
{
    if (!ptr)
        return;
    if (!alloc_is_ours(ptr)) {
        __atomic_fetch_add(&g_afallback_frees, 1, __ATOMIC_RELAXED);
        ((alloc_free_fn)g_anext_vt[4])(self, ptr);
        return;
    }
    uint32_t slab_idx = (uint32_t)(((uintptr_t)ptr - g_arena_base) >> XP_ALLOC_SLAB_SHIFT);
    int c = g_slab_class[slab_idx];
    XpThreadCache *tc = get_thread_cache();
    if (!tc || tc == XP_TC_EXITED) {
        ((void **)ptr)[0] = NULL;
        central_push_partial(c, ptr);
        AcquireSRWLockExclusive(&g_tc_lock);
        g_ex_fast_frees++;
        g_ex_live_blocks--;
        g_ex_live_bytes -= k_class_size[c];
        ReleaseSRWLockExclusive(&g_tc_lock);
        return;
    }
    uint16_t batch = k_class_batch[c];
    if (tc->count[c] < batch) {
        ((void **)ptr)[0] = tc->mag[c];
        tc->mag[c] = ptr;
        tc->count[c]++;
    } else if (!tc->spare[c]) {
        tc->spare[c] = tc->mag[c];
        ((void **)ptr)[0] = NULL;
        tc->mag[c] = ptr;
        tc->count[c] = 1;
    } else {
        central_push_batch(c, tc->spare[c]);
        tc->spare[c] = tc->mag[c];
        ((void **)ptr)[0] = NULL;
        tc->mag[c] = ptr;
        tc->count[c] = 1;
    }
    tc_add_u64(&tc->fast_frees, 1);
    tc_add_i64(&tc->live_blocks, -1);
    tc_add_i64(&tc->live_bytes, -(int64_t)k_class_size[c]);
}

__declspec(dllexport) void *xp_temp_realloc(void *self, void *ptr, size_t new_size, size_t new_align)
{
    if (new_size == 0) {
        xp_temp_free(self, ptr);
        return NULL;
    }
    if (!ptr)
        return xp_temp_alloc(self, new_size, new_align);
    if (!alloc_is_ours(ptr))
        return ((alloc_realloc_fn)g_anext_vt[3])(self, ptr, new_size, new_align);

    uint32_t slab_idx = (uint32_t)(((uintptr_t)ptr - g_arena_base) >> XP_ALLOC_SLAB_SHIFT);
    uint8_t old_cls = g_slab_class[slab_idx];
    size_t old_size = k_class_size[old_cls];
    if (__atomic_load_n(&g_aactive, __ATOMIC_ACQUIRE)
            && new_align <= 16 && (new_align & (new_align - 1)) == 0) {
        size_t need = new_size < new_align ? new_align : new_size;
        if (need <= old_size && (old_cls == 0 || need > (old_size >> 1)))
            return ptr;
    }
    void *dst = xp_temp_alloc(self, new_size, new_align);
    if (dst) {
        memcpy(dst, ptr, new_size < old_size ? new_size : old_size);
        xp_temp_free(self, ptr);
    }
    return dst;
}

__declspec(dllexport) uint64_t xp_temp_get_size(void *self, void *ptr)
{
    if (ptr && alloc_is_ours(ptr)) {
        uint32_t slab_idx = (uint32_t)(((uintptr_t)ptr - g_arena_base) >> XP_ALLOC_SLAB_SHIFT);
        return k_class_size[g_slab_class[slab_idx]];
    }
    return ((alloc_getsize_fn)g_anext_vt[9])(self, ptr);
}

static void alloc_mark_destroyed(void)
{
    AcquireSRWLockExclusive(&g_aapi);
    if (g_ainstalled && !g_adestroyed) {
        if (__atomic_load_n(&g_aactive, __ATOMIC_RELAXED)) {
            SetCriticalSectionSpinCount(g_heap_cs, g_orig_heap_spin);
            SetCriticalSectionSpinCount(g_pool_cs, g_orig_pool_spin);
            __atomic_store_n(&g_aactive, 0, __ATOMIC_RELEASE);
        }
        g_adestroyed = 1;
    }
    ReleaseSRWLockExclusive(&g_aapi);
}

static void *xp_temp_dtor(void *self, uint8_t flags)
{
    alloc_mark_destroyed();
    return ((alloc_dtor_fn)g_anext_vt[0])(self, flags);
}

static void xp_temp_cleanup(void *self)
{
    alloc_mark_destroyed();
    ((alloc_cleanup_fn)g_anext_vt[1])(self);
}

static uint64_t xp_temp_get_reserved(void *self)
{
    uint64_t committed = (uint64_t)__atomic_load_n(&g_slabs_used, __ATOMIC_ACQUIRE) << XP_ALLOC_SLAB_SHIFT;
    return ((alloc_u64_fn)g_anext_vt[5])(self) + committed;
}

static uint64_t xp_temp_get_used(void *self)
{
    int64_t live_by = 0;
    sum_tc_stats(NULL, NULL, NULL, NULL, NULL, &live_by, NULL, NULL);
    return ((alloc_u64_fn)g_anext_vt[6])(self) + (live_by > 0 ? (uint64_t)live_by : 0);
}

static uint64_t xp_temp_get_peak(void *self)
{
    uint64_t committed = (uint64_t)__atomic_load_n(&g_slabs_used, __ATOMIC_ACQUIRE) << XP_ALLOC_SLAB_SHIFT;
    return ((alloc_u64_fn)g_anext_vt[7])(self) + committed;
}

static uint32_t xp_temp_get_alloc_count(void *self)
{
    uint64_t fa = 0;
    sum_tc_stats(&fa, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    return ((alloc_u32_fn)g_anext_vt[8])(self) + (uint32_t)fa;
}

static uint64_t xp_temp_get_pool_reserved(void *self) { return ((alloc_u64_fn)g_anext_vt[10])(self); }
static uint64_t xp_temp_get_pool_used(void *self) { return ((alloc_u64_fn)g_anext_vt[11])(self); }

static XpAllocVtable g_avt = {
    .magic = XP_ALLOC_MAGIC,
    .orig_vtable = NULL,
    .depth = 0,
    .reserved = 0,
    .slots = {
        (void *)xp_temp_dtor,
        (void *)xp_temp_cleanup,
        (void *)xp_temp_alloc,
        (void *)xp_temp_realloc,
        (void *)xp_temp_free,
        (void *)xp_temp_get_reserved,
        (void *)xp_temp_get_used,
        (void *)xp_temp_get_peak,
        (void *)xp_temp_get_alloc_count,
        (void *)xp_temp_get_size,
        (void *)xp_temp_get_pool_reserved,
        (void *)xp_temp_get_pool_used,
    },
};

static int readable_range(const void *addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    DWORD p = mbi.Protect & 0xFF;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return 0;
    if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE
            && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_WRITECOPY)
        return 0;
    uintptr_t a = (uintptr_t)addr, base = (uintptr_t)mbi.BaseAddress;
    return a >= base && a + len <= base + mbi.RegionSize;
}

static int verify_hybrid_vtable(void **vt, uintptr_t text_start, uintptr_t text_end)
{
    for (int i = 0; i < 12; i++) {
        uintptr_t fn = (uintptr_t)vt[i];
        if (fn < text_start || fn + 32 > text_end)
            return 0;
    }
    if (memcmp(vt[2], k_hybrid_alloc_bytes, sizeof(k_hybrid_alloc_bytes)) != 0)
        return 0;
    if (memcmp(vt[3], k_hybrid_realloc_bytes, sizeof(k_hybrid_realloc_bytes)) != 0)
        return 0;
    if (memcmp(vt[4], k_hybrid_free_bytes, sizeof(k_hybrid_free_bytes)) != 0)
        return 0;
    if (memcmp(vt[9], k_hybrid_getsize_bytes, sizeof(k_hybrid_getsize_bytes)) != 0)
        return 0;
    return 1;
}

static void init_size_table(void)
{
    g_size_to_class[0] = 0;
    for (uint32_t q = 1; q <= (XP_ALLOC_MAX_SIZE >> 4); q++) {
        uint32_t sz = q << 4;
        uint8_t cls = XP_ALLOC_CLASSES - 1;
        for (int c = 0; c < XP_ALLOC_CLASSES; c++) {
            if (k_class_size[c] >= sz) {
                cls = (uint8_t)c;
                break;
            }
        }
        g_size_to_class[q] = cls;
    }
}

__declspec(dllexport) int xp_alloc_install(void *temp_alloc, void *expected_hybrid_vtbl,
                                           void *expected_heap_vtbl, void *expected_pool_vtbl,
                                           void *text_start, void *text_end)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_aapi);
    if (!temp_alloc || !expected_hybrid_vtbl || !expected_heap_vtbl || !expected_pool_vtbl
            || !text_start || (uintptr_t)text_end <= (uintptr_t)text_start) {
        r = XP_ERR_ARGS;
        goto out;
    }
    if (g_adestroyed) {
        r = XP_ERR_STATE;
        goto out;
    }
    if (g_ainstalled) {
        if (temp_alloc != g_temp_alloc) {
            r = XP_ERR_STATE;
            goto out;
        }
        void **live_vt = *(void ***)temp_alloc;
        if (live_vt == &g_avt.slots[0]) {
            r = XP_OK;
        } else {
            uintptr_t hdr_addr = (uintptr_t)live_vt - offsetof(XpAllocVtable, slots);
            if (readable_range((const void *)hdr_addr, sizeof(XpAllocVtable))) {
                const XpAllocVtable *top = (const XpAllocVtable *)hdr_addr;
                if (top->magic == XP_ALLOC_MAGIC && top->orig_vtable == g_aorig_vt) {
                    g_achain_depth = (int)top->depth;
                    r = XP_OK;
                } else {
                    r = XP_ERR_BYTES;
                }
            } else {
                r = XP_ERR_BYTES;
            }
        }
        goto out;
    }
    uint8_t *obj = (uint8_t *)temp_alloc;
    if (!readable_range(obj, 0x218) || !readable_range(expected_hybrid_vtbl, 12 * sizeof(void *))) {
        r = XP_ERR_ARGS;
        goto out;
    }
    /* Verify the AllocatorHybrid object layout: Heap at +0x08, Pool at +0x90, Pool.Backing == &Heap. */
    if (*(void **)(obj + 0x08) != expected_heap_vtbl
            || *(void **)(obj + 0x90) != expected_pool_vtbl
            || *(void **)(obj + 0x90 + 0x108) != (void *)(obj + 0x08)) {
        r = XP_ERR_BYTES;
        goto out;
    }
    if (!verify_hybrid_vtable((void **)expected_hybrid_vtbl, (uintptr_t)text_start, (uintptr_t)text_end)) {
        r = XP_ERR_BYTES;
        goto out;
    }

    void **cur_vt = *(void ***)obj;
    int depth = 1;
    if (cur_vt != (void **)expected_hybrid_vtbl) {
        /* Hot-reload chaining: accept a prior disabled build's XpAllocVtable wrapping the same game vtable. */
        uintptr_t hdr_addr = (uintptr_t)cur_vt - offsetof(XpAllocVtable, slots);
        if (!readable_range((const void *)hdr_addr, sizeof(XpAllocVtable))) {
            r = XP_ERR_BYTES;
            goto out;
        }
        const XpAllocVtable *prev = (const XpAllocVtable *)hdr_addr;
        if (prev->magic != XP_ALLOC_MAGIC || prev->orig_vtable != (void **)expected_hybrid_vtbl) {
            r = XP_ERR_BYTES;
            goto out;
        }
        if (prev->depth >= XP_ALLOC_MAX_CHAIN) {
            r = XP_ERR_CHAIN;
            goto out;
        }
        depth = (int)prev->depth + 1;
    }

    if (!g_arena_raw) {
        size_t res_bytes = ((size_t)XP_ALLOC_MAX_SLABS + 1u) << XP_ALLOC_SLAB_SHIFT;
        void *raw = VirtualAlloc(NULL, res_bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (!raw) {
            r = XP_ERR_MEMORY;
            goto out;
        }
        DWORD tls = TlsAlloc();
        DWORD fls = FlsAlloc(xp_alloc_fls_dtor);
        if (tls == TLS_OUT_OF_INDEXES || fls == FLS_OUT_OF_INDEXES) {
            if (tls != TLS_OUT_OF_INDEXES) TlsFree(tls);
            if (fls != FLS_OUT_OF_INDEXES) FlsFree(fls);
            VirtualFree(raw, 0, MEM_RELEASE);
            r = XP_ERR_MEMORY;
            goto out;
        }
        g_arena_raw = raw;
        g_arena_base = ((uintptr_t)raw + (XP_ALLOC_SLAB_SIZE - 1u)) & ~(uintptr_t)(XP_ALLOC_SLAB_SIZE - 1u);
        g_arena_size = (size_t)XP_ALLOC_MAX_SLABS << XP_ALLOC_SLAB_SHIFT;
        g_tls_idx = tls;
        g_fls_idx = fls;
        for (int c = 0; c < XP_ALLOC_CLASSES; c++)
            InitializeSRWLock(&g_central[c].lock);
        init_size_table();
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (nt)
            g_rtl_shutdown = (rtl_shutdown_fn)(void *)GetProcAddress(nt, "RtlDllShutdownInProgress");
    }

    g_temp_alloc = temp_alloc;
    g_aorig_vt = (void **)expected_hybrid_vtbl;
    g_anext_vt = cur_vt;
    g_heap_cs = (CRITICAL_SECTION *)(obj + 0x68);
    g_pool_cs = (CRITICAL_SECTION *)(obj + 0x1E8);
    g_achain_depth = depth;
    g_avt.orig_vtable = g_aorig_vt;
    g_avt.depth = (uint32_t)depth;
    __atomic_store_n(&g_aactive, 0, __ATOMIC_RELEASE);

    /* Atomically publish our vtable pointer on TempAllocator (starts inactive until xp_alloc_set_active(1)). */
    if (InterlockedCompareExchangePointer((void *volatile *)obj, (void *)&g_avt.slots[0], (void *)cur_vt)
            != (void *)cur_vt) {
        r = XP_ERR_RACE;
        goto out;
    }
    g_ainstalled = 1;
out:
    g_alast_error = r;
    ReleaseSRWLockExclusive(&g_aapi);
    return r;
}

__declspec(dllexport) int xp_alloc_set_active(int active)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_aapi);
    if (!g_ainstalled || g_adestroyed) {
        r = XP_ERR_STATE;
        goto out;
    }
    int want = active ? 1 : 0;
    int was = (int)__atomic_load_n(&g_aactive, __ATOMIC_RELAXED);
    if (want != was) {
        if (want) {
            g_orig_heap_spin = SetCriticalSectionSpinCount(g_heap_cs, XP_ALLOC_SPIN_COUNT);
            g_orig_pool_spin = SetCriticalSectionSpinCount(g_pool_cs, XP_ALLOC_SPIN_COUNT);
        } else {
            SetCriticalSectionSpinCount(g_heap_cs, g_orig_heap_spin);
            SetCriticalSectionSpinCount(g_pool_cs, g_orig_pool_spin);
        }
        __atomic_store_n(&g_aactive, want, __ATOMIC_RELEASE);
    }
out:
    g_alast_error = r;
    ReleaseSRWLockExclusive(&g_aapi);
    return r;
}

/* Disable fast allocations and restore spin counts. The passthrough vtable stays installed whenever any arena slab
 * has ever been committed (so live objects in the game can always free back to our arena with zero race window);
 * when zero slabs were ever committed, the previous vtable pointer is restored atomically. */
__declspec(dllexport) int xp_alloc_uninstall(void)
{
    int r = XP_OK;
    AcquireSRWLockExclusive(&g_aapi);
    if (g_adestroyed || !g_ainstalled)
        goto out;
    if (__atomic_load_n(&g_aactive, __ATOMIC_RELAXED)) {
        SetCriticalSectionSpinCount(g_heap_cs, g_orig_heap_spin);
        SetCriticalSectionSpinCount(g_pool_cs, g_orig_pool_spin);
        __atomic_store_n(&g_aactive, 0, __ATOMIC_RELEASE);
    }
    /* Serialize with any in-flight carve_slab before testing g_slabs_used == 0. */
    AcquireSRWLockExclusive(&g_arena_lock);
    uint32_t slabs = g_slabs_used;
    ReleaseSRWLockExclusive(&g_arena_lock);
    if (slabs == 0) {
        if (InterlockedCompareExchangePointer((void *volatile *)g_temp_alloc, (void *)g_anext_vt, (void *)&g_avt.slots[0])
                == (void *)&g_avt.slots[0]) {
            g_ainstalled = 0;
        } else {
            r = XP_ERR_RACE;
        }
    }
out:
    g_alast_error = r;
    ReleaseSRWLockExclusive(&g_aapi);
    return r;
}

__declspec(dllexport) void xp_alloc_get_stats(XpAllocStats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    sum_tc_stats(&out->fast_allocs, &out->fast_frees, &out->central_hits, &out->central_misses,
                 &out->live_blocks, &out->live_bytes, &out->active_threads, &out->exited_threads);
    out->fallback_allocs = __atomic_load_n(&g_afallback_allocs, __ATOMIC_RELAXED);
    out->fallback_frees = __atomic_load_n(&g_afallback_frees, __ATOMIC_RELAXED);
    out->arena_full = __atomic_load_n(&g_aarena_full, __ATOMIC_RELAXED);
    out->corrupt = __atomic_load_n(&g_acorrupt, __ATOMIC_RELAXED);
    uint32_t slabs = __atomic_load_n(&g_slabs_used, __ATOMIC_ACQUIRE);
    out->slabs_used = (int32_t)slabs;
    out->slabs_max = (int32_t)XP_ALLOC_MAX_SLABS;
    out->committed_bytes = (uint64_t)slabs << XP_ALLOC_SLAB_SHIFT;
    out->installed = g_ainstalled;
    out->active = (int32_t)__atomic_load_n(&g_aactive, __ATOMIC_ACQUIRE);
    out->chain_depth = g_achain_depth;
    out->last_error = g_alast_error;
}

/* ---- command sort ---- */

/* Code checked by FNV-1a 64 over the whole function before patching (game 2026.11.03): another hook, or a game
 * update, makes the install refuse. */
#define SORT_DEVICE_LEN 0x132         /* Kernel::Device.GatherCommands, 0x140227640 */
#define SORT_CONTEXT_LEN 0x21b        /* Kernel::Context.GatherCommands, 0x14023bcf0 */
#define SORT_MERGE_LEN 0x1ba          /* its merge sort, FUN_140216550 */
#define SORT_CALL_AT 0x99             /* Device.GatherCommands: call Context.GatherCommands (its only call) */
#define SORT_MERGE_CALL_AT 0x147      /* Context.GatherCommands: the first call to the merge sort */
static const uint64_t k_sort_device_fnv = 0xc46a74dfa8977747ull;
static const uint64_t k_sort_context_fnv = 0xc6eb060e292f1290ull;
static const uint64_t k_sort_merge_fnv = 0x04063b906cdac871ull;

/* A context's command list (Context+0x18 + list*0x18): +0x00 first block, +0x10 dword subtracted from the record
 * count, +0x14 dword block count. A block is 0x4000 bytes, 1024 records of 16 bytes; its qword at +0x3ff0 (the
 * first half of record 1023) points to the next block. */
#define CMD_LIST_AT 0x18u
#define CMD_LIST_STRIDE 0x18u
#define CMD_BLOCK_BYTES 0x4000u
#define CMD_PER_BLOCK 1024u
#define CMD_NEXT_AT 0x3ff0u
#define CMD_SMALL 64u                 /* below this, insertion sort */

typedef struct { uint64_t lo, hi; } Cmd;   /* the key is the low dword of lo */
static inline uint32_t cmd_key(const Cmd *c) { return (uint32_t)c->lo; }

typedef void *(*gather_fn)(void *ctx, uint32_t *out_count, uint32_t list, void *output, uint32_t remaining);

typedef struct XpSortStats {
    uint64_t calls, records, passes, extra_copies, small, fallbacks, verified, mismatches;
    int32_t installed, verify, last_error, version;
} XpSortStats;

static SRWLOCK g_sort_lock = SRWLOCK_INIT;
static gather_fn g_sort_original;
static uint8_t *g_sort_site;          /* the patched qword while installed */
static uint8_t *g_sort_page;          /* jump stub: written once, never freed */
static uint8_t *g_sort_page_for;      /* the call instruction g_sort_page was written for */
static uint64_t g_sort_saved, g_sort_patched;
static int32_t g_sort_installed, g_sort_last_error;
static volatile LONG g_sort_verify;
static uint64_t g_sort_calls, g_sort_records, g_sort_passes, g_sort_extra, g_sort_small, g_sort_fallbacks,
                g_sort_verified, g_sort_mismatches;
/* Per context and list: the last radix sort took an odd number of passes.
 * Indexed by ((ctx >> 3) & 31) << 7 | (list & 127): Context stride is 0x2f78 (>> 3 is 0x5ef, odd), so up to 32
 * contexts x 128 lists (DeviceDX11.PostTick gathers 87 lists across ~15 contexts) map with zero collisions. */
static volatile uint8_t g_sort_hint[4096];
static SRWLOCK g_sort_verify_lock = SRWLOCK_INIT;
static uint8_t *g_sort_verify_buf;
static size_t g_sort_verify_cap;

static inline void sort_count(uint64_t *p, uint64_t d) { __atomic_fetch_add(p, d, __ATOMIC_RELAXED); }

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* Stable: a record moves left only past strictly greater keys. */
static void cmd_insertion_sort(Cmd *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        Cmd x = a[i];
        uint32_t k = cmd_key(&x), j = i;
        while (j > 0 && cmd_key(&a[j - 1]) > k) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = x;
    }
}

/* One stable counting pass on the byte at shift. hist counts that byte over these very records. */
static void cmd_radix_pass(const Cmd *src, Cmd *dst, uint32_t n, const uint32_t *hist, int shift)
{
    uint32_t off[256], sum = 0;
    for (int b = 0; b < 256; b++) {
        off[b] = sum;
        sum += hist[b];
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = (cmd_key(&src[i]) >> shift) & 255;
        dst[off[b]++] = src[i];
    }
}

/* Context.GatherCommands, same results: output[0, count) is the list's records stably sorted by key and
 * output[count, blocks*1024) the rest of the last block, both byte for byte as the original leaves them. The
 * upper half of the region (output + blocks*0x4000) is scratch in both, with different contents; the caller
 * advances its cursor by count records and hands that space on. The original copies every block twice and merge
 * sorts; this copies once, into whichever half the radix passes will end in, and skips bytes all keys share. */
__declspec(dllexport) void *xp_gather_commands(void *ctx, uint32_t *out_count, uint32_t list, void *output, uint32_t remaining)
{
    uint8_t *l = (uint8_t *)ctx + CMD_LIST_AT + (uint64_t)list * CMD_LIST_STRIDE;
    uint32_t blocks = *(uint32_t *)(l + 0x14);
    *out_count = 0;
    if (blocks == 0)
        return NULL;
    uint32_t total = blocks * CMD_PER_BLOCK;
    uint32_t count = total - *(uint32_t *)(l + 0x10);
    if (blocks >= (1u << 18) || count > total) {   /* the original's 32-bit sizes wrap, or a negative count */
        sort_count(&g_sort_fallbacks, 1);
        return g_sort_original(ctx, out_count, list, output, remaining);
    }

    Cmd *low = (Cmd *)output, *high = low + total;
    uint32_t slot = (((uint32_t)((uintptr_t)ctx >> 3) & 31u) << 7) | (list & 127u);
    /* Small lists (< CMD_SMALL) never run radix passes and sort in place, so they always copy straight into low. */
    int odd_hint = count >= CMD_SMALL ? g_sort_hint[slot] : 0;
    Cmd *first = odd_hint ? high : low;   /* where the records are copied to */
    uint8_t *block = *(uint8_t **)l;
    for (uint32_t b = 0; b < blocks; b++) {
        memcpy(first + (size_t)b * CMD_PER_BLOCK, block, CMD_BLOCK_BYTES);
        if (b + 1 < blocks)
            block = *(uint8_t **)(block + CMD_NEXT_AT);
    }

    Cmd *result = first;
    int passes = 0;
    if (count < CMD_SMALL) {
        if (count > 1)
            cmd_insertion_sort(first, count);
        sort_count(&g_sort_small, 1);
    } else {
        /* Counted over the copy, so every pass sees exactly the records it was counted on. */
        uint32_t hist[4][256];
        memset(hist, 0, sizeof(hist));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t k = cmd_key(&first[i]);
            hist[0][k & 255]++;
            hist[1][(k >> 8) & 255]++;
            hist[2][(k >> 16) & 255]++;
            hist[3][k >> 24]++;
        }
        uint32_t k0 = cmd_key(&first[0]);
        Cmd *src = first, *dst = first == low ? high : low;
        for (int d = 0; d < 4; d++) {
            if (hist[d][(k0 >> (8 * d)) & 255] == count)
                continue;                                 /* every key has this byte: the pass would not move anything */
            cmd_radix_pass(src, dst, count, hist[d], 8 * d);
            Cmd *t = src;
            src = dst;
            dst = t;
            passes++;
        }
        result = src;
        g_sort_hint[slot] = (uint8_t)(passes & 1);
    }

    if (result != low) {
        /* The guess of the parity was wrong (or there was nothing to sort in the upper half): one more copy. The
         * rest of the last block is still where the records were first copied to. */
        memcpy(low, result, (size_t)count * sizeof(Cmd));
        if (first != low)
            memcpy(low + count, first + count, (size_t)(total - count) * sizeof(Cmd));
        if (count > 1)
            sort_count(&g_sort_extra, 1);
    } else if (first != low) {
        memcpy(low + count, first + count, (size_t)(total - count) * sizeof(Cmd));
    }
    *out_count = count;
    sort_count(&g_sort_calls, 1);
    sort_count(&g_sort_records, count);
    sort_count(&g_sort_passes, (uint64_t)passes);

    if (g_sort_verify) {
        size_t need = (size_t)total * sizeof(Cmd) * 2;
        AcquireSRWLockExclusive(&g_sort_verify_lock);
        if (g_sort_verify) {
            if (g_sort_verify_cap < need) {
                if (g_sort_verify_buf)
                    VirtualFree(g_sort_verify_buf, 0, MEM_RELEASE);
                size_t cap = (need + 0xffff) & ~(size_t)0xffff;
                g_sort_verify_buf = (uint8_t *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                g_sort_verify_cap = g_sort_verify_buf ? cap : 0;
            }
            if (g_sort_verify_buf) {
                uint32_t original_count = 0;
                void *r = g_sort_original(ctx, &original_count, list, g_sort_verify_buf, remaining);
                int same = r == g_sort_verify_buf && original_count == count
                           && memcmp(g_sort_verify_buf, low, (size_t)total * sizeof(Cmd)) == 0;
                sort_count(&g_sort_verified, 1);
                if (!same)
                    sort_count(&g_sort_mismatches, 1);
            }
        }
        ReleaseSRWLockExclusive(&g_sort_verify_lock);
    }
    return output;
}

/* device_gather: Device.GatherCommands; context_gather: Context.GatherCommands. Points the one call between them
 * at xp_gather_commands. */
__declspec(dllexport) int xp_sort_install(void *device_gather, void *context_gather)
{
    int r;
    AcquireSRWLockExclusive(&g_sort_lock);
    uint8_t *dev = (uint8_t *)device_gather, *fn = (uint8_t *)context_gather;
    uint8_t *call = dev + SORT_CALL_AT;
    uint8_t *site = (uint8_t *)((uintptr_t)(call + 1) & ~(uintptr_t)7);
    if (g_sort_installed) { r = XP_ERR_STATE; goto out; }
    if (!dev || !fn || site != (uint8_t *)((uintptr_t)(call + 4) & ~(uintptr_t)7)) { r = XP_ERR_ARGS; goto out; }
    if (fnv64(dev, SORT_DEVICE_LEN) != k_sort_device_fnv || fnv64(fn, SORT_CONTEXT_LEN) != k_sort_context_fnv
        || call[0] != 0xe8 || rel32_target(call + 5, call + 1) != fn || fn[SORT_MERGE_CALL_AT] != 0xe8
        || fnv64(rel32_target(fn + SORT_MERGE_CALL_AT + 5, fn + SORT_MERGE_CALL_AT + 1), SORT_MERGE_LEN) != k_sort_merge_fnv) {
        r = XP_ERR_BYTES;
        goto out;
    }
    if (g_sort_page && g_sort_page_for != call) { r = XP_ERR_STATE; goto out; }
    if (!g_sort_page) {
        uint8_t *page = (uint8_t *)alloc_near(call, 4096);
        int32_t probe;
        if (!page || !near_rel32(call + 5, page, &probe)) {
            if (page)
                VirtualFree(page, 0, MEM_RELEASE);
            r = XP_ERR_NEAR;
            goto out;
        }
        /* jmp [rip+0]; dq xp_gather_commands: the caller's return address stays on the stack */
        uint64_t dest = (uint64_t)(uintptr_t)&xp_gather_commands;
        page[0] = 0xff; page[1] = 0x25; memset(page + 2, 0, 4); memcpy(page + 6, &dest, 8);
        DWORD old;
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), page, 4096);
        g_sort_page_for = call;
        g_sort_page = page;
    }
    int32_t rel32;
    if (!near_rel32(call + 5, g_sort_page, &rel32)) { r = XP_ERR_NEAR; goto out; }
    uint64_t saved;
    memcpy(&saved, site, 8);   /* checked above; the compare-exchange fails if it changed since */
    uint8_t bytes[8];
    memcpy(bytes, &saved, 8);
    memcpy(bytes + (call + 1 - site), &rel32, 4);   /* only the call's displacement changes */
    uint64_t patched;
    memcpy(&patched, bytes, 8);
    g_sort_original = (gather_fn)(void *)fn;
    r = write_qword(site, saved, patched);
    if (r == XP_OK) {
        g_sort_site = site;
        g_sort_saved = saved;
        g_sort_patched = patched;
        g_sort_installed = 1;
    }
out:
    g_sort_last_error = r;
    ReleaseSRWLockExclusive(&g_sort_lock);
    return r;
}

/* Threads already inside xp_gather_commands finish there: the DLL and the stub page stay. */
__declspec(dllexport) int xp_sort_uninstall(void)
{
    int r;
    AcquireSRWLockExclusive(&g_sort_lock);
    if (!g_sort_installed) {
        r = XP_ERR_STATE;
    } else {
        r = write_qword(g_sort_site, g_sort_patched, g_sort_saved);
        if (r == XP_OK)
            g_sort_installed = 0;
    }
    g_sort_last_error = r;
    ReleaseSRWLockExclusive(&g_sort_lock);
    return r;
}

__declspec(dllexport) void xp_sort_set_verify(int on)
{
    InterlockedExchange(&g_sort_verify, on ? 1 : 0);
    if (!on) {
        AcquireSRWLockExclusive(&g_sort_verify_lock);
        if (!g_sort_verify && g_sort_verify_buf) {
            VirtualFree(g_sort_verify_buf, 0, MEM_RELEASE);
            g_sort_verify_buf = NULL;
            g_sort_verify_cap = 0;
        }
        ReleaseSRWLockExclusive(&g_sort_verify_lock);
    }
}

__declspec(dllexport) void xp_sort_get_stats(XpSortStats *out)
{
    if (!out)
        return;
    out->calls = __atomic_load_n(&g_sort_calls, __ATOMIC_RELAXED);
    out->records = __atomic_load_n(&g_sort_records, __ATOMIC_RELAXED);
    out->passes = __atomic_load_n(&g_sort_passes, __ATOMIC_RELAXED);
    out->extra_copies = __atomic_load_n(&g_sort_extra, __ATOMIC_RELAXED);
    out->small = __atomic_load_n(&g_sort_small, __ATOMIC_RELAXED);
    out->fallbacks = __atomic_load_n(&g_sort_fallbacks, __ATOMIC_RELAXED);
    out->verified = __atomic_load_n(&g_sort_verified, __ATOMIC_RELAXED);
    out->mismatches = __atomic_load_n(&g_sort_mismatches, __ATOMIC_RELAXED);
    out->installed = g_sort_installed;
    out->verify = g_sort_verify;
    out->last_error = g_sort_last_error;
    out->version = XP_VERSION;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(inst);
    return TRUE;
}
