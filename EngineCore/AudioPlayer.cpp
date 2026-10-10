#include "AudioPlayer.h"
#include "EngineCore.h"

#include <algorithm>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

AudioPlayer::AudioPlayer()
{
    m_playThread = std::thread(&AudioPlayer::PlaybackWorker, this);
}

AudioPlayer::~AudioPlayer()
{
    Stop();
    m_stopThread.store(true);
    if (m_playThread.joinable())
    {
        m_playThread.join();
    }
}

void AudioPlayer::LoadClips(const std::vector<VideoClipDesc>& clips, int64_t totalDurationMs)
{
    Pause();

    std::lock_guard<std::mutex> lock(m_pcmLock);
    m_totalDurationMs = totalDurationMs;

    int64_t totalSamples = (totalDurationMs * 44100) / 1000;
    if (totalSamples <= 0 || clips.empty())
    {
        m_pcmData.clear();
        return;
    }

    // Allocate stereo 16-bit PCM buffer (samples * 2)
    m_pcmData.assign(static_cast<size_t>(totalSamples * 2), 0);

    for (const auto& clip : clips)
    {
        int uLen = WideCharToMultiByte(CP_UTF8, 0, clip.FilePath, -1, nullptr, 0, nullptr, nullptr);
        if (uLen <= 0) continue;
        std::string uPath(uLen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, clip.FilePath, -1, &uPath[0], uLen, nullptr, nullptr);

        AVFormatContext* fmtCtx = nullptr;
        if (avformat_open_input(&fmtCtx, uPath.c_str(), nullptr, nullptr) != 0 || !fmtCtx)
        {
            continue;
        }
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
        {
            avformat_close_input(&fmtCtx);
            continue;
        }

        const AVCodec* decoder = nullptr;
        int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (streamIdx < 0 || !decoder)
        {
            avformat_close_input(&fmtCtx);
            continue;
        }

        AVCodecContext* decCtx = avcodec_alloc_context3(decoder);
        if (!decCtx)
        {
            avformat_close_input(&fmtCtx);
            continue;
        }

        if (avcodec_parameters_to_context(decCtx, fmtCtx->streams[streamIdx]->codecpar) < 0 ||
            avcodec_open2(decCtx, decoder, nullptr) < 0)
        {
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            continue;
        }

        SwrContext* swr = nullptr;
        AVChannelLayout stereoLayout;
        av_channel_layout_default(&stereoLayout, 2);
        swr_alloc_set_opts2(&swr, &stereoLayout, AV_SAMPLE_FMT_S16, 44100,
                            &decCtx->ch_layout, decCtx->sample_fmt, decCtx->sample_rate,
                            0, nullptr);

        if (!swr || swr_init(swr) < 0)
        {
            if (swr) swr_free(&swr);
            avcodec_free_context(&decCtx);
            avformat_close_input(&fmtCtx);
            continue;
        }

        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();

        int16_t resampleBuf[8192]; // Interleaved stereo S16: 4096 samples * 2
        uint8_t* outData[1] = { reinterpret_cast<uint8_t*>(resampleBuf) };

        int64_t clipStartSample = (clip.StartTimeMs * 44100) / 1000;
        int64_t clipEndSample = ((clip.StartTimeMs + clip.DurationMs) * 44100) / 1000;
        int64_t curSampleOffset = clipStartSample;

        while (av_read_frame(fmtCtx, pkt) >= 0)
        {
            if (pkt->stream_index == streamIdx)
            {
                if (avcodec_send_packet(decCtx, pkt) >= 0)
                {
                    while (avcodec_receive_frame(decCtx, frame) >= 0)
                    {
                        int outCount = swr_convert(swr, outData, 4096,
                            const_cast<const uint8_t**>(frame->data), frame->nb_samples);

                        for (int s = 0; s < outCount; ++s)
                        {
                            int64_t targetSample = curSampleOffset + s;
                            if (targetSample >= 0 && targetSample < totalSamples && targetSample < clipEndSample)
                            {
                                size_t idx = static_cast<size_t>(targetSample * 2);
                                int32_t left = static_cast<int32_t>(m_pcmData[idx]) + resampleBuf[s * 2];
                                int32_t right = static_cast<int32_t>(m_pcmData[idx + 1]) + resampleBuf[s * 2 + 1];
                                m_pcmData[idx] = static_cast<int16_t>((std::clamp)(left, -32768, 32767));
                                m_pcmData[idx + 1] = static_cast<int16_t>((std::clamp)(right, -32768, 32767));
                            }
                        }
                        curSampleOffset += outCount;
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(pkt);
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
    }
}

void AudioPlayer::Play(int64_t startPosMs)
{
    m_currentSampleIdx.store((startPosMs * 44100) / 1000);
    m_seekRequested.store(true);
    m_isPlaying.store(true);
}

void AudioPlayer::Pause()
{
    m_isPlaying.store(false);
}

void AudioPlayer::Seek(int64_t posMs)
{
    m_currentSampleIdx.store((posMs * 44100) / 1000);
    m_seekRequested.store(true);
}

void AudioPlayer::Stop()
{
    m_isPlaying.store(false);
    m_currentSampleIdx.store(0);
    m_seekRequested.store(true);
}

void AudioPlayer::PlaybackWorker()
{
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 44100 * 4;

    HWAVEOUT hWaveOut = nullptr;
    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        return;
    }

    const int BUFFER_SAMPLES = 2205; // 50 ms buffer
    const int BUFFER_BYTES = BUFFER_SAMPLES * 4;

    const int NUM_BUFFERS = 3;
    WAVEHDR hdrs[NUM_BUFFERS] = {};
    std::vector<int16_t> rawBuffers[NUM_BUFFERS];
    bool inFlight[NUM_BUFFERS] = { false };
    for (int i = 0; i < NUM_BUFFERS; ++i)
    {
        rawBuffers[i].resize(BUFFER_SAMPLES * 2, 0);
        hdrs[i].lpData = reinterpret_cast<LPSTR>(rawBuffers[i].data());
        hdrs[i].dwBufferLength = BUFFER_BYTES;
        hdrs[i].dwFlags = 0;
    }

    int curBuf = 0;

    while (!m_stopThread.load())
    {
        if (!m_isPlaying.load())
        {
            // If we just paused, reset waveOut and unprepare all in-flight buffers
            bool hadInFlight = false;
            for (int i = 0; i < NUM_BUFFERS; ++i)
            {
                if (inFlight[i]) { hadInFlight = true; break; }
            }
            if (hadInFlight)
            {
                waveOutReset(hWaveOut);
                for (int i = 0; i < NUM_BUFFERS; ++i)
                {
                    if (inFlight[i])
                    {
                        waveOutUnprepareHeader(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
                        inFlight[i] = false;
                    }
                }
                curBuf = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // If seek was requested, reset hardware and clear in-flight buffers
        if (m_seekRequested.exchange(false))
        {
            waveOutReset(hWaveOut);
            for (int i = 0; i < NUM_BUFFERS; ++i)
            {
                if (inFlight[i])
                {
                    waveOutUnprepareHeader(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
                    inFlight[i] = false;
                }
            }
            curBuf = 0;
        }

        // If current buffer is currently in flight, wait for it to finish
        if (inFlight[curBuf])
        {
            while ((hdrs[curBuf].dwFlags & WHDR_DONE) == 0 && m_isPlaying.load() && !m_stopThread.load() && !m_seekRequested.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }

            if (m_stopThread.load() || !m_isPlaying.load() || m_seekRequested.load())
            {
                continue;
            }

            waveOutUnprepareHeader(hWaveOut, &hdrs[curBuf], sizeof(WAVEHDR));
            inFlight[curBuf] = false;
        }

        // Fill buffer
        int64_t sampleIdx = m_currentSampleIdx.load();
        int64_t totalSamples = 0;
        {
            std::lock_guard<std::mutex> lock(m_pcmLock);
            totalSamples = static_cast<int64_t>(m_pcmData.size() / 2);
            if (sampleIdx >= 0 && sampleIdx < totalSamples)
            {
                int64_t toCopy = (std::min)(static_cast<int64_t>(BUFFER_SAMPLES), totalSamples - sampleIdx);
                memcpy(rawBuffers[curBuf].data(), &m_pcmData[sampleIdx * 2], toCopy * 4);
                if (toCopy < BUFFER_SAMPLES)
                {
                    memset(rawBuffers[curBuf].data() + toCopy * 2, 0, (BUFFER_SAMPLES - toCopy) * 4);
                }
            }
            else
            {
                memset(rawBuffers[curBuf].data(), 0, BUFFER_BYTES);
            }
        }

        m_currentSampleIdx.store(sampleIdx + BUFFER_SAMPLES);

        hdrs[curBuf].dwBufferLength = BUFFER_BYTES;
        hdrs[curBuf].dwFlags = 0;
        waveOutPrepareHeader(hWaveOut, &hdrs[curBuf], sizeof(WAVEHDR));
        MMRESULT res = waveOutWrite(hWaveOut, &hdrs[curBuf], sizeof(WAVEHDR));
        if (res == MMSYSERR_NOERROR)
        {
            inFlight[curBuf] = true;
        }
        else
        {
            waveOutUnprepareHeader(hWaveOut, &hdrs[curBuf], sizeof(WAVEHDR));
            inFlight[curBuf] = false;
        }

        curBuf = (curBuf + 1) % NUM_BUFFERS;
    }

    waveOutReset(hWaveOut);
    for (int i = 0; i < NUM_BUFFERS; ++i)
    {
        if (inFlight[i])
        {
            waveOutUnprepareHeader(hWaveOut, &hdrs[i], sizeof(WAVEHDR));
        }
    }
    waveOutClose(hWaveOut);
}
