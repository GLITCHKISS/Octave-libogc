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

#if !PLATFORM_GAMECUBE
// (the Wii: its heap goes on into MEM2 past MEM1's top -- left as it is, the texels from the heap)
void* GxGpuAlloc(uint32_t) { return nullptr; }
bool GxGpuFree(void*) { return false; }
size_t GxGpuFreeBytes() { return 0; }
#else

#include <ogc/system.h>
#include <malloc.h>
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
    const uint32_t kMinBlock = 4 * 1024;            // smaller ones from the heap (not worth a block)

    struct Block
    {
        uintptr_t mAddr;
        uint32_t mSize;
        uint32_t mUsed;
    };

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

void* GxGpuAlloc(uint32_t size)
{
    if (size < kMinBlock || size > 0x7fffffffu)
    {
        return nullptr;
    }
    const uint32_t n = uint32_t((size + kAlign - 1) & ~(kAlign - 1));

    Lock lock;
    if (sTop == 0)
    {
        sTop = (uintptr_t)SYS_GetArena1Hi() & ~(kAlign - 1);
        sLo = sTop;
        SYS_SetArena1Hi((void*)sLo);
    }

    // the smallest free block it fits, from that block's top (what's left stays by the free space below it)
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
            b.mUsed = 1;                            // (all of it: a sliver is no use to anyone)
            return (void*)b.mAddr;
        }
        const uintptr_t addr = b.mAddr + b.mSize - n;
        b.mSize -= n;
        Insert(uint32_t(best) + 1, addr, n, 1);
        return (void*)addr;
    }

    // else the region grown down into the arena, the heap's reserve left to it
    if (sNumBlocks >= kMaxBlocks)
    {
        return nullptr;
    }
    const uintptr_t heapTop = (uintptr_t)SYS_GetArena1Lo();
    if (sLo < heapTop + kHeapReserve + n)
    {
        return nullptr;
    }
    const uintptr_t newLo = sLo - n;
    SYS_SetArena1Hi((void*)newLo);                  // (the heap's sbrk stops below it from now on)
    sLo = newLo;
    Insert(0, newLo, n, 1);                         // (the bottom block is never a free one: see GxGpuFree)
    return (void*)newLo;
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
