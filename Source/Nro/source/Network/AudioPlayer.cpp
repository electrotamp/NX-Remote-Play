#include "AudioPlayer.hpp"

#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <thread>
#include <vector>

namespace NroAudio
{

namespace
{

// 48 kHz stereo s16 = 192000 B/s. One slot is 0x2000 B (two 0x1000 batch units, what the clients ask
// for with AudioBatching = 1) = 42.7 ms. Six slots in flight = ~256 ms of audio inside audout at most.
constexpr size_t kSlotBytes = 0x2000;
constexpr int kSlots = 6;
constexpr size_t kMaxQueued = 4;   // queued chunks waiting for a slot; beyond this the oldest is dropped (~170 ms)
constexpr size_t kPrimeChunks = 2; // chunks queued before (re)starting playback after silence (~85 ms)

void (*g_log)(const std::string &) = nullptr;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex; // guards g_queue
std::deque<std::vector<uint8_t>> g_queue;
std::atomic<unsigned> g_received{0}, g_dropped{0};
bool g_audoutOpen = false;

void Log(const std::string &s)
{
    if (g_log)
        g_log(s);
}
std::string Hex(Result rc)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%x", (unsigned)rc);
    return b;
}

void PlaybackThread()
{
    AudioOutBuffer slots[kSlots];
    void *mem[kSlots];
    for (int i = 0; i < kSlots; i++)
    {
        mem[i] = memalign(0x1000, kSlotBytes);
        if (mem[i])
            memset(mem[i], 0, kSlotBytes);
        slots[i] = {};
        slots[i].buffer = mem[i];
        slots[i].buffer_size = kSlotBytes;
    }

    int next = 0, inFlight = 0;
    unsigned appended = 0, underruns = 0;
    bool everPlayed = false;
    u64 lastReport = armGetSystemTick();

    while (g_running.load())
    {
        // Reclaim finished slots (released strictly in submission order).
        if (inFlight > 0)
        {
            AudioOutBuffer *released = nullptr;
            u32 count = 0;
            if (R_SUCCEEDED(audoutWaitPlayFinish(&released, &count, 5ULL * 1000 * 1000)) && count > 0)
            {
                inFlight -= (int)count;
                if (inFlight < 0)
                    inFlight = 0;
                if (inFlight == 0 && everPlayed)
                    ++underruns;
            }
        }
        else
            svcSleepThread(5ULL * 1000 * 1000);

        // Fill free slots from the queue; after silence wait for a small cushion first.
        while (inFlight < kSlots && mem[next])
        {
            std::vector<uint8_t> chunk;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_queue.empty() || (inFlight == 0 && g_queue.size() < kPrimeChunks))
                    break;
                chunk = std::move(g_queue.front());
                g_queue.pop_front();
            }
            AudioOutBuffer &slot = slots[next];
            memcpy(mem[next], chunk.data(), chunk.size());
            slot.data_size = chunk.size();
            slot.data_offset = 0;
            if (R_FAILED(audoutAppendAudioOutBuffer(&slot)))
                break;
            next = (next + 1) % kSlots;
            ++inFlight;
            ++appended;
            everPlayed = true;
            if (appended == 1)
                Log("Audio: playback started");
        }

        const u64 now = armGetSystemTick();
        if ((now - lastReport) / armGetSystemTickFreq() >= 10)
        {
            lastReport = now;
            Log("Audio: received " + std::to_string(g_received.load()) + " chunks, played " + std::to_string(appended) +
                ", dropped " + std::to_string(g_dropped.load()) + ", underruns " + std::to_string(underruns) +
                ", in flight " + std::to_string(inFlight));
        }
    }

    // Stop() stops audout only after this thread is joined, so the slots are no longer in use here.
    for (int i = 0; i < kSlots; i++)
        free(mem[i]);
}

} // namespace

bool Start(void (*log)(const std::string &))
{
    Stop();
    g_log = log;
    g_received = 0;
    g_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    }

    Result rc = audoutInitialize();
    if (R_FAILED(rc))
    {
        Log("Audio: audoutInitialize failed rc=" + Hex(rc) + " - continuing without sound");
        return false;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc))
    {
        Log("Audio: audoutStartAudioOut failed rc=" + Hex(rc) + " - continuing without sound");
        audoutExit();
        return false;
    }
    g_audoutOpen = true;
    Log("Audio: audout ready (" + std::to_string(audoutGetSampleRate()) + " Hz, " +
        std::to_string(audoutGetChannelCount()) + " ch)");
    g_running.store(true);
    g_thread = std::thread(PlaybackThread);
    return true;
}

void Push(const uint8_t *pcm, size_t size)
{
    if (!g_running.load() || size < 4)
        return;
    size &= ~(size_t)3; // whole stereo s16 frames only
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t off = 0; off < size; off += kSlotBytes)
    {
        const size_t n = size - off < kSlotBytes ? size - off : kSlotBytes;
        if (g_queue.size() >= kMaxQueued)
        {
            g_queue.pop_front();
            g_dropped.fetch_add(1);
        }
        g_queue.emplace_back(pcm + off, pcm + off + n);
        g_received.fetch_add(1);
    }
}

void Stop()
{
    g_running.store(false);
    if (g_thread.joinable())
        g_thread.join();
    if (g_audoutOpen)
    {
        audoutStopAudioOut();
        audoutExit();
        g_audoutOpen = false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
}

} // namespace NroAudio

