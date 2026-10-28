#if PLATFORM_DOLPHIN

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"

#include "Assets/SoundWave.h"
#include "Log.h"
#include "Profiler.h"
#include "Maths.h"

#include "System/System.h"
#include "Engine.h"

#include <gccore.h>
#include <asndlib.h>
#include <ogc/semaphore.h>
#include <ogc/lwp_watchdog.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include "vorbis/vorbisfile.h"
#include "EngineFeatures.h"
#if !OCT_VORBIS
// Built without Vorbis (EngineFeatures.h): a streamed sound never opens (the
// error is logged where it's opened), so none of vorbisfile is linked.
#define ov_open_callbacks(source, vf, initial, ibytes, callbacks) ((void)(vf), -1)
#define ov_read(vf, buffer, length, bigendian, word, sgned, bitstream) ((void)(vf), 0L)
static inline int OvNoSeek(OggVorbis_File* vf) { (void)vf; return -1; }   // (a value, for a restart's check)
#define ov_pcm_seek(vf, pos) OvNoSeek(vf)
#define ov_pcm_total(vf, i) ((void)(vf), (ogg_int64_t)-1)
#define ov_clear(vf) ((void)(vf))
#endif

static int32_t sSampleRates[AUDIO_MAX_VOICES] = {};

// ---------------------------------------------------------------------------
// Streaming (decode-on-demand) voices
//
// A SoundWave flagged Stream keeps its compressed Vorbis in RAM instead of
// decoding to full PCM. Here we open it with vorbisfile (over an in-RAM buffer)
// and decode it a chunk at a time into a triple PCM buffer, feeding ASND via
// ASND_AddVoice. Decode runs on the audio decode thread (AudioDecodeThread) while
// the main thread waits on vsync, never in the audio interrupt; the main thread
// only hands decoded buffers to ASND (AUD_Update). A long track costs its
// compressed size (~KB) instead of MBs of PCM.
// ---------------------------------------------------------------------------

struct MemSource
{
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
};

// The same, over a range of a FILE: the compressed audio stays on the disc. vorbisfile asks for
// a few KB at a time, so reads go through one small window and most are served from it.
//
// The NEXT window is read ahead by the audio I/O thread (AudioIoThread) while this one plays.
// Read on the main thread, a window refill waited on the disc lock behind the background
// loader's reads (a streamed sky's 64 KB frames, many a second): measured on hardware, a
// 83-117 ms frame every second or two, with the rest of the game at 28-30 fps.
enum { AHEAD_EMPTY = 0, AHEAD_REQUESTED, AHEAD_READY };

struct DiscSource
{
    std::string path;
    uint32_t    base = 0;       // where the audio starts in the file
    uint32_t    size = 0;
    uint32_t    pos = 0;
    uint8_t*    window = nullptr;
    uint32_t    windowAt = 0;
    uint32_t    windowSize = 0;
    // Read-ahead. While REQUESTED, only the I/O thread touches `ahead`; READY hands it back.
    uint8_t*          ahead = nullptr;
    uint32_t          aheadAt = 0;
    uint32_t          aheadSize = 0;
    volatile int32_t  aheadState = AHEAD_EMPTY;
};

static const uint32_t kDiscWindowBytes = 16 * 1024;

// A disc stream's start, done by the audio I/O thread too. On the main thread, opening a track froze
// the game for about a second on hardware each time a new one started: vorbisfile reads the headers
// (a disc seek), seeks to the END of the file to find its length (another, across the disc), back to
// the start, and the first buffer is decoded -- with the loader's reads ahead of it on the disc lock.
// Now AUD_Play only asks; AUD_Update starts the voice once the thread has it open and a buffer full.
enum { OPEN_NONE = 0, OPEN_REQUESTED, OPEN_DONE, OPEN_FAILED };

// A looping disc stream's way back to its start, done by the I/O thread as well. ov_pcm_seek reads pages at
// scattered places in the file (each a disc read through DiscRead): on the main thread that was a 23-45 ms frame
// at every loop of a streamed track (AudRead in the console perf log -- the dive's wind, the shell's water, the
// Starman's theme). Now the main thread asks and stops decoding; AUD_Update goes on once it's back at the start.
enum { LOOP_NONE = 0, LOOP_REQUESTED, LOOP_DONE };

// Compressed bytes left in the current window under which the main thread stops decoding for the frame while the
// I/O thread is still reading the next one -- rather than reading it itself, a disc read on the main thread (the
// I/O thread runs below the main thread, so it reads while the main thread waits on vsync). A voice about to run
// dry still decodes at once.
static const uint32_t kDiscAheadMargin = 4 * 1024;

// The decoding itself, done by the audio decode thread (AudioDecodeThread). On the main thread, a slice of every playing
// stream decoded each frame was 0.6-1.4 ms a frame on hardware with two disc streams at once, and up to 2.8 ms, in frames
// with no time to spare -- while the same frames waited 4-9 ms on vsync. Now the main thread asks (DECODE_REQUESTED) and
// that thread fills the buffer being filled while the main thread waits; the main thread hands full buffers to ASND, as
// before. While REQUESTED only that thread touches vf, the disc source's window, the buffer being filled, `filled` and
// `eof`; DECODE_NONE hands them back. Its own thread, not the I/O thread's work: an open there holds that thread for up
// to a second, and the disc reads it waits on would hold the decoding up behind them.
enum { DECODE_NONE = 0, DECODE_REQUESTED };

// PCM decoded between the decode thread's looks at whether the main thread wants the stream back (a few packets:
// well under a millisecond).
static const uint32_t kDecodeUnitBytes = 4 * 1024;
// The decode thread's priorities. Below the main thread (64) it decodes while the main thread waits on vsync and the GPU.
// But a frame with almost no wait (1-2 ms of it a frame on hardware, gliding fast with two disc streams) starves it,
// and the voice runs out: so once a voice has less than kDecodeBoostLeadUs of audio left in ASND with the next buffer
// not decoded yet, the main thread raises it above itself (StreamBoostDecode) and it decodes that buffer there and
// then, and goes back down. 65: above the main thread, below the SD driver's reads (66).
static const u32 kDecodePriority = 56;
static const u32 kDecodeBoostPriority = 65;
static const uint64_t kDecodeBoostLeadUs = 150 * 1000;
// The last resort: under this much left and the decode thread still not done (in a disc read), the main thread takes
// the stream back and decodes the rest itself -- more than a decode and one ASND mix (21 ms) take.
static const uint64_t kDecodeMainLeadUs = 40 * 1000;
// How long the main thread waits for the decode thread to give a stream back (StreamTakeBack): the rest of a unit.
// Longer is the thread in a disc read: tried again next frame.
static const uint64_t kDecodeTakeBackUs = 2000;

static sem_t sAudioIoSem = LWP_SEM_NULL;
static lwp_t sAudioIoThread = LWP_THREAD_NULL;
static sem_t sAudioDecodeSem = LWP_SEM_NULL;
static lwp_t sAudioDecodeThread = LWP_THREAD_NULL;
static lwp_t sAudioTrackThread = LWP_THREAD_NULL;   // (GameCube: a streamed track's copy made in ARAM, AudioTrackThread)
// PCM the decode thread decoded since the main thread last looked (AUD_Update: the AudDecKB perf figure). Interrupts
// off around each change: one CPU.
static volatile uint32_t sDecodedBytes = 0;
// The decode thread raised above the main thread (StreamBoostDecode) and not yet back down. (Set by the main thread,
// cleared by the decode thread as it goes back down: while it's up, the main thread can't run.)
static volatile bool sDecodeBoosted = false;
static void RequestReadAhead(DiscSource* d);

struct StreamVoice
{
    bool           active = false;
    OggVorbis_File vf;
    MemSource      mem;
    DiscSource     disc;
    uint8_t*       memOwned = nullptr;   // private copy of the compressed Vorbis
    // THREE buffers, in rotation: one playing, one queued behind it, one being FILLED. With two,
    // a buffer could only be refilled once it had finished playing, so a whole buffer (0.2 s of
    // audio) was decoded in one go, five times a second, on the main thread: measured in Dolphin
    // with stereo Vorbis, that was a 24-27 ms frame every time and 54-58 fps; with no music the
    // same game held 59.9 with no frame over 17 ms. Now a small slice is decoded EVERY frame.
    uint8_t*       buf[3] = { nullptr, nullptr, nullptr };
    uint32_t       bufSize = 0;
    int            nextBuf = 0;         // the one being filled
    uint32_t       filled = 0;          // bytes of it decoded so far
    uint32_t       sliceBytes = 0;      // the least to decode in one frame
    uint32_t       bytesPerSec = 0;     // of decoded audio
    int            channels = 2;
    int            bytesPerSample = 1;   // 1 = 8-bit unsigned, 2 = 16-bit LE
    bool           loop = false;
    bool           eof = false;
    // A disc stream that is PCM already (SoundWave::GetDiscPcm, cooked uncompressed): copied from the disc window as it
    // is, no vf at all (vfOpen stays false: never opened, cleared or parked), its loop's start its first byte.
    bool           raw = false;
    // Opened by the I/O thread (OPEN_*): while REQUESTED only it touches vf, the disc source and
    // buf[0]; DONE hands them back with the first buffer decoded (openFilled bytes), and the voice
    // is set as AUD_Play asked (the open* fields).
    volatile int32_t openState = OPEN_NONE;
    bool           vfOpen = false;
    uint32_t       openFilled = 0;
    int32_t        openFormat = 0;
    int32_t        openPitchHz = 0;
    int32_t        openVolL = 0;
    int32_t        openVolR = 0;
    // Back to the start of a loop by the I/O thread (LOOP_*): while REQUESTED only it touches vf and the disc
    // source; DONE hands them back.
    volatile int32_t loopState = LOOP_NONE;
    // Stopped while the I/O thread still worked on it (opening, reading ahead, seeking a loop's start): the voice
    // left it, and that thread frees it once it's done (StreamStop, StreamTryRetire). Waiting for it held the main
    // thread up behind the disc ("AUDIO stop waited 25 ms" restarting the dive's wind).
    volatile bool  retiring = false;
    // Stopped, its decoder kept open a while (StreamPark) for the same sound to start again from its start without
    // being opened anew (StreamTakeCached): the main thread's alone. `restart`, set with OPEN_REQUESTED, tells the I/O
    // thread it is only to be seeked back to its start (as a loop is), not opened.
    bool           cached = false;
    uint64_t       cachedAtUs = 0;
    bool           restart = false;
    // Decoded by the decode thread (DECODE_*). `decodeYield` asks it to give the stream back at once (StreamTakeBack);
    // `decodeUrgent`, that the voice is nearly out: it decodes it above the main thread, and reads the disc itself
    // rather than wait on the I/O thread's read-ahead.
    volatile int32_t decodeState = DECODE_NONE;
    volatile bool  decodeYield = false;
    volatile bool  decodeUrgent = false;
    // Urgent: how much it decodes above the main thread before it goes back below it (StreamBoostBudget). A whole
    // buffer at once -- 0.2 s of two streams -- was a 17 ms frame on hardware: a hitch every time a glide with two
    // streams playing starved the thread, the character's moves jumping with it.
    volatile uint32_t decodeBudget = 0;
    // The main thread's: when the audio handed to ASND runs out, as it reckons it (StreamQueued: each buffer's length
    // at the voice's pitch, from when it was handed over or the one before it ends) -- a little early if anything, as
    // the mixer takes each up a moment late. And the pitch it plays at (Hz).
    uint64_t       playEndUs = 0;
    int32_t        pitchHz = 0;
};

// Two slots a voice, in turn: one can be left to the I/O thread to finish and free (retiring) while the voice
// goes on with the other at once. sStreams[v] is the voice's slot now, sSpares[v] its other (VoiceStream, StreamFor).
//
// And a slot set aside for a stopped disc stream's decoder (sCache, StreamPark): the dive's wind, stopped as it fades
// and started again at the next dive, was opened anew each time -- on the I/O thread, the headers parsed (every
// codebook's tables built) and three disc reads (the start, the END for the length, back), with a playing track's
// read-aheads waiting behind it; on the main thread, the whole decoder freed at each stop (ov_clear). Kept, it is
// only seeked back to its start. Slots move between voices and the cache by pointer: each is one voice's (current or
// spare) or the cache's. (The I/O thread goes by the array and the slots' states, never by these pointers.)
static const int32_t kStreamCacheSlots = 1;                    // (one decoder kept at most: memory)
static const int32_t kStreamSlots = AUDIO_MAX_VOICES * 2 + kStreamCacheSlots;
static const uint64_t kStreamCacheUs = 10 * 1000 * 1000;       // kept this long after it stopped, then freed
static StreamVoice sStreamSlots[kStreamSlots];
static StreamVoice* sStreams[AUDIO_MAX_VOICES] = {};
static StreamVoice* sSpares[AUDIO_MAX_VOICES] = {};
static StreamVoice* sCache[kStreamCacheSlots] = {};

static StreamVoice* VoiceStream(int32_t v)
{
    if (sStreams[v] == nullptr)
    {
        sStreams[v] = &sStreamSlots[v * 2];
        sSpares[v] = &sStreamSlots[v * 2 + 1];
    }
    return sStreams[v];
}

static StreamVoice* CacheSlot(int32_t c)
{
    if (sCache[c] == nullptr)
        sCache[c] = &sStreamSlots[AUDIO_MAX_VOICES * 2 + c];
    return sCache[c];
}
static size_t DiscRead(void* ptr, size_t size, size_t nmemb, void* ds);
static int DiscSeek(void* ds, ogg_int64_t off, int whence);
static long DiscTell(void* ds);
static int MemClose(void* ds);
static uint32_t StreamFill(StreamVoice* sv, uint8_t* dst, uint32_t want);
static void StreamFree(StreamVoice* sv);

// Whether the I/O thread (or the decode thread) has work in hand on this slot. (Interrupts off around it where it decides
// who frees it.)
static bool StreamIoBusy(const StreamVoice* sv)
{
    return sv->openState == OPEN_REQUESTED || sv->disc.aheadState == AHEAD_REQUESTED || sv->loopState == LOOP_REQUESTED ||
           sv->decodeState == DECODE_REQUESTED;
}

// The I/O thread, done with a slot its voice has left: freed here, once nothing is left in hand on it. It stays
// `retiring` till StreamFree has finished (cleared last), so the main thread can't take it back half freed.
static void StreamTryRetire(StreamVoice* sv)
{
    const u32 level = IRQ_Disable();
    const bool take = sv->retiring && !StreamIoBusy(sv);
    IRQ_Restore(level);
    if (take)
        StreamFree(sv);
}

// Serves read-ahead requests. Below the main thread's priority (see AUD_Initialize), so it runs
// while the main thread waits on the GPU and vsync, not in the middle of its work.
#if PLATFORM_GAMECUBE
extern "C" void OctSd_NoteThreadPriority(u32 prio);  // (SdGeckoDma.c: its card reads above the main thread)
#endif

// The read-aheads asked for (RequestReadAhead), each into its source's spare window. (The I/O thread's.)
static void ServeReadAheads()
{
    for (int32_t s = 0; s < kStreamSlots; ++s)
    {
        DiscSource* d = &sStreamSlots[s].disc;
        if (d->aheadState != AHEAD_REQUESTED)
            continue;

        const bool ok = SYS_ReadFileRange(d->path.c_str(), true, d->base + d->aheadAt, d->aheadSize, (char*)d->ahead);
        d->aheadState = ok ? AHEAD_READY : AHEAD_EMPTY;   // failed: the main thread reads it itself
    }
}

// A PCM stream back at its start (its loop): the start's window read here too, so the thread decoding it next has it.
static void StreamRawRewind(StreamVoice* sv)
{
    uint8_t first;
    sv->disc.pos = 0;
    if (DiscRead(&first, 1, 1, &sv->disc) == 1)
        sv->disc.pos = 0;                      // (the byte read only for the window: read again from it)
}

static void* AudioIoThread(void*)
{
#if PLATFORM_GAMECUBE
    OctSd_NoteThreadPriority(50);  // (as created in AUD_Initialize)
#endif
    while (true)
    {
        LWP_SemWait(sAudioIoSem);

        // A playing track's read-ahead before any start: a start holds this thread for a while (and runs only while
        // the main thread waits), and the track's window could run out behind it -- read on the main thread then.
        ServeReadAheads();

        for (int32_t s = 0; s < kStreamSlots; ++s)
        {
            StreamVoice* sv = &sStreamSlots[s];
            if (sv->openState != OPEN_REQUESTED)
                continue;
            if (sv->retiring)                  // (stopped before it was opened: no need to open it now)
            {
                if (!sv->restart)
                    sv->vfOpen = false;        // (a restart's decoder is open already: StreamFree clears it)
                __asm__ volatile("" ::: "memory");
                sv->openState = OPEN_FAILED;
                continue;
            }

            bool ok;
            if (sv->raw)                       // PCM: nothing to open (its start read below, into the first buffers)
                ok = true;
            else if (sv->restart)              // kept open (StreamPark): back to its start, as a loop goes back
                ok = (ov_pcm_seek(&sv->vf, 0) == 0);
            else
            {
                ov_callbacks dcb = { DiscRead, DiscSeek, MemClose, DiscTell };
                ok = (ov_open_callbacks(&sv->disc, &sv->vf, NULL, 0, dcb) >= 0);
                sv->vfOpen = ok;
            }
            if (ok)
            {
                sv->openFilled = StreamFill(sv, sv->buf[0], sv->bufSize);   // the first buffer whole
                DCFlushRange(sv->buf[0], sv->bufSize);
                // and the second, queued as soon as the voice starts. Left to AUD_Update, the voice had nothing
                // queued, so ASND_TestVoiceBufferReady said "about to run dry" and the main thread decoded the whole
                // buffer in one frame: ~25 ms, a dropped frame at every track's start (seen on hardware, a jolt).
                if (!sv->eof)
                {
                    sv->filled = StreamFill(sv, sv->buf[1], sv->bufSize);
                    if (sv->filled < sv->bufSize) sv->eof = true;
                }
            }
            __asm__ volatile("" ::: "memory");   // (all of it written before the main thread is told)
            sv->openState = ok ? OPEN_DONE : OPEN_FAILED;
        }

        // Read-aheads first: a loop's seek below then finds them done (one asked for near the file's end is of no
        // more use, and is dropped by DiscRead), and asks for the window after the start -- read in the second pass,
        // before the main thread needs it.
        ServeReadAheads();

        for (int32_t s = 0; s < kStreamSlots; ++s)
        {
            StreamVoice* sv = &sStreamSlots[s];
            if (sv->loopState != LOOP_REQUESTED)
                continue;
            if (sv->vfOpen)
                ov_pcm_seek(&sv->vf, 0);       // (its reads through DiscRead, here: none on the main thread)
            else if (sv->raw)
                StreamRawRewind(sv);
            __asm__ volatile("" ::: "memory");
            sv->loopState = LOOP_DONE;
        }

        ServeReadAheads();

        for (int32_t s = 0; s < kStreamSlots; ++s)
            StreamTryRetire(&sStreamSlots[s]);
    }
    return nullptr;
}

// ov_callbacks over an in-RAM compressed buffer.
static size_t MemRead(void* ptr, size_t size, size_t nmemb, void* ds)
{
    MemSource* m = (MemSource*)ds;
    size_t want = size * nmemb;
    size_t avail = m->size - m->pos;
    if (want > avail) want = avail;
    if (want > 0) memcpy(ptr, m->data + m->pos, want);
    m->pos += (uint32_t)want;
    return (size > 0) ? (want / size) : 0;
}
static int MemSeek(void* ds, ogg_int64_t off, int whence)
{
    MemSource* m = (MemSource*)ds;
    int64_t np = (whence == SEEK_CUR) ? (int64_t)m->pos + off
               : (whence == SEEK_END) ? (int64_t)m->size + off
               : off;
    if (np < 0) np = 0;
    if (np > (int64_t)m->size) np = m->size;
    m->pos = (uint32_t)np;
    return 0;
}
static long MemTell(void* ds) { return (long)((MemSource*)ds)->pos; }
static int  MemClose(void* ds) { (void)ds; return 0; }

static size_t DiscRead(void* ptr, size_t size, size_t nmemb, void* ds)
{
    DiscSource* d = (DiscSource*)ds;
    size_t want = size * nmemb;
    size_t avail = d->size - d->pos;
    if (want > avail) want = avail;

    size_t done = 0;
    while (done < want)
    {
        if (d->pos < d->windowAt || d->pos >= d->windowAt + d->windowSize)
        {
            if (d->aheadState == AHEAD_READY)
            {
                if (d->pos >= d->aheadAt && d->pos < d->aheadAt + d->aheadSize)
                {
                    // The read-ahead has it: swap the buffers, no disc access here.
                    uint8_t* w = d->window;
                    d->window = d->ahead;
                    d->ahead = w;
                    d->windowAt = d->aheadAt;
                    d->windowSize = d->aheadSize;
                }
                d->aheadState = AHEAD_EMPTY;    // used, or the stream seeked elsewhere
            }
        }

        if (d->pos < d->windowAt || d->pos >= d->windowAt + d->windowSize)
        {
            // Not read ahead (the stream's start, a seek, a loop): read it here. (Timed only on the main
            // thread: the profiler's stats aren't the audio threads' to touch, and they open and decode streams.)
            const lwp_t self = LWP_GetSelf();
            const bool onMain = (self != sAudioIoThread && self != sAudioDecodeThread && self != sAudioTrackThread);
            if (onMain) BEGIN_FRAME_STAT("AudRead");
            uint32_t fill = d->size - d->pos;
            if (fill > kDiscWindowBytes) fill = kDiscWindowBytes;
            const bool readOk = SYS_ReadFileRange(d->path.c_str(), true, d->base + d->pos, fill, (char*)d->window);
            if (onMain) END_FRAME_STAT("AudRead");
            if (!readOk)
            {
                LogError("Audio stream: disc read failed (%s, offset %u, %u bytes)",
                         d->path.c_str(), d->base + d->pos, fill);
                d->windowSize = 0;
                break;
            }
            d->windowAt = d->pos;
            d->windowSize = fill;
        }

        RequestReadAhead(d);

        uint32_t from = d->pos - d->windowAt;
        size_t take = d->windowSize - from;
        if (take > want - done) take = want - done;
        memcpy((uint8_t*)ptr + done, d->window + from, take);
        done += take;
        d->pos += (uint32_t)take;
    }
    return (size > 0) ? (done / size) : 0;
}
static int DiscSeek(void* ds, ogg_int64_t off, int whence)
{
    DiscSource* d = (DiscSource*)ds;
    int64_t np = (whence == SEEK_CUR) ? (int64_t)d->pos + off
               : (whence == SEEK_END) ? (int64_t)d->size + off
               : off;
    if (np < 0) np = 0;
    if (np > (int64_t)d->size) np = d->size;
    d->pos = (uint32_t)np;
    return 0;
}
static long DiscTell(void* ds) { return (long)((DiscSource*)ds)->pos; }

// Ask the I/O thread for the window after the current one, if there is one and nothing is pending.
static void RequestReadAhead(DiscSource* d)
{
    if (d->ahead == nullptr || sAudioIoSem == LWP_SEM_NULL || d->aheadState != AHEAD_EMPTY)
        return;

    const uint32_t next = d->windowAt + d->windowSize;
    if (d->windowSize == 0 || next >= d->size)
        return;

    d->aheadAt = next;
    d->aheadSize = glm::min(d->size - next, kDiscWindowBytes);
    d->aheadState = AHEAD_REQUESTED;
    LWP_SemPost(sAudioIoSem);
}

// Waits until the I/O thread is done with this source's read-ahead buffer (before freeing it).
static void WaitReadAhead(DiscSource* d)
{
    while (d->aheadState == AHEAD_REQUESTED)
        usleep(500);
}

// At a looping stream's end: on the main thread, a disc stream's way back to the start is the I/O thread's
// (LOOP_REQUESTED) -- true, and StreamFill stops there. Anywhere else (in RAM, the I/O thread itself, no I/O
// thread) it seeks there and then.
static bool StreamDeferLoop(StreamVoice* sv)
{
    if (sv->disc.window == nullptr || sAudioIoSem == LWP_SEM_NULL || LWP_GetSelf() == sAudioIoThread)
        return false;
    __asm__ volatile("" ::: "memory");
    sv->loopState = LOOP_REQUESTED;
    LWP_SemPost(sAudioIoSem);
    return true;
}

// Decode up to `want` bytes into dst. Returns bytes written (0 = ended, or a loop's start asked of the I/O thread).
static uint32_t StreamFill(StreamVoice* sv, uint8_t* dst, uint32_t want)
{
    uint32_t written = 0;
    int32_t errors = 0;

    if (sv->raw)
    {
        // PCM as it plays: copied from the disc window. At its end, a loop goes back to its first byte -- asked of the
        // I/O thread as a Vorbis loop's seek is (StreamDeferLoop), or done here.
        while (written < want)
        {
            if (errors > 32)
            {
                LogError("Audio stream: disc reads kept failing; stream ended.");
                sv->eof = true;
                break;
            }
            written += (uint32_t)DiscRead(dst + written, 1, want - written, &sv->disc);
            if (written == want)
                break;
            if (sv->disc.pos < sv->disc.size)
            {
                ++errors;                      // (a read failed: DiscRead logged it)
                continue;
            }
            if (!sv->loop)
            {
                sv->eof = true;
                break;
            }
            if (StreamDeferLoop(sv))
                break;
            sv->disc.pos = 0;
            ++errors;                          // (a loop with nothing in it counts as an error too)
        }
        return written;
    }

    while (written < want)
    {
        // ov_read can go on returning an error (a read that keeps failing, a broken stream).
        // This loop retried for ever on the MAIN thread, which is a frozen game. Give up on
        // the stream instead: silence is a far better failure than a hang.
        if (errors > 32)
        {
            LogError("Audio stream: decoder kept failing; stream ended.");
            sv->eof = true;
            break;
        }

        uint32_t remain = want - written;
        long ret;

        if (sv->bytesPerSample == 2)
        {
            // ASND wants little-endian 16-bit -> decode straight into dst.
            ret = ov_read(&sv->vf, (char*)(dst + written), (int)remain, 0, 2, 1, NULL);
            if (ret == 0) { if (sv->loop) { if (StreamDeferLoop(sv)) break; ov_pcm_seek(&sv->vf, 0); ++errors; continue; } sv->eof = true; break; }   // a loop that cannot restart counts as an error too
            if (ret < 0) { ++errors; continue; }
            written += (uint32_t)ret;
        }
        else
        {
            // 8-bit unsigned target: decode native-endian 16-bit, then convert.
            int16_t tmp[1024];
            long cap = (long)sizeof(tmp);
            if (cap > (long)(remain * 2)) cap = (long)(remain * 2);
            ret = ov_read(&sv->vf, (char*)tmp, (int)cap, 1, 2, 1, NULL);
            if (ret == 0) { if (sv->loop) { if (StreamDeferLoop(sv)) break; ov_pcm_seek(&sv->vf, 0); ++errors; continue; } sv->eof = true; break; }   // a loop that cannot restart counts as an error too
            if (ret < 0) { ++errors; continue; }
            int n = (int)(ret / 2);
            for (int i = 0; i < n; ++i)
                dst[written++] = (uint8_t)(((int)tmp[i] + 32768) >> 8);
        }
    }
    return written;
}

// Near the end of the window with the next still being read: no decoding now (the I/O thread reads it while the main
// thread waits on vsync), unless the voice is about to run dry -- a disc read here would only queue behind that one.
static bool StreamWaitsOnDisc(const StreamVoice* sv)
{
    const DiscSource& d = sv->disc;
    if (d.window == nullptr || d.aheadState != AHEAD_REQUESTED)
        return false;
    const uint32_t left = (d.pos >= d.windowAt && d.pos < d.windowAt + d.windowSize) ? d.windowAt + d.windowSize - d.pos : 0;
    return left < kDiscAheadMargin;
}

// The rest of the buffer being filled decoded, a unit at a time: till it's full, the stream ends, or a loop's start is
// asked of the I/O thread -- and, on the decode thread, till the disc's next window is still being read (unless the
// voice is nearly out), another voice nearly out wants the thread, or the main thread wants the stream back. The
// decode thread's, or the main thread's when it has taken the stream back (the whole rest at once, a disc read here if
// need be). Returns the bytes decoded.
static uint32_t StreamDecode(StreamVoice* sv, bool onMain)
{
    uint8_t* buf = sv->buf[sv->nextBuf];
    uint32_t decoded = 0;
    while (!sv->eof && sv->filled < sv->bufSize && sv->loopState == LOOP_NONE)
    {
        if (!onMain)
        {
            if (sv->decodeYield || sv->retiring)
                break;
            if (sv->decodeUrgent)
            {
                if (sv->decodeBudget == 0)
                    break;                       // (its budget done: the rest below the main thread)
                // Above the main thread till its budget's done. (Again each unit: the SD driver puts a noted thread
                // back to the priority it noted after each read -- below the main thread.)
                if (sDecodeBoosted)
                    LWP_SetThreadPriority(LWP_THREAD_NULL, kDecodeBoostPriority);
            }
            else if (sDecodeBoosted || StreamWaitsOnDisc(sv))
                break;                       // (another voice nearly out: that one first)
        }
        uint32_t want = sv->bufSize - sv->filled;
        if (want > kDecodeUnitBytes) want = kDecodeUnitBytes;   // (whole sample frames: both multiples of 4)
        const bool budgeted = !onMain && sv->decodeUrgent;
        if (budgeted && want > sv->decodeBudget) want = sv->decodeBudget;
        const uint32_t n = StreamFill(sv, buf + sv->filled, want);
        sv->filled += n;
        decoded += n;
        if (budgeted)
            sv->decodeBudget -= (n < sv->decodeBudget) ? n : sv->decodeBudget;
        if (!onMain)
        {
            const u32 level = IRQ_Disable();
            sDecodedBytes += n;
            IRQ_Restore(level);
        }
        if (n < want && sv->loopState == LOOP_NONE) sv->eof = true;   // (short of it at a loop: the start's asked for)
    }
    return decoded;
}

// One stream's buffer being filled decoded, and the stream handed back to the main thread.
static void StreamDecodeSlot(StreamVoice* sv)
{
    StreamDecode(sv, false);
    __asm__ volatile("" ::: "memory");   // (all of it written before the main thread is told)
    sv->decodeState = DECODE_NONE;
    if (sv->retiring && sAudioIoSem != LWP_SEM_NULL)
        LWP_SemPost(sAudioIoSem);         // (stopped while decoding: the I/O thread's next pass frees it)
}

// Decodes the streams the main thread asks it to (AUD_Update, DECODE_REQUESTED), each into its buffer being filled. Above
// the I/O thread and below the main thread (see AUD_Initialize): it runs while the main thread waits on vsync and the GPU,
// and the main thread takes the CPU back the moment it wakes -- except for a voice nearly out (kDecodeBoostLeadUs).
static void* AudioDecodeThread(void*)
{
#if PLATFORM_GAMECUBE
    OctSd_NoteThreadPriority(kDecodePriority);  // (as created in AUD_Initialize: a disc read here, should one be needed,
                                                //  as the I/O thread's)
#endif
    while (true)
    {
        LWP_SemWait(sAudioDecodeSem);

        while (true)
        {
            // The voices nearly out first, above the main thread (StreamBoostDecode); then back below it for the rest.
            for (int32_t s = 0; s < kStreamSlots; ++s)
            {
                StreamVoice* sv = &sStreamSlots[s];
                if (sv->decodeState == DECODE_REQUESTED && sv->decodeUrgent)
                {
                    StreamDecode(sv, false);        // (its budget's worth)
                    sv->decodeUrgent = false;       // (still asked for: the rest below, in the main thread's wait)
                }
            }
            if (sDecodeBoosted)
            {
                sDecodeBoosted = false;
                LWP_SetThreadPriority(LWP_THREAD_NULL, kDecodePriority);   // (the main thread goes on from here)
            }

            bool boosted = false;
            for (int32_t s = 0; s < kStreamSlots && !boosted; ++s)
            {
                StreamVoice* sv = &sStreamSlots[s];
                if (sv->decodeState == DECODE_REQUESTED)
                    StreamDecodeSlot(sv);
                boosted = sDecodeBoosted;            // (a voice nearly out meanwhile: round again, that one first)
            }
            if (!boosted)
                break;
        }
    }
    return nullptr;
}

// The stream back from the decode thread, the voice about to run dry with it still decoding (the thread starved, or
// behind the disc): asked to stop after its unit, which it does as soon as the main thread waits (it's the highest
// thread below it). False: not back yet (in a disc read) -- next frame.
static bool StreamTakeBack(StreamVoice* sv)
{
    sv->decodeYield = true;
    const uint64_t fromUs = SYS_GetTimeMicroseconds();
    while (sv->decodeState == DECODE_REQUESTED && SYS_GetTimeMicroseconds() - fromUs < kDecodeTakeBackUs)
        usleep(100);
    return sv->decodeState != DECODE_REQUESTED;
}

// The buffer being filled asked of the decode thread, to be decoded while the main thread waits on vsync. (The main
// thread's, with the stream its own and more of it to decode.)
static void StreamRequestDecode(StreamVoice* sv, bool urgent)
{
    sv->decodeYield = false;
    sv->decodeUrgent = urgent;
    __asm__ volatile("" ::: "memory");   // (all of it written before the thread is told)
    sv->decodeState = DECODE_REQUESTED;
    LWP_SemPost(sAudioDecodeSem);
}

// An urgent voice's budget above the main thread: kBoostBudgetUs of its audio at its pitch, whole sample frames -- a
// few frames' worth, so the lead grows each frame it's boosted and the work's spread over them, as a slice a frame on
// the main thread was.
static const uint64_t kBoostBudgetUs = 42 * 1000;
static void StreamBoostBudget(StreamVoice* sv)
{
    const uint32_t frame = uint32_t(glm::max<int32_t>(sv->channels * sv->bytesPerSample, 1));
    const uint64_t perSec = uint64_t(glm::max<int32_t>(sv->pitchHz, 1)) * frame;
    uint32_t bytes = uint32_t(perSec * kBoostBudgetUs / 1000000u);
    bytes -= bytes % (frame * 4);
    sv->decodeBudget = glm::max<uint32_t>(bytes, frame * 256);
}

// The decode thread raised above the main thread, for the voices nearly out (decodeUrgent): it runs at once -- here, as
// the main thread asks -- decodes them, and goes back below the main thread, which goes on from here with them decoded
// (unless the thread waits on the disc: then the main thread goes on at once, and its last resort is StreamTakeBack).
static void StreamBoostDecode()
{
    sDecodeBoosted = true;
    __asm__ volatile("" ::: "memory");
    LWP_SetThreadPriority(sAudioDecodeThread, kDecodeBoostPriority);
    LWP_SemPost(sAudioDecodeSem);        // (should it be waiting for work)
}

// How long `bytes` of the stream's PCM plays for at its pitch, and the audio handed to ASND reckoned on by that (from
// now, or from when what it has already runs out).
static uint64_t StreamPlayUs(const StreamVoice* sv, uint32_t bytes)
{
    const uint64_t perSec = uint64_t(glm::max<int32_t>(sv->pitchHz, 1)) * uint32_t(sv->channels * sv->bytesPerSample);
    return uint64_t(bytes) * 1000000u / perSec;
}
static void StreamQueued(StreamVoice* sv, uint32_t bytes, uint64_t nowUs)
{
    sv->playEndUs = glm::max(sv->playEndUs, nowUs) + StreamPlayUs(sv, bytes);
}

// The main thread's old way, without a decode thread (it couldn't be made): a slice a frame, keeping pace with time.
static void StreamDecodeSlice(StreamVoice* sv, bool aboutDry, float elapsed)
{
    if (!aboutDry && StreamWaitsOnDisc(sv))
        return;
    // A quarter more than the time that passed, so the decoder stays ahead of the playback.
    const uint32_t frameBytes = uint32_t(sv->channels * sv->bytesPerSample);
    uint32_t slice = uint32_t(sv->bytesPerSec * elapsed * 1.25f);
    slice -= slice % frameBytes;
    slice = glm::max(slice, sv->sliceBytes);
    uint32_t want = sv->bufSize - sv->filled;
    if (want > slice) want = slice;
    // If the voice is about to run dry (a long frame, a slow read), finish the buffer now:
    // one long frame is better than a gap in the music.
    if (aboutDry) want = sv->bufSize - sv->filled;
    uint32_t n = StreamFill(sv, sv->buf[sv->nextBuf] + sv->filled, want);
    sv->filled += n;
    if (n < want && sv->loopState == LOOP_NONE) sv->eof = true;   // (short of it at a loop: the start's asked for)
}

// A slot nothing is in hand on, left to the I/O thread to free (StreamTryRetire): the decoder's frees off the main thread.
static void StreamRetire(StreamVoice* sv)
{
    sv->cached = false;
    __asm__ volatile("" ::: "memory");
    sv->retiring = true;
    LWP_SemPost(sAudioIoSem);
}

// A disc stream stopped with nothing in hand on the I/O thread: its decoder kept open in the cache (StreamTakeCached
// starts it again), its PCM buffers and disc windows freed as before (made again at its start, so all it keeps is the
// decoder). The voice gets the cache's slot in its place -- the one kept before that, left to the I/O thread to free.
// False: not kept (freed as ever). Only with Config.ini's AudioDecodeThread=1, as the decode thread.
static bool StreamPark(StreamVoice* sv)
{
    if (!GetEngineConfig()->mAudioDecodeThread)
        return false;
    if (!sv->vfOpen || sv->memOwned != nullptr || sv->openState == OPEN_FAILED)
        return false;                       // (not open, in RAM, or a restart's seek failed: its decoder's state unsure)
    int32_t v = 0;
    while (v < AUDIO_MAX_VOICES && sStreams[v] != sv)
        ++v;
    if (v == AUDIO_MAX_VOICES)
        return false;

    int32_t c = 0;                          // the cache's slot: a free one, or else the one kept longest
    for (int32_t i = 0; i < kStreamCacheSlots; ++i)
    {
        if (!CacheSlot(i)->cached && !CacheSlot(i)->retiring) { c = i; break; }
        if (CacheSlot(i)->cached && CacheSlot(c)->cached && CacheSlot(i)->cachedAtUs < CacheSlot(c)->cachedAtUs) c = i;
    }
    StreamVoice* out = CacheSlot(c);
    if (out->cached)
        StreamRetire(out);                  // (the voice's now, retiring: StreamFor goes on to its spare meanwhile)

    for (int b = 0; b < 3; ++b)
    {
        if (sv->buf[b]) { free(sv->buf[b]); sv->buf[b] = nullptr; }
    }
    sv->filled = 0;
    if (sv->disc.window) { free(sv->disc.window); sv->disc.window = nullptr; }
    if (sv->disc.ahead) { free(sv->disc.ahead); sv->disc.ahead = nullptr; }
    sv->disc.windowSize = 0;
    sv->disc.aheadState = AHEAD_EMPTY;      // (EMPTY or READY here: not busy)
    sv->openState = OPEN_NONE;              // (NONE, or DONE if stopped before its voice was set)
    sv->loopState = LOOP_NONE;              // (NONE or DONE: it's seeked to its start anyway)
    sv->decodeYield = false;                // (decodeState NONE here: not busy)
    sv->decodeUrgent = false;
    sv->eof = false;
    sv->active = false;
    sv->cached = true;
    sv->cachedAtUs = SYS_GetTimeMicroseconds();

    sCache[c] = sv;
    sStreams[v] = out;
    return true;
}

// The same sound kept open (StreamPark): it becomes the voice's slot, and the voice's (stopped: free, or the I/O
// thread's to free) goes to the cache. nullptr: none kept.
static StreamVoice* StreamTakeCached(int32_t v, SoundWave* soundWave)
{
    for (int32_t c = 0; c < kStreamCacheSlots; ++c)
    {
        StreamVoice* sv = CacheSlot(c);
        if (!sv->cached || sv->disc.base != soundWave->GetDiscOffset() || sv->disc.size != soundWave->GetDiscSize() ||
            sv->disc.path != soundWave->GetDiscPath())
            continue;
        sv->cached = false;
        sCache[c] = VoiceStream(v);
        sStreams[v] = sv;
        return sv;
    }
    return nullptr;
}

// Stops a stream: freed here, or -- the I/O thread still working on it -- left to that thread to free when it's
// done (retiring), and the voice goes on with its other slot (StreamFor). Never waits on the disc. A disc stream
// otherwise keeps its decoder a while (StreamPark).
static void StreamStop(StreamVoice* sv)
{
    if (!sv->active) return;
    if (sAudioIoSem != LWP_SEM_NULL)
    {
        const u32 level = IRQ_Disable();    // (the I/O thread's states can't change in between: one CPU)
        const bool busy = StreamIoBusy(sv);
        if (busy)
        {
            sv->retiring = true;
            sv->active = false;
        }
        IRQ_Restore(level);
        if (busy)
        {
            LWP_SemPost(sAudioIoSem);       // (its next pass frees it, once its work on it is done)
            return;
        }
        if (StreamPark(sv))                 // (nothing in hand on it, and nothing new till the main thread asks)
            return;
    }
    StreamFree(sv);
}

// Frees a stream's decoder, buffers and disc windows. Nothing of it is the I/O thread's by now (StreamStop, or
// that thread itself retiring it); the waits below are for safety only and come to nothing.
static void StreamFree(StreamVoice* sv)
{
    const uint64_t waitFromUs = SYS_GetTimeMicroseconds();
    const bool wasOpening = (sv->openState == OPEN_REQUESTED), wasReading = (sv->disc.aheadState == AHEAD_REQUESTED);
    while (sv->openState == OPEN_REQUESTED)   // (stopped while the I/O thread opens it: let it finish)
        usleep(500);
    sv->openState = OPEN_NONE;
    while (sv->loopState == LOOP_REQUESTED)
        usleep(500);
    sv->loopState = LOOP_NONE;
    sv->decodeYield = true;                   // (decoding: done after its unit)
    while (sv->decodeState == DECODE_REQUESTED)
        usleep(500);
    sv->decodeYield = false;
    sv->decodeUrgent = false;
    WaitReadAhead(&sv->disc);
    const uint64_t waitedUs = SYS_GetTimeMicroseconds() - waitFromUs;
    if (waitedUs > 1000)    // (the main thread held up by the I/O thread's disc work: a hitch, logged)
    {
        LogWarning("AUDIO stop waited %.1f ms (%s)", waitedUs / 1000.0f,
                   wasOpening ? "the stream still opening" : (wasReading ? "a read-ahead on the disc" : "?"));
    }
    sv->disc.aheadState = AHEAD_EMPTY;
    if (sv->vfOpen) ov_clear(&sv->vf);
    sv->vfOpen = false;
    for (int b = 0; b < 3; ++b)
    {
        if (sv->buf[b]) { free(sv->buf[b]); sv->buf[b] = nullptr; }
    }
    sv->filled = 0;
    if (sv->memOwned) { free(sv->memOwned); sv->memOwned = nullptr; }
    if (sv->disc.window) { free(sv->disc.window); sv->disc.window = nullptr; }
    if (sv->disc.ahead) { free(sv->disc.ahead); sv->disc.ahead = nullptr; }
    sv->disc.windowSize = 0;
    sv->active = false;
    sv->eof = false;
    sv->cached = false;
    sv->restart = false;
    __asm__ volatile("" ::: "memory");   // (all of it freed before the slot is anyone's again)
    sv->retiring = false;
}

// The voice's slot to start a stream in: its own, or -- that one still the I/O thread's to free -- its spare, the
// two swapped. Both retiring (restarted twice within the I/O thread's work): waits for the other, logged as before.
static StreamVoice* StreamFor(int32_t v)
{
    StreamVoice* cur = VoiceStream(v);
    if (!cur->retiring)
        return cur;
    StreamVoice* other = sSpares[v];
    if (other->retiring)
    {
        const uint64_t waitFromUs = SYS_GetTimeMicroseconds();
        while (other->retiring)
        {
            LWP_SemPost(sAudioIoSem);
            usleep(500);
        }
        LogWarning("AUDIO stop waited %.1f ms (a voice restarted twice while its streams were still on the disc)",
                   (SYS_GetTimeMicroseconds() - waitFromUs) / 1000.0f);
    }
    sSpares[v] = cur;
    sStreams[v] = other;
    return other;
}

// Keep-alive callback: non-NULL so an underrun parks the voice in SND_WAITING
// instead of ending it. Feeding happens on the main thread in AUD_Update (the
// decoding on the decode thread).
static void StreamCallback(s32 voice) { (void)voice; }

// ---------------------------------------------------------------------------
// Push-PCM streams (AUD_OpenStream)
//
// Audio produced at runtime (e.g. a video soundtrack) is queued into a FIFO and fed
// to ASND in ~100 ms buffers: one playing, one queued with ASND_AddVoice. These use
// ASND voices above AUD_Play's range.
//
// libasnd details this relies on (see libasnd/asndlib.c):
// - ASND_StatusVoice only reports paused/unused, never "drained", and
//   ASND_TestVoiceBufferReady is also true when the voice has nothing at all.
// - ASND_AddVoice fails while a previous add hasn't been picked up by the mixer.
// So data only leaves the FIFO once ASND accepts it, and a free buffer is one
// ASND_TestPointer says ASND isn't using. The played position comes from ASND's
// per-voice tick counter, which only advances while the voice has data.
// ---------------------------------------------------------------------------

// SD diagnostic log (System_Dolphin.cpp; a no-op unless the local logger is enabled). Stream memory
// problems also go there, since LogError only reaches the screen.
void OctLog(const char* format, ...);

#define AUD_MAX_PCM_STREAMS 8
#define AUD_PCM_STREAM_BUFFERS 3

struct PcmStream
{
    bool     active = false;
    bool     started = false;           // ASND_SetVoice issued
    bool     paused = true;
    int32_t  voice = 0;
    int32_t  format = 0;
    uint32_t sampleRate = 0;
    uint32_t frameBytes = 4;
    int32_t  volume = MID_VOLUME;
    uint8_t* buf[AUD_PCM_STREAM_BUFFERS] = { nullptr, nullptr, nullptr };
    uint32_t bufCapacity = 0;
    // FIFO of queued PCM not yet handed to ASND, in the slot's kept buffer (see PcmSlotMemory), not
    // a growing std::vector: on the GameCube a vector reallocation that couldn't find a big enough
    // block threw bad_alloc, which without exceptions aborts, and libogc's abort loops forever.
    uint8_t* pending = nullptr;
    uint32_t pendingCapacity = 0;
    uint32_t pendingSize = 0;           // bytes in use, from the start of the buffer
    uint32_t pendingHead = 0;           // bytes already consumed
    uint32_t tickBase = 0;              // ASND tick counter when the voice (re)started
    uint64_t fedFrames = 0;             // frames handed to ASND since the voice (re)started
};

static PcmStream sPcmStreams[AUD_MAX_PCM_STREAMS];

// Memory each stream slot keeps across close and open. Games open and close streams often (a sound
// each time the player does something); freeing and re-allocating these blocks every time fragmented
// the GameCube's small heap until a stream couldn't open at all, with plenty of memory free in total.
struct PcmSlotMemory
{
    uint8_t* buf[AUD_PCM_STREAM_BUFFERS] = { nullptr, nullptr, nullptr };
    uint32_t bufAlloc = 0;
    uint8_t* pending = nullptr;
    uint32_t pendingAlloc = 0;
};
static PcmSlotMemory sPcmSlotMemory[AUD_MAX_PCM_STREAMS];

static PcmStream* GetPcmStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > AUD_MAX_PCM_STREAMS)
        return nullptr;

    PcmStream* stream = &sPcmStreams[streamId - 1];
    return stream->active ? stream : nullptr;
}

static uint32_t PcmPendingBytes(const PcmStream& s)
{
    return s.pendingSize - s.pendingHead;
}

// Removes n bytes from the front of the FIFO. (The rest moves to the front on the next queue.)
static void PcmConsume(PcmStream& s, uint32_t n)
{
    s.fedFrames += n / s.frameBytes;    // PcmConsume runs as data goes to ASND
    s.pendingHead += n;

    if (s.pendingHead >= s.pendingSize)
    {
        s.pendingSize = 0;
        s.pendingHead = 0;
    }
}

// Hands the next full buffer of PCM to ASND if it has room. One buffer per call is
// enough (each holds ~100 ms and this runs every frame), and a second AddVoice would
// fail anyway until ASND's mixer picks up the first.
static void PcmFeed(PcmStream& s)
{
    if (s.paused || PcmPendingBytes(s) < s.bufCapacity)
        return;

    const uint8_t* src = s.pending + s.pendingHead;

    if (!s.started)
    {
        memcpy(s.buf[0], src, s.bufCapacity);
        s.tickBase = ASND_GetTickCounterVoice(s.voice);

        if (ASND_SetVoice(s.voice, s.format, s.sampleRate, 0, s.buf[0], s.bufCapacity, s.volume, s.volume, StreamCallback) == SND_OK)
        {
            PcmConsume(s, s.bufCapacity);
            s.started = true;
        }
        return;
    }

    // Room for another buffer (one is playing with nothing queued, or it ran dry).
    if (ASND_TestVoiceBufferReady(s.voice) != 1)
        return;

    for (uint32_t i = 0; i < AUD_PCM_STREAM_BUFFERS; ++i)
    {
        if (ASND_TestPointer(s.voice, s.buf[i]) == SND_BUSY)
            continue;   // ASND is still reading from it

        memcpy(s.buf[i], src, s.bufCapacity);

        if (ASND_AddVoice(s.voice, s.buf[i], s.bufCapacity) == SND_OK)
        {
            PcmConsume(s, s.bufCapacity);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// ARAM sound effects (GameCube)
//
// The GameCube has 16 MB of ARAM beside its 24 MB of main memory, reachable only by DMA. A sound
// effect's PCM moves there when it loads (AUD_ProcessWaveBuffer) and its main-memory copy is
// freed: in one game that was ~780 KB of main memory back, for 128 KB of voice buffers.
//
// libasnd plays from main memory, so a voice playing an ARAM sound has two 8 KB buffers: the
// first is read from ARAM when it starts, and each time libasnd takes the queued buffer it calls
// AramVoiceCallback (from the audio interrupt), which reads the next 8 KB into the free one and
// queues it. Nothing waits on the main thread, so a long frame cannot starve a sound.
//
// Every ARAM transfer runs with interrupts off, in pieces of at most 8 KB (tens of
// microseconds): a loader thread's upload and the interrupt's reads never meet, and libogc's
// AR_StartDMA is never called while a transfer is running.
// ---------------------------------------------------------------------------
#if PLATFORM_GAMECUBE

#include <ogc/aram.h>
#include <ogc/mutex.h>
#include <ogc/machine/processor.h>

static const uint32_t kAramVoiceBuf = 8 * 1024;    // over one ASND mix block (4 KB) at any format
static const uint32_t kAramDmaPiece = 8 * 1024;
static const uint32_t kAramMaxBlocks = 256;

struct AramBlock
{
    uint32_t at;
    uint32_t size;
    bool     used;
};

static bool      sAramReady = false;
static uint32_t  sAramTotal = 0;
static AramBlock sAramBlocks[kAramMaxBlocks];     // address order, covering the user area
static uint32_t  sAramNumBlocks = 0;
static uint32_t  sAramSoundBytes = 0;
static mutex_t   sAramMutex = LWP_MUTEX_NULL;

struct AramVoice
{
    volatile bool active = false;
    uint32_t aram = 0;
    uint32_t size = 0;
    uint32_t pos = 0;           // bytes handed to ASND so far
    bool     loop = false;
    uint8_t* buf[2] = { nullptr, nullptr };
    int      next = 0;          // the buffer to fill next
};

static AramVoice sAramVoices[AUDIO_MAX_VOICES];

// A transfer between main memory and ARAM, a piece at a time with interrupts off. Main memory
// and ARAM addresses 32-byte aligned; the length is rounded up to 32.
static void AramDma(uint32_t dir, void* mem, uint32_t aram, uint32_t len)
{
    len = (len + 31) & ~31u;
    uint8_t* m = (uint8_t*)mem;
    if (dir == AR_MRAMTOARAM) DCFlushRange(m, len);
    else DCInvalidateRange(m, len);

    for (uint32_t done = 0; done < len; done += kAramDmaPiece)
    {
        const uint32_t piece = glm::min(kAramDmaPiece, len - done);
        uint32_t level;
        _CPU_ISR_Disable(level);
        AR_StartDMA(dir, (uint32_t)MEM_VIRTUAL_TO_PHYSICAL(m + done), aram + done, piece);
        while (AR_GetDMAStatus()) {}
        _CPU_ISR_Restore(level);
    }
}

static void AramInit()
{
    const uint32_t base = AR_Init(nullptr, 0);   // the OS keeps the first 16 KB
    sAramTotal = AR_GetSize();
    if (sAramTotal <= base || LWP_MutexInit(&sAramMutex, false) != 0)
    {
        return;
    }

    for (AramVoice& v : sAramVoices)
    {
        v.buf[0] = (uint8_t*)memalign(32, kAramVoiceBuf);
        v.buf[1] = (uint8_t*)memalign(32, kAramVoiceBuf);
        if (v.buf[0] == nullptr || v.buf[1] == nullptr)
        {
            return;     // no ARAM sounds: they stay in main memory, as before
        }
    }

    sAramBlocks[0] = { base, sAramTotal - base, false };
    sAramNumBlocks = 1;
    sAramReady = true;
}

// First fit, in 32-byte units. 0 if there is no room (the sound then stays in main memory).
static uint32_t AramAlloc(uint32_t len)
{
    len = (len + 31) & ~31u;
    uint32_t at = 0;
    LWP_MutexLock(sAramMutex);
    for (uint32_t i = 0; i < sAramNumBlocks; ++i)
    {
        AramBlock& b = sAramBlocks[i];
        if (b.used || b.size < len)
            continue;
        if (b.size > len)
        {
            if (sAramNumBlocks == kAramMaxBlocks)
                continue;   // no entry for the rest: take a block that fits exactly, or nothing
            memmove(&sAramBlocks[i + 2], &sAramBlocks[i + 1], (sAramNumBlocks - i - 1) * sizeof(AramBlock));
            sAramBlocks[i + 1] = { b.at + len, b.size - len, false };
            ++sAramNumBlocks;
            b.size = len;
        }
        b.used = true;
        at = b.at;
        sAramSoundBytes += len;
        break;
    }
    LWP_MutexUnlock(sAramMutex);
    return at;
}

// ARAM for anything, not only sounds (a texture's stash: Texture::StashFrom). 0 if there is none.
uint32_t AUD_AllocAram(uint32_t len)
{
    return sAramReady ? AramAlloc(len) : 0;
}

// A copy between main memory and ARAM (32-byte aligned; the length rounds up to 32), from any
// thread: each 8 KB piece runs with interrupts off, so it never meets the sound voices' own.
void AUD_AramDma(bool toAram, void* mem, uint32_t aram, uint32_t len)
{
    AramDma(toAram ? AR_MRAMTOARAM : AR_ARAMTOMRAM, mem, aram, len);
}

void AUD_FreeAram(uint32_t aramAddress)
{
    if (!sAramReady || aramAddress == 0)
        return;

    LWP_MutexLock(sAramMutex);
    for (uint32_t i = 0; i < sAramNumBlocks; ++i)
    {
        if (sAramBlocks[i].at != aramAddress || !sAramBlocks[i].used)
            continue;
        sAramBlocks[i].used = false;
        sAramSoundBytes -= sAramBlocks[i].size;
        // Join free neighbours, so the space comes back whole.
        if (i + 1 < sAramNumBlocks && !sAramBlocks[i + 1].used)
        {
            sAramBlocks[i].size += sAramBlocks[i + 1].size;
            memmove(&sAramBlocks[i + 1], &sAramBlocks[i + 2], (sAramNumBlocks - i - 2) * sizeof(AramBlock));
            --sAramNumBlocks;
        }
        if (i > 0 && !sAramBlocks[i - 1].used)
        {
            sAramBlocks[i - 1].size += sAramBlocks[i].size;
            memmove(&sAramBlocks[i], &sAramBlocks[i + 1], (sAramNumBlocks - i - 1) * sizeof(AramBlock));
            --sAramNumBlocks;
        }
        break;
    }
    LWP_MutexUnlock(sAramMutex);
}

void AUD_GetAramStats(uint32_t& total, uint32_t& free, uint32_t& sounds)
{
    total = sAramReady ? sAramTotal : 0;
    free = 0;
    sounds = 0;
    if (!sAramReady)
        return;
    LWP_MutexLock(sAramMutex);
    for (uint32_t i = 0; i < sAramNumBlocks; ++i)
    {
        if (!sAramBlocks[i].used) free += sAramBlocks[i].size;
    }
    sounds = sAramSoundBytes;
    LWP_MutexUnlock(sAramMutex);
}

// Reads the voice's next piece into its free buffer and queues it. Runs in the audio interrupt
// (libasnd calls it while the voice has no buffer queued) and once from AramVoiceStart.
static void AramVoiceCallback(s32 voice)
{
    if (voice < 0 || voice >= AUDIO_MAX_VOICES)
        return;
    AramVoice& v = sAramVoices[voice];
    if (!v.active)
        return;

    if (v.pos >= v.size)
    {
        if (!v.loop)
            return;     // played out: the voice runs dry and waits (AUD_IsPlaying is then false)
        v.pos = 0;
    }

    if (ASND_TestVoiceBufferReady(voice) != 1 || ASND_TestPointer(voice, v.buf[v.next]) == SND_BUSY)
        return;         // no room yet; libasnd calls again

    const uint32_t n = glm::min(kAramVoiceBuf, v.size - v.pos);
    AramDma(AR_ARAMTOMRAM, v.buf[v.next], v.aram + v.pos, n);
    if (ASND_AddVoice(voice, v.buf[v.next], n) == SND_OK)
    {
        v.pos += n;
        v.next ^= 1;
    }
}

static void AramVoiceStop(uint32_t voice)
{
    if (voice < AUDIO_MAX_VOICES)
        sAramVoices[voice].active = false;
}

// A voice playing `size` bytes of PCM from ARAM at `aram`: a sound effect's (AramVoiceStart), or a streamed track's
// copy (AramTrack).
static void AramVoiceStartAt(uint32_t voice, uint32_t aram, uint32_t size, int32_t format, int32_t pitchHz, int32_t volL,
                             int32_t volR, bool loop)
{
    ASND_StopVoice(voice);

    AramVoice& v = sAramVoices[voice];
    v.active = false;
    v.aram = aram;
    v.size = size;
    v.loop = loop;

    const uint32_t n = glm::min(kAramVoiceBuf, v.size);
    AramDma(AR_ARAMTOMRAM, v.buf[0], v.aram, n);
    v.pos = n;
    v.next = 1;
    v.active = true;

    ASND_SetVoice(voice, format, pitchHz, 0, v.buf[0], n, volL, volR, AramVoiceCallback);

    // Queue the second buffer now, not when the first runs out. Interrupts off: the callback
    // may also run from the audio interrupt.
    uint32_t level;
    _CPU_ISR_Disable(level);
    AramVoiceCallback(voice);
    _CPU_ISR_Restore(level);
}

static void AramVoiceStart(uint32_t voice, SoundWave* soundWave, int32_t format, int32_t pitchHz, int32_t volL, int32_t volR, bool loop)
{
    AramVoiceStartAt(voice, soundWave->GetAramAddress(), soundWave->GetWaveDataSize(), format, pitchHz, volL, volR, loop);
}

// A loaded sound effect's PCM moves to ARAM. Called on whichever thread loaded the sound.
static void AramStoreWave(SoundWave* soundWave)
{
    if (!sAramReady || soundWave->GetStream() || soundWave->GetWaveData() == nullptr || soundWave->GetWaveDataSize() == 0)
        return;

    const uint32_t size = soundWave->GetWaveDataSize();
    const uint32_t at = AramAlloc(size);
    if (at == 0)
    {
        OctLog("ARAM full: '%s' (%u bytes) stays in main memory", soundWave->GetName().c_str(), size);
        return;
    }

    AramDma(AR_MRAMTOARAM, soundWave->GetWaveData(), at, size);
    soundWave->MoveWaveDataToAram(at);
}

// ---------------------------------------------------------------------------
// Streamed tracks copied to ARAM (GameCube; Config.ini AudioDecodeThread=1)
//
// (A track cooked uncompressed -- Stream on, Compress off -- is PCM on the disc already: copied to ARAM as it is, with
// no Vorbis on the console at all. What follows is for one cooked as Vorbis.)
//
// A long track streams from the disc because its PCM doesn't fit main memory: a 3.4 MB track, with about 4.5 MB free,
// and a sound that isn't streamed is read into main memory whole before it moves to ARAM. But a stream decodes as it
// plays, every frame: three 44.1 kHz stereo streams at once were 4-5 ms of Vorbis a frame on hardware, with 2 ms of
// the frame to spare, and the decode thread raised above the main thread to keep them going made 33-50 ms frames.
//
// So each disc-streamed sound, as it loads (AUD_ProcessWaveBuffer), is decoded once, in the background, straight into
// a block of ARAM a piece at a time (kTrackChunkBytes: no copy of the whole track in main memory), by the track thread
// (AudioTrackThread): far below the main thread and the other audio threads, in what time is left once they've had
// theirs. It never goes above the main thread. Once a copy is whole (TRACK_READY, and only then), AUD_Play plays the
// sound from it as an ARAM sound effect plays (AramVoiceStartAt, looped as asked): nothing decoded as it plays. Till
// then, or when ARAM hasn't room for it, it streams as before.
// ---------------------------------------------------------------------------
enum { TRACK_FREE = 0, TRACK_PENDING, TRACK_DECODING, TRACK_READY, TRACK_SKIPPED };

struct AramTrack
{
    SoundWave*       wave = nullptr;        // whose copy (only compared: nullptr once it has gone)
    std::string      path;                  // its Vorbis: this range of this file (the SoundWave's)
    uint32_t         base = 0;
    uint32_t         size = 0;
    char             name[32] = {};
    int32_t          channels = 2;
    int32_t          bytesPerSample = 2;
    bool             raw = false;           // PCM on the disc (SoundWave::GetDiscPcm): copied as it is, not decoded
    uint32_t         aram = 0;              // its ARAM block (0: none)
    uint32_t         pcmBytes = 0;          // the PCM in it, once READY
    // FREE -> PENDING (TrackRegister) -> DECODING (the track thread's) -> READY or SKIPPED (still streamed); back to
    // FREE when its SoundWave goes (AUD_ReleaseTrack). Changed under sTrackMutex; READY is read without it (AUD_Play).
    volatile int32_t state = TRACK_FREE;
    volatile bool    released = false;      // its SoundWave gone while it decodes: the track thread frees it
};

static const int32_t  kMaxAramTracks = 8;
static const uint32_t kTrackChunkBytes = 16 * 1024;           // PCM decoded, then copied to ARAM, at a time
static const uint32_t kTrackAramReserve = 1024 * 1024;        // ARAM left free for what loads later (sound effects,
                                                              //  texture stashes)
// Priority 30: below everything else that works -- the main thread (64), the decode thread (56), the I/O thread (50),
// the background reader (45), the asset loader (40) -- so it only gets what they leave; above the SD log's thread (10),
// which a rest after each chunk lets in.
static const u32      kTrackPriority = 30;
// AUD_WaitForTracks: the main thread asleep till they're made (a loading screen's), the rests left out
static volatile bool  sTrackRush = false;
static AramTrack sAramTracks[kMaxAramTracks];
static mutex_t   sTrackMutex = LWP_MUTEX_NULL;
static sem_t     sTrackSem = LWP_SEM_NULL;

// The decoder's disc source and state, the track thread's alone (one track at a time).
static DiscSource     sTrackDisc;
static OggVorbis_File sTrackVf;

// A disc-streamed sound to copy to ARAM (as it loads, on whichever thread loads it). Vorbis: 16-bit only, the stream's
// format; PCM: 8 or 16-bit, as it is.
static void TrackRegister(SoundWave* wave)
{
    if (sTrackSem == LWP_SEM_NULL || !wave->GetStream() || wave->GetDiscSize() == 0)
        return;
    const uint32_t bits = wave->GetBitsPerSample();
    if (wave->GetDiscPcm() ? (bits != 8 && bits != 16) : (bits != 16))
        return;

    AramTrack* slot = nullptr;
    bool known = false;
    LWP_MutexLock(sTrackMutex);
    for (AramTrack& t : sAramTracks)
    {
        if (t.state != TRACK_FREE && t.wave == wave)
            known = true;
        else if (t.state == TRACK_FREE && slot == nullptr)
            slot = &t;
    }
    if (!known && slot != nullptr)
    {
        slot->wave = wave;
        slot->path = wave->GetDiscPath();
        slot->base = wave->GetDiscOffset();
        slot->size = wave->GetDiscSize();
        strncpy(slot->name, wave->GetName().c_str(), sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = 0;
        slot->channels = (int32_t)wave->GetNumChannels();
        slot->bytesPerSample = int32_t(bits / 8);
        slot->raw = wave->GetDiscPcm();
        slot->aram = 0;
        slot->pcmBytes = 0;
        slot->released = false;
        __asm__ volatile("" ::: "memory");
        slot->state = TRACK_PENDING;
    }
    LWP_MutexUnlock(sTrackMutex);

    if (known)
        return;
    if (slot == nullptr)
    {
        LogWarning("AUDIO %s not in ARAM: no room in the track list (it streams)", wave->GetName().c_str());
        return;
    }
    LWP_SemPost(sTrackSem);
}

// One track decoded into its ARAM block (the track thread's). Marked READY only once all of it is there; failed,
// stopped or without room, its block is freed and it goes on streaming.
static void TrackDecode(AramTrack* t)
{
    const uint64_t fromUs = SYS_GetTimeMicroseconds();
    const uint32_t frameBytes = uint32_t(glm::max<int32_t>(t->channels, 1)) * uint32_t(t->bytesPerSample);

    sTrackDisc.path = t->path;
    sTrackDisc.base = t->base;
    sTrackDisc.size = t->size;
    sTrackDisc.pos = 0;
    sTrackDisc.windowAt = 0;
    sTrackDisc.windowSize = 0;
    sTrackDisc.window = (uint8_t*)malloc(kDiscWindowBytes);
    sTrackDisc.ahead = nullptr;               // (no read-ahead: it reads here, in its own time)
    sTrackDisc.aheadState = AHEAD_EMPTY;
    uint8_t* stage = (uint8_t*)memalign(32, kTrackChunkBytes);

    const char* why = nullptr;
    bool opened = false;
    uint32_t pcm = 0;
    uint32_t freeBytes = 0;
    if (sTrackDisc.window == nullptr || stage == nullptr)
        why = "no memory to decode it";
    else if (t->raw)
    {
        pcm = t->size - t->size % frameBytes;   // (PCM already: its size is all there is to know)
        uint32_t total = 0, sounds = 0;
        AUD_GetAramStats(total, freeBytes, sounds);
        if (pcm == 0)
            why = "no audio in it";
        else if (freeBytes < pcm + kTrackAramReserve || (t->aram = AramAlloc(pcm)) == 0)
            why = "no room";
    }
    else
    {
        ov_callbacks dcb = { DiscRead, DiscSeek, MemClose, DiscTell };
        opened = (ov_open_callbacks(&sTrackDisc, &sTrackVf, NULL, 0, dcb) >= 0);
        const ogg_int64_t frames = opened ? ov_pcm_total(&sTrackVf, -1) : -1;
        if (!opened)
            why = "it would not open";
        else if (frames <= 0 || frames > ogg_int64_t((16u << 20) / frameBytes))
            why = "its length is unknown";
        else
        {
            pcm = uint32_t(frames) * frameBytes;
            uint32_t total = 0, sounds = 0;
            AUD_GetAramStats(total, freeBytes, sounds);
            if (freeBytes < pcm + kTrackAramReserve || (t->aram = AramAlloc(pcm)) == 0)   // (rounded up to 32 there)
                why = "no room";
        }
    }

    // A chunk at a time: decoded into `stage`, copied to the block. Every chunk but the last is whole, so each copy
    // starts 32-byte aligned. Errors skipped as a stream skips them (StreamFill): the same samples it would play.
    uint32_t written = 0;
    int32_t errors = 0;
    bool eof = false;

    // PCM: read into `stage` a chunk at a time and copied to the block as it is -- nothing decoded.
    while (t->raw && why == nullptr && written < pcm)
    {
        if (t->released)
        {
            why = "stopped";
            break;
        }
        const uint32_t want = glm::min(kTrackChunkBytes, pcm - written);
        if (DiscRead(stage, 1, want, &sTrackDisc) != want)
        {
            why = "a disc read failed";
            break;
        }
        AramDma(AR_MRAMTOARAM, stage, t->aram + written, want);   // (rounded up to 32: the block has room for it)
        written += want;
        if (!sTrackRush)
            usleep(1000);                    // (a rest each chunk, as below)
    }
    if (t->raw)
        eof = true;

    while (why == nullptr && !eof)
    {
        if (t->released)
        {
            why = "stopped";
            break;
        }
        uint32_t n = 0;
        while (n < kTrackChunkBytes && !eof && why == nullptr)
        {
            long ret;
            if (written + n >= pcm)
            {
                char probe[64];             // (all it said it has: nothing more, or it's longer than it said)
                ret = ov_read(&sTrackVf, probe, (int)sizeof(probe), 0, 2, 1, NULL);
                if (ret > 0) why = "longer than it said";
            }
            else
            {
                const uint32_t want = glm::min(kTrackChunkBytes - n, pcm - written - n);
                ret = ov_read(&sTrackVf, (char*)(stage + n), (int)want, 0, 2, 1, NULL);
                if (ret > 0) n += (uint32_t)ret;
            }
            if (ret == 0) eof = true;
            else if (ret < 0 && ++errors > 32) why = "the decoder kept failing";
        }
        if (why != nullptr)
            break;
        if (n > 0)
        {
            AramDma(AR_MRAMTOARAM, stage, t->aram + written, n);   // (rounded up to 32: the block has room for it)
            written += n;
        }
        if (!sTrackRush)
            usleep(1000);                    // (a rest each chunk: the SD log's thread, below this one, gets a turn)
    }
    if (why == nullptr && written == 0)
        why = "no audio in it";

    if (opened) ov_clear(&sTrackVf);
    if (sTrackDisc.window) { free(sTrackDisc.window); sTrackDisc.window = nullptr; }
    if (stage) free(stage);
    sTrackDisc.windowSize = 0;

    const uint32_t ms = uint32_t((SYS_GetTimeMicroseconds() - fromUs) / 1000);
    char name[sizeof(t->name)];
    memcpy(name, t->name, sizeof(name));
    LWP_MutexLock(sTrackMutex);
    if (why == nullptr && !t->released)
    {
        t->pcmBytes = written;
        __asm__ volatile("" ::: "memory");   // (all of it in ARAM, and its size set, before AUD_Play may use it)
        t->state = TRACK_READY;
    }
    else
    {
        if (why == nullptr) why = "stopped";
        if (t->aram != 0) { AUD_FreeAram(t->aram); t->aram = 0; }
        if (t->released)
        {
            t->wave = nullptr;
            t->released = false;
            t->state = TRACK_FREE;
        }
        else
            t->state = TRACK_SKIPPED;
    }
    LWP_MutexUnlock(sTrackMutex);

    if (why == nullptr)
        LogWarning("AUDIO %s in ARAM: %u KB, %u ms%s", name, written / 1024, ms, t->raw ? " (PCM, copied)" : "");
    else if (strcmp(why, "no room") == 0)
        LogWarning("AUDIO %s not in ARAM: %u KB, %u KB free (it streams)", name, pcm / 1024, freeBytes / 1024);
    else
        LogWarning("AUDIO %s not in ARAM: %s (it streams)", name, why);
}

// Copies the tracks registered to ARAM, the smallest first (more of them ready sooner), one at a time.
static void* AudioTrackThread(void*)
{
    OctSd_NoteThreadPriority(kTrackPriority);   // (as created in AUD_Initialize: its disc reads as the I/O thread's)
    while (true)
    {
        LWP_SemWait(sTrackSem);
        while (true)
        {
            AramTrack* next = nullptr;
            LWP_MutexLock(sTrackMutex);
            for (AramTrack& t : sAramTracks)
            {
                if (t.state == TRACK_PENDING && (next == nullptr || t.size < next->size))
                    next = &t;
            }
            if (next != nullptr)
                next->state = TRACK_DECODING;
            LWP_MutexUnlock(sTrackMutex);
            if (next == nullptr)
                break;
            TrackDecode(next);
        }
    }
    return nullptr;
}

// The tracks registered so far made now, while the game loads: the main thread sleeps till none is waiting or being
// made (or timeoutMs), so the track thread -- below everything -- has the CPU, without its rests. Loading takes that
// much longer (a few seconds for a few MB of Vorbis); a track then never streams in play. Nothing without
// AudioDecodeThread=1.
void AUD_WaitForTracks(uint32_t timeoutMs)
{
    if (sTrackSem == LWP_SEM_NULL)
        return;
    const uint64_t fromUs = SYS_GetTimeMicroseconds();
    sTrackRush = true;
    LWP_SemPost(sTrackSem);                 // (should it be waiting for work)
    while (true)
    {
        bool busy = false;
        LWP_MutexLock(sTrackMutex);
        for (const AramTrack& t : sAramTracks)
            busy = busy || t.state == TRACK_PENDING || t.state == TRACK_DECODING;
        LWP_MutexUnlock(sTrackMutex);
        if (!busy || SYS_GetTimeMicroseconds() - fromUs > uint64_t(timeoutMs) * 1000u)
            break;
        usleep(2000);
    }
    sTrackRush = false;
    LogWarning("AUDIO tracks made while loading: %u ms", (unsigned)((SYS_GetTimeMicroseconds() - fromUs) / 1000));
}

// The sound's whole copy in ARAM, or nullptr (not made yet, no room, or not a track). The main thread's: a READY track
// only changes on it (AUD_ReleaseTrack).
static const AramTrack* TrackReady(SoundWave* wave)
{
    if (sTrackSem == LWP_SEM_NULL)
        return nullptr;
    for (const AramTrack& t : sAramTracks)
    {
        if (t.state == TRACK_READY && t.wave == wave)
            return &t;
    }
    return nullptr;
}

// A sound going (SoundWave::Destroy, on the main thread): its copy's ARAM freed, any voice playing from it stopped
// first. One the track thread is decoding is freed there, as it stops. True if a voice could have played from it.
bool AUD_ReleaseTrack(SoundWave* soundWave)
{
    if (sTrackSem == LWP_SEM_NULL)
        return false;
    bool hadCopy = false;
    LWP_MutexLock(sTrackMutex);
    for (AramTrack& t : sAramTracks)
    {
        if (t.state == TRACK_FREE || t.wave != soundWave)
            continue;
        if (t.state == TRACK_DECODING)
        {
            t.wave = nullptr;
            t.released = true;
            continue;
        }
        if (t.state == TRACK_READY)
        {
            hadCopy = true;
            for (uint32_t v = 0; v < AUDIO_MAX_VOICES; ++v)
            {
                if (sAramVoices[v].active && sAramVoices[v].aram == t.aram)
                {
                    AramVoiceStop(v);         // (no more of it read, from here or the audio interrupt)
                    ASND_StopVoice(v);
                }
            }
        }
        if (t.aram != 0) { AUD_FreeAram(t.aram); t.aram = 0; }
        t.wave = nullptr;
        t.state = TRACK_FREE;
    }
    LWP_MutexUnlock(sTrackMutex);
    return hadCopy;
}

#endif // PLATFORM_GAMECUBE

#if PLATFORM_GAMECUBE
// ASND keeps its mixer on the DSP for good and never yields it, and a memory card is unlocked by a
// DSP task (libogc's card.c): a card mounted while ASND runs that has to be unlocked waited forever
// (the game hung). The card is mounted at boot before ASND starts (SYS_Initialize); a mount after
// that stops ASND for it and starts it again (System_Dolphin.cpp MountMemoryCard). Voices playing
// then end; a game's own streaming voice has to notice it's unused and set it again.
static bool sAsndRunning = false;

bool AUD_SuspendForCard()
{
    if (!sAsndRunning)
        return false;
    ASND_End();
    sAsndRunning = false;
    return true;
}

void AUD_ResumeAfterCard()
{
    ASND_Init();
    ASND_Pause(0);
    sAsndRunning = true;
}
#endif

void AUD_Initialize()
{
    ASND_Init();
    ASND_Pause(0);
#if PLATFORM_GAMECUBE
    sAsndRunning = true;
    AramInit();
#endif

    // At boot, so its stack sits low in a heap not yet cut up. 64 KB was the least: an SD read goes through
    // libfat and the SD driver, and 16 KB overflowed on hardware (see SYS_CreateThread).
    // Priority 50: below the main thread (64), because SD reads busy-wait and a reader at or above
    // the main thread holds the CPU when it wakes from vsync; above the asset loader (40), so the
    // music's small reads go before the sky's big ones.
    if (LWP_SemInit(&sAudioIoSem, 0, 64) != 0)
    {
        sAudioIoSem = LWP_SEM_NULL;
    }
    else if (LWP_CreateThread(&sAudioIoThread, AudioIoThread, nullptr, nullptr, 128 * 1024, 50) != 0)   // (128 KB: it opens
                                                                                                          //  and decodes streams too)
    {
        LWP_SemDestroy(sAudioIoSem);
        sAudioIoSem = LWP_SEM_NULL;   // no thread: streams read on the main thread, as before
    }

    // The decode thread (AudioDecodeThread). Priority 56: below the main thread, so it decodes while the main thread waits
    // and never in the middle of its work (but for a voice nearly out: kDecodeBoostLeadUs); above the I/O thread, so an
    // open's header parsing there (tens of ms of CPU) can't hold a playing stream's decoding up. 128 KB, as the I/O
    // thread: it decodes, and may read the disc (libfat). Only with Config.ini's AudioDecodeThread=1 (an opt-in: without
    // it, as before).
    if (!GetEngineConfig()->mAudioDecodeThread)
    {
        sAudioDecodeSem = LWP_SEM_NULL;   // (streams decoded on the main thread, a slice a frame)
    }
    else if (LWP_SemInit(&sAudioDecodeSem, 0, 64) != 0)
    {
        sAudioDecodeSem = LWP_SEM_NULL;
    }
    else if (LWP_CreateThread(&sAudioDecodeThread, AudioDecodeThread, nullptr, nullptr, 128 * 1024, kDecodePriority) != 0)
    {
        LWP_SemDestroy(sAudioDecodeSem);
        sAudioDecodeSem = LWP_SEM_NULL;   // no thread: streams decoded on the main thread, a slice a frame, as before
        sAudioDecodeThread = LWP_THREAD_NULL;
    }

#if PLATFORM_GAMECUBE
    // The track thread (AudioTrackThread): streamed tracks copied to ARAM, with the decode thread (AudioDecodeThread=1).
    // 128 KB, as the I/O thread: it opens and decodes, and reads the disc (libfat). Without it, tracks only stream.
    if (GetEngineConfig()->mAudioDecodeThread && sAramReady && LWP_MutexInit(&sTrackMutex, false) == 0)
    {
        if (LWP_SemInit(&sTrackSem, 0, 64) != 0)
        {
            sTrackSem = LWP_SEM_NULL;
        }
        else if (LWP_CreateThread(&sAudioTrackThread, AudioTrackThread, nullptr, nullptr, 128 * 1024, kTrackPriority) != 0)
        {
            LWP_SemDestroy(sTrackSem);
            sTrackSem = LWP_SEM_NULL;
            sAudioTrackThread = LWP_THREAD_NULL;
        }
    }
#endif
}

void AUD_Shutdown()
{
    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        if (sPcmStreams[i].active)
            AUD_CloseStream(i + 1);
    }

    for (PcmSlotMemory& mem : sPcmSlotMemory)
    {
        for (uint32_t b = 0; b < AUD_PCM_STREAM_BUFFERS; ++b)
        {
            if (mem.buf[b]) free(mem.buf[b]);
        }
        if (mem.pending) free(mem.pending);
        mem = PcmSlotMemory();
    }

    ASND_End();
#if PLATFORM_GAMECUBE
    sAsndRunning = false;
#endif
}

void AUD_Update()
{
    // Decoding keeps pace with TIME, not frames. A fixed slice a frame (1/40 s) fell behind below
    // 40 fps, and then the catch-up below decoded the rest of a buffer in one frame: on hardware at
    // 28-30 fps, that was a long frame again and again.
    static uint64_t sLastUpdateUs = 0;
    const uint64_t nowUs = SYS_GetTimeMicroseconds();
    float elapsed = (sLastUpdateUs != 0 && nowUs > sLastUpdateUs) ? float(nowUs - sLastUpdateUs) / 1000000.0f : 0.0f;
    elapsed = glm::min(elapsed, 0.1f);
    sLastUpdateUs = nowUs;

#if PROFILING_ENABLED
    // What the decode thread decoded since the last frame, off the main thread: AudDecKB in the perf log (KB of PCM a
    // frame). (Its time can't be told: the main thread preempts it partway.)
    {
        const u32 level = IRQ_Disable();
        const uint32_t decoded = sDecodedBytes;
        sDecodedBytes = 0;
        IRQ_Restore(level);
        if (decoded != 0)
        {
            BEGIN_FRAME_STAT("AudDecKB");
            END_FRAME_STAT("AudDecKB");
            CpuStat* stat = GetProfiler()->FindCpuStat("AudDecKB", false);
            if (stat != nullptr)
                stat->mTime = decoded / 1024.0f;
        }
    }
#endif

    // The decode thread behind (raised above the main thread), the main thread's own decoding (the last resort) and the
    // buffers ASND refused (taken next frame), logged once a second at most.
    static uint32_t sBoosts = 0;
    static uint32_t sMainDecodes = 0;
    static uint32_t sTakeBackFails = 0;
    static uint32_t sRefused = 0;
    static uint64_t sMainDecodeLoggedUs = 0;

    // A stopped stream's decoder kept its while and not started again: left to the I/O thread to free (StreamPark).
    for (int32_t c = 0; c < kStreamCacheSlots; ++c)
    {
        StreamVoice* kept = CacheSlot(c);
        if (kept->cached && nowUs - kept->cachedAtUs > kStreamCacheUs)
            StreamRetire(kept);
    }

    // Keep streamed voices fed (the decoding asked of the decode thread; the buffers it decoded handed to ASND here).
    for (int32_t v = 0; v < AUDIO_MAX_VOICES; ++v)
    {
        StreamVoice* sv = VoiceStream(v);
        if (!sv->active) continue;

        // Opened by the I/O thread: still going, nothing to do; failed, gone; done, the voice set going
        // on its first buffer (the next one decoded from the next frame on, as ever).
        const int32_t open = sv->openState;
        if (open == OPEN_REQUESTED)
            continue;
        if (open == OPEN_FAILED)
        {
            LogError("Audio stream: failed to open Vorbis stream on disc (%s).", sv->disc.path.c_str());
            StreamStop(sv);
            continue;
        }
        if (open == OPEN_DONE)
        {
            sv->openState = OPEN_NONE;
            ASND_SetVoice(v, sv->openFormat, sv->openPitchHz, 0, sv->buf[0], sv->openFilled,
                          sv->openVolL, sv->openVolR, StreamCallback);
            sv->playEndUs = 0;
            StreamQueued(sv, sv->openFilled, nowUs);
            continue;
        }

        if (ASND_StatusVoice(v) == SND_UNUSED)
        {
            StreamStop(sv);   // voice finished / drained
            continue;
        }

        const bool aboutDry = ASND_TestVoiceBufferReady(v);

        if (sAudioDecodeSem != LWP_SEM_NULL)
        {
            // The audio ASND has left, as reckoned (StreamQueued). Normally 200-400 ms: the next buffer is decoded long
            // before what plays runs out. Under kDecodeBoostLeadUs with it not decoded (the decode thread starved: a
            // frame with no wait in it), the decode thread is raised above this one to decode it now; under
            // kDecodeMainLeadUs and still not (the thread in a disc read), this thread decodes it, as the last resort.
            const uint64_t leadUs = (sv->playEndUs > nowUs) ? sv->playEndUs - nowUs : 0;
            const bool urgent = (leadUs < kDecodeBoostLeadUs);

            // Back at a loop's start (the I/O thread): decoding goes on. Still on its way: none this frame.
            if (sv->decodeState == DECODE_NONE && sv->loopState == LOOP_DONE)
                sv->loopState = LOOP_NONE;
            const bool more = !sv->eof && sv->filled < sv->bufSize && sv->loopState == LOOP_NONE;

            if (urgent)
                StreamBoostBudget(sv);           // (before it's asked: the thread may run at once)
            if (sv->decodeState == DECODE_NONE && more)
                StreamRequestDecode(sv, urgent);
            else if (sv->decodeState == DECODE_REQUESTED && urgent)
                sv->decodeUrgent = true;
            if (urgent && sv->decodeState == DECODE_REQUESTED)
            {
                StreamBoostDecode();             // (decoded by the time this returns, unless it's on the disc)
                ++sBoosts;
            }

            if (sv->decodeState == DECODE_REQUESTED)
            {
                if (leadUs >= kDecodeMainLeadUs)
                    continue;                    // (the decode thread's still: nothing to hand over yet)
                if (!StreamTakeBack(sv))
                {
                    ++sTakeBackFails;
                    continue;
                }
            }
            if (leadUs < kDecodeMainLeadUs && !sv->eof && sv->filled < sv->bufSize && sv->loopState == LOOP_NONE &&
                StreamDecode(sv, true) > 0)
            {
                ++sMainDecodes;
            }
        }
        else
        {
            // No decode thread: a slice here every frame, as before. Back at a loop's start: decoding goes on.
            const int32_t loopNow = sv->loopState;
            if (loopNow == LOOP_DONE)
                sv->loopState = LOOP_NONE;
            if (!sv->eof && sv->filled < sv->bufSize && loopNow != LOOP_REQUESTED)
                StreamDecodeSlice(sv, aboutDry, elapsed);
        }

        // Hand it over once it is full (or is all there is) and the voice has room for it -- or, at a loop's end with
        // the start still on its way and the voice about to run dry, what there is of it. Only once ASND has it: it
        // refuses one till its mixer has taken up the last ASND_SetVoice or ASND_AddVoice (21 ms, a mix), and says
        // there's room when the voice has run dry with one queued. Thrown away then, that was 0.2 s of the music gone.
        uint8_t* buf = sv->buf[sv->nextBuf];
        const bool loopWait = (sv->loopState == LOOP_REQUESTED);
        if (sv->filled > 0 && (sv->filled >= sv->bufSize || sv->eof || loopWait) && ASND_TestVoiceBufferReady(v))
        {
            DCFlushRange(buf, sv->bufSize);
            if (ASND_AddVoice(v, buf, sv->filled) == SND_OK)
            {
                StreamQueued(sv, sv->filled, nowUs);
                sv->nextBuf = (sv->nextBuf + 1) % 3;
                sv->filled = 0;
            }
            else
                ++sRefused;                      // (tried again next frame)
        }

        // The next buffer's decoding asked of the decode thread, to run while this thread waits on vsync. (Asked every
        // frame till it's full: it stops short when the disc's next window is still being read.)
        if (sAudioDecodeSem != LWP_SEM_NULL && sv->decodeState == DECODE_NONE && !sv->eof && sv->filled < sv->bufSize &&
            sv->loopState == LOOP_NONE)
        {
            StreamRequestDecode(sv, false);
        }
    }

    if ((sBoosts != 0 || sMainDecodes != 0 || sTakeBackFails != 0) && nowUs - sMainDecodeLoggedUs >= 1000000)
    {
        LogWarning("AUDIO decode thread behind: raised above the main thread %u time(s), decoded on the main thread %u "
                   "time(s), %u wait(s) for it ran out; %u buffer(s) ASND took a frame late",
                   sBoosts, sMainDecodes, sTakeBackFails, sRefused);
        sBoosts = 0;
        sMainDecodes = 0;
        sTakeBackFails = 0;
        sRefused = 0;
        sMainDecodeLoggedUs = nowUs;
    }

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        if (sPcmStreams[i].active)
            PcmFeed(sPcmStreams[i]);
    }
}

void AUD_Play(
    uint32_t voiceIndex,
    SoundWave* soundWave,
    float volume,
    float pitch,
    bool loop,
    float startTime,
    bool spatial)
{
#if PLATFORM_GAMECUBE
    AramVoiceStop(voiceIndex);      // whatever this voice played before
#endif

    //float startPercent = startTime / soundWave->GetDuration();
    //startPercent = glm::clamp(startPercent, 0.0f, 1.0f);
    //uint32_t startingSample = uint32_t(startPercent * soundWave->GetNumSamples());

    int32_t volumeInt = int32_t(volume * MID_VOLUME);
    int32_t pitchHz = int32_t(soundWave->GetSampleRate() * pitch);
    int32_t voiceFormat = 0;

    sSampleRates[voiceIndex] = soundWave->GetSampleRate();

    bool stereo = (soundWave->GetNumChannels() == 2);
    bool bit16 = (soundWave->GetBitsPerSample() == 16);

    // Wave data is stored in little endian format
    if (stereo)
    {
        voiceFormat = bit16 ? VOICE_STEREO_16BIT_LE : VOICE_STEREO_8BIT_U;
    }
    else
    {
        voiceFormat = bit16 ? VOICE_MONO_16BIT_LE : VOICE_MONO_8BIT_U;
    }

    // Streamed sound: decode-on-demand from the in-RAM compressed Vorbis rather
    // than playing a fully-decoded PCM buffer.
    const bool fromDisc = soundWave->GetStream() && soundWave->GetDiscSize() > 0;

#if PLATFORM_GAMECUBE
    // A disc stream's whole copy in ARAM (AramTrack): played from there, as a sound effect is -- nothing decoded as it
    // plays. Till it's ready, streamed below.
    const AramTrack* track = fromDisc ? TrackReady(soundWave) : nullptr;
    if (track != nullptr)
    {
        StreamStop(VoiceStream(voiceIndex));   // (any stream on this voice before)
        LogWarning("AUDIO start %s (from ARAM)", soundWave->GetName().c_str());
        AramVoiceStartAt(voiceIndex, track->aram, track->pcmBytes, voiceFormat, pitchHz,
                         spatial ? 0 : volumeInt, spatial ? 0 : volumeInt, loop);
        return;
    }
#endif

    if (fromDisc || (soundWave->GetStream() && soundWave->GetCompressedData() != nullptr))
    {
        StreamStop(VoiceStream(voiceIndex));   // clean any previous stream on this voice (the I/O thread's to free, if busy)
        // The same sound stopped a moment ago (StreamPark): its decoder, still open, only seeked back to the start by
        // the I/O thread -- the same audio from the same first sample, as a loop starts again.
        StreamVoice* sv = (fromDisc && sAudioIoSem != LWP_SEM_NULL) ? StreamTakeCached(voiceIndex, soundWave) : nullptr;
        const bool restart = (sv != nullptr);
        if (sv == nullptr)
            sv = StreamFor(voiceIndex);
        sv->active = true;  // so a failure below can StreamStop() what it allocated
        sv->buf[0] = sv->buf[1] = sv->buf[2] = nullptr;
        sv->memOwned = nullptr;
        sv->raw = fromDisc && soundWave->GetDiscPcm();   // (never parked, so never a restart)

        if (fromDisc)
        {
            LogWarning("AUDIO start %s (streamed from the disc%s)", soundWave->GetName().c_str(), restart ? ", kept open" : "");
            // The audio never came off the disc: open it where it lies. Costs one small
            // window of memory however long the track is.
            sv->disc.path = soundWave->GetDiscPath();
            sv->disc.base = soundWave->GetDiscOffset();
            sv->disc.size = soundWave->GetDiscSize();
            if (sv->raw)
            {
                const uint32_t frame = soundWave->GetNumChannels() * (bit16 ? 2 : 1);
                sv->disc.size -= sv->disc.size % glm::max<uint32_t>(frame, 1);   // (whole sample frames)
            }
            if (!restart)
                sv->disc.pos = 0;   // (a restart's is where its decoder left it: vorbisfile seeks from what it knows)
            sv->disc.windowAt = 0;
            sv->disc.windowSize = 0;
            sv->disc.window = (uint8_t*)malloc(kDiscWindowBytes);
            // The read-ahead window. Without it (no memory) the stream still plays, reading here.
            sv->disc.ahead = (uint8_t*)malloc(kDiscWindowBytes);
            sv->disc.aheadState = AHEAD_EMPTY;

            // Opened by the I/O thread when there is one (asked below, once the buffers are made);
            // here only without one.
            ov_callbacks dcb = { DiscRead, DiscSeek, MemClose, DiscTell };
            if (sv->disc.window == nullptr ||
                (sAudioIoSem == LWP_SEM_NULL && !sv->raw && ov_open_callbacks(&sv->disc, &sv->vf, NULL, 0, dcb) < 0))
            {
                LogError("SoundWave '%s': failed to open Vorbis stream on disc.", soundWave->GetName().c_str());
                WaitReadAhead(&sv->disc);
                if (sv->disc.window) { free(sv->disc.window); sv->disc.window = nullptr; }
                if (sv->disc.ahead) { free(sv->disc.ahead); sv->disc.ahead = nullptr; }
                if (sv->vfOpen) { ov_clear(&sv->vf); sv->vfOpen = false; }   // (a restart's kept decoder)
                sv->active = false;
                return;
            }
            if (!restart)
                sv->vfOpen = (sAudioIoSem == LWP_SEM_NULL && !sv->raw);
        }
        else
        {

        // Own a private copy of the compressed Vorbis so the stream is independent
        // of the SoundWave asset's lifetime. In non-embedded builds the asset can be
        // swept (ref count -> 0) while the music is still playing; keeping the asset's
        // pointer would dangle, and the freed block -- reused for other allocations --
        // corrupts memory (scene-wide geometry collapse). Embedded never unloads
        // assets, which is why this only bit non-embedded builds.
        uint32_t compressedSize = soundWave->GetCompressedSize();
        sv->memOwned = (uint8_t*)malloc(compressedSize);
        if (sv->memOwned == nullptr)
        {
            LogError("SoundWave '%s': failed to allocate %u-byte stream copy.",
                     soundWave->GetName().c_str(), compressedSize);
            sv->active = false;
            return;
        }
        memcpy(sv->memOwned, soundWave->GetCompressedData(), compressedSize);

        sv->mem.data = sv->memOwned;
        sv->mem.size = compressedSize;
        sv->mem.pos  = 0;

        ov_callbacks cb = { MemRead, MemSeek, MemClose, MemTell };
        if (ov_open_callbacks(&sv->mem, &sv->vf, NULL, 0, cb) < 0)
        {
            LogError("SoundWave '%s': failed to open Vorbis stream.", soundWave->GetName().c_str());
            free(sv->memOwned);
            sv->memOwned = nullptr;
            sv->active = false;
            return;
        }
        sv->vfOpen = true;
        }

        sv->active = true;
        sv->channels = (int)soundWave->GetNumChannels();
        sv->bytesPerSample = bit16 ? 2 : 1;
        sv->loop = loop;
        sv->eof = false;
        sv->nextBuf = 1;
        sv->pitchHz = pitchHz;
        sv->playEndUs = 0;

        // ~0.2s double buffers.
        uint32_t bytesPerSec = soundWave->GetSampleRate() * sv->channels * sv->bytesPerSample;
        sv->bytesPerSec = bytesPerSec;
        sv->bufSize = ((bytesPerSec / 5) + 31) & ~31u;
        sv->buf[0] = (uint8_t*)memalign(32, sv->bufSize);
        sv->buf[1] = (uint8_t*)memalign(32, sv->bufSize);
        sv->buf[2] = (uint8_t*)memalign(32, sv->bufSize);

        // A frame's slice: 1/40 s of audio. At 60 frames a second the decoder gains on the playback
        // (1.5x); 1/25 s measured 18-19 ms worst frames, a shade over the 16.7 ms budget. A game
        // running slower than 40 fps falls behind here, and the catch-up in AUD_Update (finish the
        // buffer at once when the voice is about to run dry) covers it. Whole sample frames only.
        const uint32_t frameBytes = uint32_t(sv->channels * sv->bytesPerSample);
        sv->sliceBytes = bytesPerSec / 40;
        sv->sliceBytes -= sv->sliceBytes % frameBytes;
        sv->filled = 0;

        if (sv->buf[0] == nullptr || sv->buf[1] == nullptr || sv->buf[2] == nullptr)
        {
            LogError("SoundWave '%s': failed to allocate stream buffers.", soundWave->GetName().c_str());
            StreamStop(sv);
            return;
        }

        if (fromDisc && sAudioIoSem != LWP_SEM_NULL && (restart || !sv->vfOpen))
        {
            // The I/O thread opens it (or, kept open, seeks it to its start) and decodes the first buffer;
            // AUD_Update sets the voice then.
            sv->openFormat = voiceFormat;
            sv->openPitchHz = pitchHz;
            sv->openVolL = sv->openVolR = spatial ? 0 : volumeInt;
            sv->openFilled = 0;
            sv->restart = restart;
            __asm__ volatile("" ::: "memory");
            sv->openState = OPEN_REQUESTED;
            LWP_SemPost(sAudioIoSem);
            return;
        }

        uint32_t n0 = StreamFill(sv, sv->buf[0], sv->bufSize);   // the first buffer whole: it has to start
        DCFlushRange(sv->buf[0], sv->bufSize);
        ASND_SetVoice(
            voiceIndex, voiceFormat, pitchHz, 0,
            sv->buf[0], n0,
            spatial ? 0 : volumeInt, spatial ? 0 : volumeInt,
            StreamCallback);
        StreamQueued(sv, n0, SYS_GetTimeMicroseconds());
        if (sAudioDecodeSem != LWP_SEM_NULL && !sv->eof)
            StreamRequestDecode(sv, false);   // (the second buffer, decoded while this frame waits on vsync)
        return;
    }

#if PLATFORM_GAMECUBE
    if (soundWave->GetAramAddress() != 0)
    {
        AramVoiceStart(voiceIndex, soundWave, voiceFormat, pitchHz,
                       spatial ? 0 : volumeInt, spatial ? 0 : volumeInt, loop);
        return;
    }
#endif

    if (loop)
    {
        ASND_SetInfiniteVoice(
            voiceIndex,
            voiceFormat,
            pitchHz,
            0,
            soundWave->GetWaveData(),
            soundWave->GetWaveDataSize(),
            spatial ? 0 : volumeInt,
            spatial ? 0 : volumeInt);
    }
    else
    {
        ASND_SetVoice(
            voiceIndex,
            voiceFormat,
            pitchHz,
            0,
            soundWave->GetWaveData(),
            soundWave->GetWaveDataSize(),
            spatial ? 0 : volumeInt,
            spatial ? 0 : volumeInt,
            nullptr);
    }
}

void AUD_Stop(uint32_t voiceIndex)
{
#if PLATFORM_GAMECUBE
    AramVoiceStop(voiceIndex);
#endif
    ASND_StopVoice(voiceIndex);

    if (voiceIndex < AUDIO_MAX_VOICES)
    {
        StreamStop(VoiceStream(voiceIndex));
    }
}

// Whether a voice fed buffer by buffer (a callback set) has played its last one. libasnd never
// ends such a voice itself -- it clears a voice's state at its end only when it has NO callback --
// so ASND_StatusVoice goes on saying SND_WORKING for ever. What does show the end: once the last
// buffer runs out with nothing queued, the voice's current buffer is cleared, and ASND_TestPointer
// finds none of its buffers in use.
static bool VoiceDrained(uint32_t voice, uint8_t* const* bufs, int count)
{
    for (int b = 0; b < count; ++b)
    {
        if (bufs[b] != nullptr && ASND_TestPointer(voice, bufs[b]) == SND_BUSY)
            return false;
    }
    return true;
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (voiceIndex < AUDIO_MAX_VOICES)
    {
        // Played to the end: stopped here, or the voice would count as busy for ever and a sound
        // played one at a time would never play again (measured on hardware: effects that
        // played once and then no more).
#if PLATFORM_GAMECUBE
        AramVoice& av = sAramVoices[voiceIndex];
        if (av.active && !av.loop && av.pos >= av.size && VoiceDrained(voiceIndex, av.buf, 2))
        {
            AUD_Stop(voiceIndex);
            return false;
        }
#endif
        StreamVoice* sv = VoiceStream(voiceIndex);
        if (sv->active && (sv->openState == OPEN_REQUESTED || sv->openState == OPEN_DONE))
            return true;   // (still being opened by the I/O thread: playing, as far as anyone asks)
        if (sv->active && sv->decodeState == DECODE_NONE && sv->eof && sv->filled == 0 && VoiceDrained(voiceIndex, sv->buf, 3))
        {
            AUD_Stop(voiceIndex);
            return false;
        }
    }

    int32_t status = ASND_StatusVoice(voiceIndex);
    return status == SND_WORKING;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    int32_t leftVolInt = int32_t(MID_VOLUME * leftVolume);
    int32_t rightVolInt = int32_t(MID_VOLUME * rightVolume);
    ASND_ChangeVolumeVoice(voiceIndex, leftVolInt, rightVolInt);
    if (voiceIndex < AUDIO_MAX_VOICES && VoiceStream(voiceIndex)->openState != OPEN_NONE)
    {
        VoiceStream(voiceIndex)->openVolL = leftVolInt;   // (not set going yet: it starts at this)
        VoiceStream(voiceIndex)->openVolR = rightVolInt;
    }
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    int32_t pitchHz = int32_t(pitch * sSampleRates[voiceIndex]);
    ASND_ChangePitchVoice(voiceIndex, pitchHz);
    if (voiceIndex < AUDIO_MAX_VOICES)
        VoiceStream(voiceIndex)->pitchHz = pitchHz;   // (a stream's reckoning of what ASND has left: StreamQueued)
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size)
{
    return (uint8_t*)SYS_AlignedMalloc(size, 32);
}

void AUD_FreeWaveBuffer(void* buffer)
{
    SYS_AlignedFree(buffer);
}

void AUD_ProcessWaveBuffer(SoundWave* soundWave)
{
#if PLATFORM_GAMECUBE
    AramStoreWave(soundWave);     // a sound effect's PCM moves to ARAM
    TrackRegister(soundWave);     // a disc stream's copy made in ARAM, in the background (AramTrack)
#else
    (void)soundWave;
#endif
}

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels)
{
    if (sampleRate == 0 || numChannels < 1 || numChannels > 2)
        return 0;

    OCT_ASSERT(AUDIO_MAX_VOICES + AUD_MAX_PCM_STREAMS <= MAX_SND_VOICES);

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        PcmStream& s = sPcmStreams[i];
        if (s.active)
            continue;

        s = PcmStream();
        s.voice = AUDIO_MAX_VOICES + i;
        s.format = (numChannels == 2) ? VOICE_STEREO_16BIT_LE : VOICE_MONO_16BIT_LE;
        s.sampleRate = sampleRate;
        s.frameBytes = 2 * numChannels;

        // ~100 ms per buffer.
        s.bufCapacity = (sampleRate * s.frameBytes) / 10;
        s.bufCapacity -= s.bufCapacity % s.frameBytes;

        uint32_t allocSize = (s.bufCapacity + 31) & ~31u;
        PcmSlotMemory& mem = sPcmSlotMemory[i];
        bool allocated = true;

        // Reuse the slot's buffers when they're big enough.
        if (mem.bufAlloc < allocSize)
        {
            for (uint32_t b = 0; b < AUD_PCM_STREAM_BUFFERS; ++b)
            {
                if (mem.buf[b]) free(mem.buf[b]);
                mem.buf[b] = (uint8_t*)memalign(32, allocSize);
                allocated = allocated && (mem.buf[b] != nullptr);
            }
            mem.bufAlloc = allocated ? allocSize : 0;
        }

        // Room for 2 s of queued audio up front, so queueing doesn't allocate; it grows if a game
        // queues more. 1 s wasn't enough: a game topping up at 1 s with 0.5 s chunks can have 1.5 s
        // waiting, more when slow frames hold back feeding, and growing on a fragmented heap
        // failed and dropped audio.
        const uint32_t pendingWanted = sampleRate * s.frameBytes * 2;
        if (allocated && mem.pendingAlloc < pendingWanted)
        {
            if (mem.pending) free(mem.pending);
            mem.pending = (uint8_t*)malloc(pendingWanted);
            mem.pendingAlloc = (mem.pending != nullptr) ? pendingWanted : 0;
            allocated = (mem.pending != nullptr);
        }

        if (!allocated)
        {
            LogError("AUD_OpenStream: failed to allocate stream buffers.");
            OctLog("AUD_OpenStream: failed to allocate stream buffers (slot %u, 3 x %u-byte buffers, %u-byte queue)",
                   i + 1, allocSize, pendingWanted);
            s = PcmStream();
            return 0;
        }

        for (uint32_t b = 0; b < AUD_PCM_STREAM_BUFFERS; ++b)
        {
            s.buf[b] = mem.buf[b];
        }
        s.pending = mem.pending;
        s.pendingCapacity = mem.pendingAlloc;

        s.active = true;
        return i + 1;
    }

    return 0;
}

void AUD_CloseStream(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    ASND_StopVoice(s->voice);

    // The slot keeps its buffers for the next stream (see PcmSlotMemory).
    *s = PcmStream();
}

void AUD_QueueStreamData(uint32_t streamId, const uint8_t* data, uint32_t size)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr || data == nullptr || size == 0)
        return;

    // Move what hasn't been handed to ASND yet to the front, then append.
    if (s->pendingHead > 0)
    {
        memmove(s->pending, s->pending + s->pendingHead, s->pendingSize - s->pendingHead);
        s->pendingSize -= s->pendingHead;
        s->pendingHead = 0;
    }

    if (size > s->pendingCapacity - s->pendingSize)
    {
        // More queued than the buffer holds: grow it, or drop this data if there's no memory
        // (a gap in the sound, instead of a hang).
        const uint32_t needed = s->pendingSize + size;
        uint8_t* grown = (uint8_t*)realloc(s->pending, needed);
        if (grown == nullptr)
        {
            LogError("AUD_QueueStreamData: out of memory, dropped %u bytes.", size);
            OctLog("AUD_QueueStreamData: out of memory, dropped %u bytes (stream %u, %u bytes waiting, %u-byte queue)",
                   size, streamId, s->pendingSize, s->pendingCapacity);
            PcmFeed(*s);
            return;
        }
        OctLog("AUD_QueueStreamData: stream %u queue grew from %u to %u bytes", streamId, s->pendingCapacity, needed);
        s->pending = grown;
        s->pendingCapacity = needed;
        sPcmSlotMemory[streamId - 1].pending = grown;
        sPcmSlotMemory[streamId - 1].pendingAlloc = needed;
    }

    memcpy(s->pending + s->pendingSize, data, size);
    s->pendingSize += size;

    PcmFeed(*s);
}

uint64_t AUD_GetStreamPlayedFrames(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr || !s->started)
        return 0;

    // ASND counts 48 kHz output samples (in steps of 1024) while the voice has data
    // and isn't paused, so this is how much of the stream has actually played.
    uint32_t ticks = ASND_GetTickCounterVoice(s->voice);
    if (ticks < s->tickBase)
    {
        s->tickBase = 0;    // ASND_SetVoice reset the counter
    }

    // Never more than was handed to ASND. The tick count runs ahead of it while the voice starves
    // (e.g. frames too slow to feed it), and a game topping up by the played count would then
    // queue more and more until the queue overflowed and dropped audio.
    const uint64_t tickFrames = uint64_t(ticks - s->tickBase) * s->sampleRate / 48000;
    return glm::min(tickFrames, s->fedFrames);
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr || s->paused == paused)
        return;

    s->paused = paused;

    if (s->started)
        ASND_PauseVoice(s->voice, paused ? 1 : 0);

    if (!paused)
        PcmFeed(*s);
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    int32_t volumeInt = int32_t(glm::clamp(volume, 0.0f, 2.0f) * MID_VOLUME);
    if (volumeInt == s->volume)
        return;

    s->volume = volumeInt;

    if (s->started)
        ASND_ChangeVolumeVoice(s->voice, volumeInt, volumeInt);
}

void AUD_FlushStream(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    ASND_StopVoice(s->voice);
    s->started = false;
    s->pendingSize = 0;
    s->pendingHead = 0;
    s->fedFrames = 0;
}

#endif