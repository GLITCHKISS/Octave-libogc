// HeapCheck_Dolphin.cpp -- OctHeapCheck: the heap and the GPU's region checked, for a test build (GameCube).
//
// WHY. A corrupted heap shows up far from what corrupted it: a texture turned to noise, a crash in malloc minutes
// later. Called once a frame (or around a suspect), this finds the damage at the first check after it happened:
// every chunk of newlib's heap walked from its start to its top (each chunk's size sane, each free one's foot and
// links), its free lists, and the GPU region's book (GxGpuCheck: its blocks tiling it, the heap below it). The
// first failure is logged (HEAPFAIL: where, what, the chunk and the one before it, the words round it); after it,
// every check is false and quiet.
//
// HOW. newlib's mallocr as devkitPPC builds it: __malloc_av_ (the bins; av[2] the top chunk), __malloc_sbrk_base
// (the heap's start). Chunks: prev_size, size (bit 0: the chunk before is in use), then fd/bk when free. The walk
// holds the heap's lock; what it found is logged after letting go of it (logging allocates). ~20,000 chunks a walk:
// a millisecond or so.
//
// USE. From C++: OctHeapCheck("where"); from Lua: System.HeapCheck("where"). True: all sound (always, off the
// GameCube). Not a release-build thing: it's a walk of the whole heap.

#if PLATFORM_DOLPHIN

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <sys/reent.h>

#include "Log.h"
#if API_GX
#include "Graphics/GX/GxUtils.h"
#endif

#if PLATFORM_GAMECUBE
extern "C"
{
    extern char* __malloc_sbrk_base;
    extern void* __malloc_av_[];
    void __malloc_lock(struct _reent* r);
    void __malloc_unlock(struct _reent* r);
}

namespace
{
    struct Chunk { uint32_t mPrevSize; uint32_t mSize; Chunk* mFd; Chunk* mBk; };
    const uint32_t kPrevInUse = 1u;
    const uint32_t kSizeMask = ~3u;
    const uint32_t kFencepost = 4u | kPrevInUse;    // (where a later sbrk didn't follow on: the walk stops there)
    const int kBins = 128;

    bool sFailed = false;
    uint32_t sAlign = 0;                            // malloc's alignment, 8 or 16: whichever walks cleanly
    uint32_t sChecks = 0;

    // what the walk found, kept under the lock and logged after
    struct Found
    {
        const char* mWhy = nullptr;
        uintptr_t mBad = 0, mBefore = 0;
        uint32_t mBeforeSize = 0;
        uint32_t mChunks = 0;
        int32_t mBin = -1;
        uint32_t mWords[16] = {};
        uint32_t mBeforeWords[8] = {};
    };

    Chunk* Bin(int i) { return (Chunk*)((char*)&__malloc_av_[2 * i + 2] - 8); }
    Chunk* Top() { return (Chunk*)__malloc_av_[2]; }
    bool InBins(const void* p) { return p >= (const void*)Bin(0) && p <= (const void*)Bin(kBins - 1); }
    bool InRam(uintptr_t a) { return a >= 0x80000000u && a + 4 <= 0x81800000u; }

    Chunk* First(uint32_t align)
    {
        const uintptr_t b = (uintptr_t)__malloc_sbrk_base;
        if (b == (uintptr_t)-1 || b == 0) return nullptr;
        const uintptr_t mem = (b + 8 + align - 1) & ~(uintptr_t)(align - 1);   // (its user part aligned)
        return (Chunk*)(mem - 8);
    }

    bool Valid(const Chunk* p, const Chunk* first, const Chunk* top)
    {
        return InBins(p) || (p >= first && p <= top && ((uintptr_t)p & 7) == 0);
    }

    void Copy(uint32_t* dst, uint32_t count, uintptr_t from)
    {
        from &= ~(uintptr_t)3;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t a = from + i * 4;
            dst[i] = InRam(a) ? *(volatile uint32_t*)a : 0xdeadbeefu;
        }
    }

    bool Fail(Found& f, const char* why, const Chunk* bad, const Chunk* before, uint32_t beforeSize)
    {
        f.mWhy = why;
        f.mBad = (uintptr_t)bad;
        f.mBefore = (uintptr_t)before;
        f.mBeforeSize = beforeSize;
        Copy(f.mWords, 16, (uintptr_t)bad - 16);
        if (before != nullptr) Copy(f.mBeforeWords, 8, (uintptr_t)before);
        return false;
    }

    // the whole heap: chunk by chunk from its start to its top, then its free lists. False: broken (f says how)
    bool Walk(uint32_t align, Found& f)
    {
        Chunk* first = First(align);
        Chunk* top = Top();
        if (first == nullptr || InBins(top)) return true;           // (nothing taken from the heap yet)
        Chunk* p = first;
        Chunk* prev = nullptr;
        uint32_t prevSize = 0, n = 0;
        while (p != top)
        {
            if (p > top || !InRam((uintptr_t)p + 8)) return Fail(f, "walked past the heap's top", p, prev, prevSize);
            const uint32_t size = p->mSize & kSizeMask;
            if (p->mSize == kFencepost) break;                          // (a gap in the heap: as far as it goes)
            if (size < 16 || (size & 7) || (uintptr_t)p + size > (uintptr_t)top)
            {
                return Fail(f, "a chunk's size is wrong", p, prev, prevSize);
            }
            Chunk* next = (Chunk*)((uintptr_t)p + size);
            if (next != top && !(next->mSize & kPrevInUse))              // (p is free: its foot and its links)
            {
                if (next->mPrevSize != size) return Fail(f, "a free chunk's foot doesn't match its size", p, prev, prevSize);
                if (!Valid(p->mFd, first, top) || !Valid(p->mBk, first, top) || p->mFd->mBk != p || p->mBk->mFd != p)
                {
                    return Fail(f, "a free chunk's links are broken", p, prev, prevSize);
                }
            }
            prev = p; prevSize = size; p = next;
            if (++n > 4000000u) break;
        }
        f.mChunks = n;
        for (int i = 1; i < kBins; ++i)                                  // (the free lists, as mallinfo walks them)
        {
            Chunk* b = Bin(i);
            uint32_t k = 0;
            for (Chunk* q = b->mFd; q != b; q = q->mFd)
            {
                if (!Valid(q, first, top) || !Valid(q->mBk, first, top) || q->mBk->mFd != q)
                {
                    f.mBin = i;
                    return Fail(f, "a free list is broken", q, nullptr, 0);
                }
                if (++k > 1000000u) break;
            }
        }
        return true;
    }
}
#endif

bool OctHeapCheck(const char* where)
{
#if PLATFORM_GAMECUBE
    if (sFailed)
    {
        return false;
    }
    Found f;
    bool ok = true;
    __malloc_lock(_REENT);
    if (sAlign == 0)
    {
        Found probe;
        if (Walk(8, probe)) sAlign = 8;
        else if (Walk(16, probe)) sAlign = 16;
        if (sAlign == 0) { ok = Walk(8, f); if (ok) sAlign = 8; }   // (broken already, or not this malloc: say so)
    }
    if (ok && sAlign != 0)
    {
        ok = Walk(sAlign, f);
    }
    __malloc_unlock(_REENT);

    uint32_t gpuAddr = 0, gpuIndex = 0;
    const char* gpuWhy = nullptr;
#if API_GX
    if (ok)
    {
        gpuWhy = GxGpuCheck(&gpuAddr, &gpuIndex);
        ok = (gpuWhy == nullptr);
    }
#endif
    sChecks++;
    if (ok)
    {
        if (sChecks == 1)
        {
            LogWarning("HEAPCHECK on: heap from %08x (aligned %u), %u chunks", (unsigned)(uintptr_t)__malloc_sbrk_base,
                       (unsigned)sAlign, (unsigned)f.mChunks);
        }
        return true;
    }

    sFailed = true;
    if (gpuWhy != nullptr)
    {
        LogWarning("HEAPFAIL at %s (check %u): GPU region: %s (address %08x, block %u)", where, (unsigned)sChecks, gpuWhy,
                   (unsigned)gpuAddr, (unsigned)gpuIndex);
        return false;
    }
    LogWarning("HEAPFAIL at %s (check %u): %s, chunk %08x%s", where, (unsigned)sChecks, f.mWhy, (unsigned)f.mBad,
               f.mBin >= 0 ? " (in a free list)" : "");
    if (f.mBefore != 0)
    {
        LogWarning("HEAPFAIL the chunk before: %08x size %u (user %u bytes at %08x): %08x %08x %08x %08x %08x %08x %08x %08x",
                   (unsigned)f.mBefore, (unsigned)f.mBeforeSize, (unsigned)(f.mBeforeSize - 4), (unsigned)(f.mBefore + 8),
                   (unsigned)f.mBeforeWords[0], (unsigned)f.mBeforeWords[1], (unsigned)f.mBeforeWords[2],
                   (unsigned)f.mBeforeWords[3], (unsigned)f.mBeforeWords[4], (unsigned)f.mBeforeWords[5],
                   (unsigned)f.mBeforeWords[6], (unsigned)f.mBeforeWords[7]);
    }
    for (int i = 0; i < 16; i += 8)
    {
        LogWarning("HEAPFAIL round it %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                   (unsigned)((f.mBad - 16 + i * 4) & ~(uintptr_t)3), (unsigned)f.mWords[i], (unsigned)f.mWords[i + 1],
                   (unsigned)f.mWords[i + 2], (unsigned)f.mWords[i + 3], (unsigned)f.mWords[i + 4], (unsigned)f.mWords[i + 5],
                   (unsigned)f.mWords[i + 6], (unsigned)f.mWords[i + 7]);
    }
    return false;
#else
    (void)where;
    return true;
#endif
}

#endif
