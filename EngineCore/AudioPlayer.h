#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmeapi.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>

#pragma comment(lib, "winmm.lib")

struct VideoClipDesc;

class AudioPlayer
{
public:
    AudioPlayer();
    ~AudioPlayer();

    void LoadClips(const std::vector<VideoClipDesc>& clips, int64_t totalDurationMs);
    void Play(int64_t startPosMs);
    void Pause();
    void Seek(int64_t posMs);
    void Stop();

private:
    void PlaybackWorker();

    std::vector<int16_t> m_pcmData; // 44100Hz 16-bit Stereo interleaved
    std::mutex m_pcmLock;
    int64_t m_totalDurationMs = 0;

    std::atomic<bool> m_isPlaying{ false };
    std::atomic<bool> m_stopThread{ false };
    std::atomic<bool> m_seekRequested{ false };
    std::atomic<int64_t> m_currentSampleIdx{ 0 };

    std::thread m_playThread;
};
