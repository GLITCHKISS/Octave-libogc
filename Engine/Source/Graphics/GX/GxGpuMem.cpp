// GxGpuMem.cpp -- the GPU's big blocks from their own region of the arena: textures' texels, meshes' display lists
// (GameCube; the Wii's from the heap).
//
// WHY. A 24 MB console that goes from scene to scene frees and loads megabytes of textures each time, and with one
// heap the small blocks made while a scene ran (a node's name, a Lua table, a list grown) stay where they were put:
// among the texture memory. After a big scene the heap had megabytes free in holes of a few hundred KB, and the next
// scene's 512 KB-2 MB textures had nowhere to go -- worse with every change of scene.
//
// WHAT. Texels and display lists come from a region at the top of the arena, grown down as it needs, the heap growing up from
// below: the two never mix. A freed block joins the free space beside it; the region's bottom, free, goes back to
// the heap (the arena's top raised past it). A scene's textures, gone together, leave one piece.
//
// SAFE. Only the engine's own GPU blocks come here (GxGpuAlloc; freed by GxGpuFree, GxDeferFree's) -- not malloc: the heap, newlib's and
// libstdc++'s allocations are untouched. The region never takes the arena's last kHeapReserve from the heap, and
// when it can't fit a block the caller takes it from the heap as before. (Mesh vertex arrays stay in the heap:
// freed in too many places, and read by the collision builder.)

#if API_GX && PLATFORM_DOLPHIN

#include "Graphics/GX/GxUtils.h"

#include <malloc.h>

#if !PLATFORM_GAMECUBE
// (the Wii: its heap goes on into MEM2 past MEM1's top -- left as it is, the texels from the heap)
void* GxGpuAlloc(uint32_t) { return nullptr; }
void* GxGpuAllocTransient(uint32_t size) { return memalign(32, size); }
void GxGpuRegionEnable(bool) {}
bool GxGpuFree(void*) { return false; }
size_t GxGpuFreeBytes() { return 0; }
const char* GxGpuCheck(uint32_t*, uint32_t*) { return nullptr; }
#else

#include <ogc/system.h>
#include <string.h>
#include <sys/reent.h>

extern "C"
{
void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
}

namespace
{
    const uint32_t kMaxBlocks = 2048;               // used and free together (a big scene: several hundred)
    const uintptr_t kAlign = 32;                    // GX's textures and display lists
    const uint32_t kHeapReserve = 256 * 1024;       // the arena the heap always keeps to grow into
    const uint32_t kTransientReserve = 32 * 1024;   // the least it keeps for a short-lived block (GxGpuAllocTransient)
    const uint32_t kMinBlock = 4 * 1024;            // smaller ones from the heap (not worth a block)

    struct Block
    {
        uintptr_t mAddr;
        uint32_t mSize;
        uint32_t mUsed;
    };

    bool sEnabled = true;                           // (GxGpuRegionEnable)
    Block sBlocks[kMaxBlocks];                      // address order, tiling sLo..sTop
    uint32_t sNumBlocks = 0;
    uintptr_t sTop = 0;                             // the arena's top when the first block came (0: not yet)
    volatile uintptr_t sLo = 0;                     // the region's bottom: the arena's top now

    // the heap's own lock: it is also what keeps sbrk (which reads the arena's top) out while that moves
    struct Lock
    {
        Lock() { __malloc_lock(_REENT); }
        ~Lock() { __malloc_unlock(_REENT); }
    };

    void Insert(uint32_t at, uintptr_t addr, uint32_t size, uint32_t used)
    {
        memmove(&sBlocks[at + 1], &sBlocks[at], (sNumBlocks - at) * sizeof(Block));
        sBlocks[at].mAddr = addr;
        sBlocks[at].mSize = size;
        sBlocks[at].mUsed = used;
        sNumBlocks++;
    }

    void Remove(uint32_t at)
    {
        memmove(&sBlocks[at], &sBlocks[at + 1], (sNumBlocks - at - 1) * sizeof(Block));
        sNumBlocks--;
    }

    int32_t Find(uintptr_t addr)
    {
        int32_t lo = 0, hi = int32_t(sNumBlocks) - 1;
        while (lo <= hi)
        {
            const int32_t mid = (lo + hi) / 2;
            if (sBlocks[mid].mAddr == addr) return mid;
            if (sBlocks[mid].mAddr < addr) lo = mid + 1; else hi = mid - 1;
        }
        return -1;
    }
}

// Off: every block from the heap, as before the region (blocks already in it are still freed there)
void GxGpuRegionEnable(bool enable)
{
    sEnabled = enable;
}

namespace
{
    // A block of n bytes (32-aligned) from the region, with the lock held: the smallest free block it fits (from that
    // block's top, so what's left stays by the free space below it), else -- grow true -- the region grown down into
    // the arena, `reserve` of it left to the heap. Null when neither can.
    void* RegionTake(uint32_t n, bool grow, uint32_t reserve)
    {
        if (sTop == 0)
        {
            sTop = (uintptr_t)SYS_GetArena1Hi() & ~(kAlign - 1);
            sLo = sTop;
            SYS_SetArena1Hi((void*)sLo);
        }

        int32_t best = -1;
        for (uint32_t i = 0; i < sNumBlocks; ++i)
        {
            if (!sBlocks[i].mUsed && sBlocks[i].mSize >= n && (best < 0 || sBlocks[i].mSize < sBlocks[best].mSize))
            {
                best = int32_t(i);
            }
        }
        if (best >= 0)
        {
            Block& b = sBlocks[best];
            if (b.mSize - n < 1024 || sNumBlocks >= kMaxBlocks)
            {
                b.mUsed = 1;                        // (all of it: a sliver is no use to anyone)
                return (void*)b.mAddr;
            }
            const uintptr_t addr = b.mAddr + b.mSize - n;
            b.mSize -= n;
            Insert(uint32_t(best) + 1, addr, n, 1);
            return (void*)addr;
        }

        if (!grow || sNumBlocks >= kMaxBlocks)
        {
            return nullptr;
        }
        const uintptr_t heapTop = (uintptr_t)SYS_GetArena1Lo();
        if (sLo < heapTop + reserve + n)
        {
            return nullptr;
        }
        const uintptr_t newLo = sLo - n;
        SYS_SetArena1Hi((void*)newLo);              // (the heap's sbrk stops below it from now on)
        sLo = newLo;
        Insert(0, newLo, n, 1);                     // (the bottom block is never a free one: see GxGpuFree)
        return (void*)newLo;
    }
}

void* GxGpuAlloc(uint32_t size)
{
    if (!sEnabled || size < kMinBlock || size > 0x7fffffffu)
    {
        return nullptr;
    }
    Lock lock;
    return RegionTake(uint32_t((size + kAlign - 1) & ~(kAlign - 1)), true, kHeapReserve);
}

// A block wanted for a short while (a texture brought back from ARAM for a menu: Texture::Restore), freed with
// GxGpuFree or, if it came from the heap, free -- GxDeferFree does either. Taken from wherever costs the heap least,
// in order: a hole already free in the region (no use to the heap); the heap's own free memory (its holes: memalign);
// the region grown with only kTransientReserve left to the heap. In play, Lua's peaks had taken the arena, leaving
// the heap 0.7-1.2 MB of holes and the region no room to grow -- with GxGpuAlloc first, a pause menu's 960 KB came
// back from ARAM only part way, or not at all (the GameCube's log, 2026-11-02), though the holes would have held most
// of it; and growing the region took the arena Lua needed next. (The game is held while a menu is up: Lua makes next
// to nothing then, so the heap can spare more of its reserve for the moment.)
void* GxGpuAllocTransient(uint32_t size)
{
    if (size == 0 || size > 0x7fffffffu)
    {
        return nullptr;
    }
    const uint32_t n = uint32_t((size + kAlign - 1) & ~(kAlign - 1));
    if (sEnabled && n >= kMinBlock)
    {
        Lock lock;
        void* block = RegionTake(n, false, 0);
        if (block != nullptr)
        {
            return block;
        }
    }
    void* block = memalign(kAlign, n);              // (the heap's holes, or its top: as before the region)
    if (block != nullptr || !sEnabled || n < kMinBlock)
    {
        return block;
    }
    Lock lock;
    return RegionTake(n, true, kTransientReserve);
}

bool GxGpuFree(void* block)
{
    const uintptr_t addr = (uintptr_t)block;
    if (block == nullptr || sTop == 0 || addr < sLo || addr >= sTop)
    {
        return false;                               // (the heap's: the region's live blocks are all at or above sLo)
    }

    Lock lock;
    int32_t i = Find(addr);
    if (i < 0 || !sBlocks[i].mUsed)
    {
        return true;                                // (not a block's start, or freed twice: left alone)
    }
    sBlocks[i].mUsed = 0;
    if (uint32_t(i) + 1 < sNumBlocks && !sBlocks[i + 1].mUsed)
    {
        sBlocks[i].mSize += sBlocks[i + 1].mSize;
        Remove(uint32_t(i) + 1);
    }
    if (i > 0 && !sBlocks[i - 1].mUsed)
    {
        sBlocks[i - 1].mSize += sBlocks[i].mSize;
        Remove(uint32_t(i));
        --i;
    }
    if (i == 0)
    {
        // its bottom free: back to the heap below (the arena's top raised past it)
        sLo = sBlocks[0].mAddr + sBlocks[0].mSize;
        SYS_SetArena1Hi((void*)sLo);
        Remove(0);
    }
    return true;
}

// The region's book checked (OctHeapCheck, HeapCheck_Dolphin.cpp): its blocks in address order tiling sLo..sTop with
// no gap or overlap, each a 32-byte multiple, no two free side by side, its bottom block in use, the arena's top at
// its bottom and the heap's below it. Null if all is well; else what's wrong, and where (outAddr) and which block
// (outIndex).
const char* GxGpuCheck(uint32_t* outAddr, uint32_t* outIndex)
{
    Lock lock;
    *outAddr = 0;
    *outIndex = 0;
    if (sTop == 0)
    {
        return nullptr;                             // (no block yet)
    }
    if ((uintptr_t)SYS_GetArena1Hi() != sLo)
    {
        *outAddr = (uint32_t)(uintptr_t)SYS_GetArena1Hi();
        return "the arena's top isn't the region's bottom";
    }
    if ((uintptr_t)SYS_GetArena1Lo() > sLo)
    {
        *outAddr = (uint32_t)(uintptr_t)SYS_GetArena1Lo();
        return "the heap's top is above the region's bottom";
    }
    if (sNumBlocks > kMaxBlocks)
    {
        *outIndex = sNumBlocks;
        return "more blocks than the book holds";
    }
    uintptr_t at = sLo;
    for (uint32_t i = 0; i < sNumBlocks; ++i)
    {
        const Block& b = sBlocks[i];
        *outIndex = i;
        *outAddr = (uint32_t)b.mAddr;
        if (b.mAddr != at) return "a gap or an overlap between blocks";
        if (b.mSize == 0 || (b.mSize & (kAlign - 1)) != 0) return "a block's size is wrong";
        if (b.mUsed > 1) return "a block's used flag is garbage";
        if (i == 0 && !b.mUsed) return "the bottom block is free";
        if (i > 0 && !b.mUsed && !sBlocks[i - 1].mUsed) return "two free blocks side by side";
        at = b.mAddr + b.mSize;
        if (at > sTop) return "a block runs past the region's top";
    }
    if (at != sTop)
    {
        *outAddr = (uint32_t)at;
        return "the blocks don't reach the region's top";
    }
    return nullptr;
}

// The region's free blocks (inside it, not its bottom: that's the heap's again): free memory all the same
size_t GxGpuFreeBytes()
{
    Lock lock;
    size_t total = 0;
    for (uint32_t i = 0; i < sNumBlocks; ++i)
    {
        if (!sBlocks[i].mUsed) total += sBlocks[i].mSize;
    }
    return total;
}

#endif  // PLATFORM_GAMECUBE

#endif
