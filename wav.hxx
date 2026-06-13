#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ios>
#include <mutex>
#include <stdexcept>
#include <thread>

constexpr static inline size_t DECODE_BUFFER_SAMPLES = 16384;
constexpr static inline size_t MAX_QUEUE_BUFFERS = 8;

enum AudioFormat : int {
  PCM = 1,
  IEEE_FLOAT = 3,
  ALAW = 6,
  MULAW = 7,
  EXTENSIBLE = 0xFFFE
};

enum AudioType : int {
  PCM8 = 0,
  PCM16,
  PCM24,
  PCM32,
  IEEE,
  ALAW_CODEC,
  MULAW_CODEC
};

struct RIFF_Header {
  char riff[4];
  uint32_t file_size;
  char wave[4];
};

struct Chunk_Header {
  char id[4];
  uint32_t size;
};

struct FMT_Header {
  uint16_t audio_format;
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t block_align;
  uint16_t bits_per_sample;
};

struct FMT_Extensible {
  uint16_t cbSize;
  uint16_t validBitsPerSample;
  uint32_t channelMask;
  uint8_t subFormat[16];
};

struct AudioChunk {
  alignas(16) float samples[DECODE_BUFFER_SAMPLES];
  size_t frames = 0;
  size_t sampleCount = 0;
};

class WavCodec {
private:
  FILE *file;
  std::mutex streamMutex;

  size_t audioFormat = 0;
  size_t sampleRate = 0;
  size_t channels = 0;
  size_t bytesPerSample = 0;
  size_t frameSize = 0;

  size_t totalFrames = 0;
  size_t totalSamples = 0;

  off_t audioDataOffset = 0;
  size_t audioDataSize = 0;

  AudioType audioType = AudioType::PCM16;

  struct {
    std::atomic<int> currentFrameIndex{0};
    std::atomic<bool> isPlaying{false};
    std::atomic<bool> isPaused{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> seekRequested{false};
    std::atomic<int> seekFrame{0};
  } playbackState;

  std::thread decodeThread;
  std::thread playbackThread;

  AudioChunk ringBuffer[MAX_QUEUE_BUFFERS];
  size_t ringBufferHead = 0;
  size_t ringBufferTail = 0;
  size_t ringBufferSize = 0;
  // std::queue<AudioChunk> queue;
  std::mutex queueMutex;
  std::condition_variable queueNotEmpty;
  std::condition_variable queueNotFull;

  static inline void (*decodeFn)(WavCodec *, float *, int, int) = nullptr;

  snd_pcm_t *pcmHandle = nullptr;

  static inline float pcm8_to_float(uint8_t sample) {
    return (sample - 128) / 128.0f;
  }

  static inline float pcm16_to_float(int16_t sample) {
    return sample / 32768.0f;
  }

  static inline float pcm24_to_float(int32_t sample) {
    return sample / 8388608.0f;
  }

  static inline float pcm32_to_float(int32_t sample) {
    return sample / 2147483648.0f;
  }

  static constexpr uint8_t KSDATAFORMAT_SUBTYPE_PCM[16] = {
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  static constexpr uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = {
      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  static inline bool isPCMGuid(const uint8_t *guid) {

    return memcmp(guid, KSDATAFORMAT_SUBTYPE_PCM, 16) == 0;
  }

  static inline bool isFloatGuid(const uint8_t *guid) {
    return memcmp(guid, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16) == 0;
  }

  static float decodeALawSample(uint8_t value) {
    value ^= 0x55;

    int sign = value & 0x80;
    int exponent = (value & 0x70) >> 4;
    int mantissa = value & 0x0F;

    int sample;

    if (exponent == 0)
      sample = (mantissa << 4) + 8;
    else
      sample = ((mantissa << 4) + 0x108) << (exponent - 1);

    return sign ? -(sample / 32768.0f) : (sample / 32768.0f);
  }

  static float decodeMuLawSample(uint8_t value) {
    value = ~value;

    int sign = value & 0x80;
    int exponent = (value >> 4) & 0x07;
    int mantissa = value & 0x0F;

    int sample = ((mantissa << 3) + 0x84) << exponent;

    sample -= 0x84;

    return sign ? -(sample / 32768.0f) : (sample / 32768.0f);
  };

  void detectAudioType() {

    switch (audioFormat) {

    case AudioFormat::PCM:

      switch (bytesPerSample) {

      case 1:
        audioType = PCM8;
        break;

      case 2:
        audioType = PCM16;
        break;

      case 3:
        audioType = PCM24;
        break;

      case 4:
        audioType = PCM32;
        break;

      default:
        throw std::runtime_error("Unsupported PCM bit depth");
      }

      break;

    case AudioFormat::IEEE_FLOAT:
      audioType = AudioType::IEEE;
      break;

    case AudioFormat::ALAW:
      audioType = AudioType::ALAW_CODEC;
      break;

    case AudioFormat::MULAW:
      audioType = AudioType::MULAW_CODEC;
      break;

    default:
      throw std::runtime_error("Unsupported WAV format");
    }
  }

  void decodePCM8(float *output, int sampleOffset, int sampleCount) {

    // off_t location = audioDataOffset + sampleOffset;
    //
    // fseek(file, location, SEEK_SET);
    //
    // uint8_t raw[sampleCount];
    //
    // fread(raw, sizeof(uint8_t), sampleCount, file);
    //
    // for (int i = 0; i < sampleCount; ++i)
    //   output[i] = pcm8_to_float(raw[i]);
  }

  void decodePCM16(float *output, int sampleOffset, int sampleCount) {

    off_t location = audioDataOffset + (sampleOffset * sizeof(int16_t));

    alignas(16) thread_local int16_t raw[DECODE_BUFFER_SAMPLES];

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      // file.seekg(location, std::ios::beg);
      fseek(file, location, SEEK_SET);
      // file.read(reinterpret_cast<char *>(raw), sizeof(int16_t) *
      // sampleCount);
      fread(raw, sizeof(int16_t), sampleCount, file);
    }

    for (int i = 0; i < sampleCount; ++i)
      output[i] = pcm16_to_float(raw[i]);
  }

  void decodePCM24(float *output, int sampleOffset, int sampleCount) {

    // long location = audioDataOffset + (sampleOffset * 3);
    //
    // fseek(file, location, SEEK_SET);
    //
    // uint8_t raw[sampleCount * 3];
    //
    // fread(raw, 1, sampleCount * 3, file);
    //
    // for (int i = 0; i < sampleCount; ++i) {
    //
    //   int idx = i * 3;
    //
    //   int32_t sample =
    //       (raw[idx + 0]) | (raw[idx + 1] << 8) | (raw[idx + 2] << 16);
    //
    //   if (sample & 0x800000)
    //     sample |= ~0xFFFFFF;
    //
    //   output[i] = pcm24_to_float(sample);
    // }
  }

  void decodePCM32(float *output, int sampleOffset, int sampleCount) {

    // long location = audioDataOffset + (sampleOffset * sizeof(int32_t));
    //
    // fseek(file, location, SEEK_SET);
    //
    // int32_t raw[sampleCount];
    //
    // fread(raw, sizeof(int32_t), sampleCount, file);
    //
    // for (int i = 0; i < sampleCount; ++i)
    //   output[i] = pcm32_to_float(raw[i]);
  }

  void decodeIEEEFloat(float *output, int sampleOffset, int sampleCount) {

    // long location = audioDataOffset + (sampleOffset * sizeof(float));
    //
    // fseek(file, location, SEEK_SET);
    //
    // fread(output, sizeof(float), sampleCount, file);
  }

  void decodeALaw(float *output, int sampleOffset, int sampleCount) {

    // long location = audioDataOffset + sampleOffset;
    //
    // fseek(file, location, SEEK_SET);
    //
    // uint8_t raw[sampleCount];
    //
    // fread(raw, sizeof(uint8_t), sampleCount, file);
    //
    // for (int i = 0; i < sampleCount; ++i)
    //   output[i] = decodeALawSample(raw[i]);
  }

  void decodeMuLaw(float *output, int sampleOffset, int sampleCount) {

    // long location = audioDataOffset + sampleOffset;
    //
    // fseek(file, location, SEEK_SET);
    //
    // uint8_t raw[sampleCount];
    //
    // fread(raw, sizeof(uint8_t), sampleCount, file);
    //
    // for (int i = 0; i < sampleCount; ++i)
    //   output[i] = decodeMuLawSample(raw[i]);
  }

  void decideDecoderFn() {

    switch (audioType) {

    case AudioType::PCM8:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodePCM8(o, off, c);
      };
      break;

    case AudioType::PCM16:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodePCM16(o, off, c);
      };
      break;

    case AudioType::PCM24:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodePCM24(o, off, c);
      };
      break;

    case AudioType::PCM32:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodePCM32(o, off, c);
      };
      break;

    case AudioType::IEEE:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodeIEEEFloat(o, off, c);
      };
      break;

    case AudioType::ALAW_CODEC:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodeALaw(o, off, c);
      };
      break;

    case AudioType::MULAW_CODEC:
      decodeFn = [](WavCodec *self, float *o, int off, int c) {
        self->decodeMuLaw(o, off, c);
      };
      break;
    }
  }

private:
  void flushQueue() {

    std::lock_guard<std::mutex> lock(queueMutex);

    ringBufferHead = 0;
    ringBufferTail = 0;
    ringBufferSize = 0;
    // while (!queue.empty())
    //   queue.pop();
  }

  void decoderWorker() {
    int localFrame = playbackState.currentFrameIndex.load();
    size_t sampleOffset = localFrame * channels;

    while (!playbackState.stopRequested.load()) {
      // 1. SAFE SYNCHRONIZED SEEK CHECK
      if (playbackState.seekRequested.load()) {
        flushQueue(); // Resets ring indicators under lock
        localFrame = playbackState.seekFrame.load();
        playbackState.currentFrameIndex.store(localFrame);
        sampleOffset = localFrame * channels;
      }

      size_t targetSlot = 0;
      {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueNotFull.wait(lock, [&]() {
          return ringBufferSize < MAX_QUEUE_BUFFERS ||
                 playbackState.stopRequested.load() ||
                 playbackState.seekRequested.load();
        });

        if (playbackState.stopRequested.load())
          break;

        if (playbackState.seekRequested.load())
          continue; // Instantly loops back up to handle the seek offset
                    // position safely

        targetSlot = ringBufferHead;
      }

      if (sampleOffset >= totalSamples)
        break;

      int sampleCount = std::min(static_cast<size_t>(DECODE_BUFFER_SAMPLES),
                                 totalSamples - sampleOffset);

      decodeFn(this, ringBuffer[targetSlot].samples, sampleOffset, sampleCount);
      ringBuffer[targetSlot].frames = sampleCount / channels;

      {
        std::lock_guard<std::mutex> lock(queueMutex);
        ringBufferHead = (ringBufferHead + 1) % MAX_QUEUE_BUFFERS;
        ringBufferSize++;
      }

      queueNotEmpty.notify_one();
      sampleOffset += sampleCount;
    }
  }

  void playbackWorker() {
    while (!playbackState.stopRequested.load()) {
      size_t targetSlot = 0;

      {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueNotEmpty.wait(lock, [&]() {
          return (ringBufferSize > 0 && !playbackState.isPaused.load() &&
                  !playbackState.seekRequested.load()) ||
                 playbackState.stopRequested.load() ||
                 playbackState.seekRequested.load();
        });

        if (playbackState.stopRequested.load())
          break;

        // 2. CRITICAL FIX: Intercept seek requests immediately and purge
        // hardware pipelines
        if (playbackState.seekRequested.load()) {
          snd_pcm_drop(pcmHandle); // Instantly stops audio output on hardware
          snd_pcm_prepare(pcmHandle); // Readies ALSA for fresh PCM streams
          playbackState.seekRequested.store(false); // Clear request token
          queueNotFull.notify_all(); // Wake decoder to fill the empty slot
          continue;
        }

        if (playbackState.isPaused.load()) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }

        targetSlot = ringBufferTail;
      }

      const float *data = ringBuffer[targetSlot].samples;
      int remainingFrames = ringBuffer[targetSlot].frames;

      // 3. Crisp Micro-Period Feeding Loop
      while (remainingFrames > 0 && !playbackState.stopRequested.load() &&
             !playbackState.isPaused.load() &&
             !playbackState.seekRequested.load()) {
        int framesToSubmit = std::min(512, remainingFrames);

        snd_pcm_sframes_t written =
            snd_pcm_writei(pcmHandle, data, framesToSubmit);

        if (written == -EPIPE || written < 0) {
          snd_pcm_prepare(pcmHandle);
          continue;
        }

        remainingFrames -= written;
        data += written * channels;
        playbackState.currentFrameIndex.fetch_add(written);
      }

      // If a seek or pause occurred mid-chunk execution, preserve the current
      // ring slot indices
      if (playbackState.seekRequested.load() || playbackState.isPaused.load()) {
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(queueMutex);
        ringBufferTail = (ringBufferTail + 1) % MAX_QUEUE_BUFFERS;
        ringBufferSize--;
      }
      queueNotFull.notify_one();

      // Auto-shutdown check if file has ended naturally
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (ringBufferSize == 0 &&
            playbackState.currentFrameIndex.load() >= totalFrames) {
          playbackState.stopRequested.store(true);
          break;
        }
      }
    }
    playbackState.isPlaying.store(false);
  }

  void openALSA() {

    int err = snd_pcm_open(&pcmHandle, "default", SND_PCM_STREAM_PLAYBACK, 0);

    if (err < 0)
      throw std::runtime_error(snd_strerror(err));

    err = snd_pcm_set_params(pcmHandle, SND_PCM_FORMAT_FLOAT_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, channels,
                             sampleRate, 1, 50000);

    if (err < 0)
      throw std::runtime_error(snd_strerror(err));
  }

  void closeALSA() {

    if (!pcmHandle)
      return;

    snd_pcm_drain(pcmHandle);

    snd_pcm_close(pcmHandle);

    pcmHandle = nullptr;
  }

public:
  WavCodec(const char *filename) {

    // file.open(filename, std::ios::binary);
    file = fopen(filename, "rb");

    if (!file)
      throw std::runtime_error("Could not open WAV file");

    RIFF_Header riff;

    // file.read(reinterpret_cast<char *>(&riff), sizeof(RIFF_Header));
    fread(&riff, sizeof(RIFF_Header), 1, file);

    if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
      throw std::runtime_error("Invalid WAV file");
    }

    while (true) {

      Chunk_Header chunk;

      // if (!file.read(reinterpret_cast<char *>(&chunk), sizeof(chunk)))
      if (fread(&chunk, sizeof(chunk), 1, file) != 1)
        break;

      // std::streamoff chunkStart = file.tellg();
      size_t chunkDataStart = ftell(file);

      if (memcmp(chunk.id, "fmt ", 4) == 0) {
        FMT_Header fmt;

        // file.read(reinterpret_cast<char *>(&fmt), sizeof(fmt));
        fread(&fmt, sizeof(fmt), 1, file);

        audioFormat = fmt.audio_format;

        channels = fmt.num_channels;

        sampleRate = fmt.sample_rate;

        bytesPerSample = fmt.bits_per_sample / 8;

        frameSize = channels * bytesPerSample;

        if (audioFormat == AudioFormat::EXTENSIBLE) {
          FMT_Extensible ext;

          // file.read(reinterpret_cast<char *>(&ext), sizeof(ext));
          fread(&ext, sizeof(ext), 1, file);

          if (isPCMGuid(ext.subFormat)) {
            audioFormat = AudioFormat::PCM;
          } else if (isFloatGuid(ext.subFormat)) {
            audioFormat = AudioFormat::IEEE_FLOAT;
          }
        }
      }

      else if (memcmp(chunk.id, "data", 4) == 0) {
        // audioDataOffset = file.tellg();
        audioDataOffset = ftell(file);

        audioDataSize = chunk.size;

        totalFrames = chunk.size / frameSize;

        totalSamples = totalFrames * channels;
      }

      // file.seekg(chunkStart + chunk.size + (chunk.size & 1), std::ios::beg);
      fseek(file, chunkDataStart + chunk.size + (chunk.size & 1), SEEK_SET);
    }

    detectAudioType();
    decideDecoderFn();
  }

  ~WavCodec() {

    stop();

    if (file) {

      // file.close();
      fclose(file);
      file = nullptr;
    }
  }

  void seek(int frame) {
    if (frame < 0)
      frame = 0;
    if (frame >= totalFrames)
      frame = totalFrames - 1;

    // 1. Force the frame position index immediately
    playbackState.seekFrame.store(frame);
    playbackState.seekRequested.store(true);

    // 2. If the playback engine had terminated because it hit the end, wake it
    // up now
    if (!playbackState.isPlaying.load()) {
      // Force the playback frame index variable directly so playback() reads
      // from here
      playbackState.currentFrameIndex.store(frame);
      playback();
    } else {
      // 3. Thread pool is alive; broadcast to wake them from conditional sleeps
      std::lock_guard<std::mutex> lock(queueMutex);
      queueNotFull.notify_all();
      queueNotEmpty.notify_all();
    }
  }
  // void seek(int frame) {
  //   if (frame < 0)
  //     frame = 0;
  //   if (frame >= totalFrames)
  //     frame = totalFrames - 1;
  //
  //   // 1. Store the request variables safely
  //   playbackState.seekFrame.store(frame);
  //   playbackState.seekRequested.store(true);
  //
  //   // 2. Wake up any active thread waiting on conditional variables
  //   {
  //     std::lock_guard<std::mutex> lock(queueMutex);
  //     queueNotFull.notify_all();
  //     queueNotEmpty.notify_all();
  //   }
  //
  //   // 3. FIX FOR FINISHED PLAYBACK: If the song hit the end and threads
  //   died,
  //   // we must clean up and re-launch the workers to start streaming again.
  //   if (!playbackState.isPlaying.load()) {
  //     playback();
  //   } else {
  //     // If the decoder thread finished and exited early but playback was
  //     still
  //     // draining, re-spawn the decoder thread so it can fulfill the seek
  //     // request.
  //     std::lock_guard<std::mutex> lock(queueMutex);
  //     if (decodeThread.joinable() && !playbackState.stopRequested.load()) {
  //       // We only join if it's dead/finished. A quick check if it's still
  //       // alive: If it exited naturally, we must re-start it. To do this
  //       safely
  //       // without complex tracking, we let the playbackWorker handle it, or
  //       // just ensure playback() checks handles it. Let's make sure it
  //       boots: if (playbackState.currentFrameIndex.load() >= totalFrames) {
  //         if (decodeThread.joinable())
  //           decodeThread.join();
  //         playbackState.stopRequested.store(false);
  //         decodeThread = std::thread(&WavCodec::decoderWorker, this);
  //       }
  //     }
  //   }
  // }

  void playback() {
    // If already running, do nothing
    if (playbackState.isPlaying.exchange(true)) {
      return;
    }

    playbackState.stopRequested.store(false);
    playbackState.seekRequested.store(false);
    playbackState.isPaused.store(false);

    // Completely clear out old indices before booting up
    flushQueue();

    openALSA();

    // Re-instantiate the background workers
    decodeThread = std::thread(&WavCodec::decoderWorker, this);
    playbackThread = std::thread(&WavCodec::playbackWorker, this);

    // DETACH FOR SMOOTH LIFECYCLE:
    // This allows the threads to naturally die and disappear when the song ends
    // without requiring the main thread to call .join() and freeze user
    // controls.
    decodeThread.detach();
    playbackThread.detach();
  }
  // void playback() {
  //   // If the threads already died naturally from finishing, join them clean
  //   // before spinning up again
  //   if (!playbackState.isPlaying.load()) {
  //     if (decodeThread.joinable())
  //       decodeThread.join();
  //     if (playbackThread.joinable())
  //       playbackThread.join();
  //   }
  //
  //   if (playbackState.isPlaying.exchange(true)) {
  //     return;
  //   }
  //
  //   playbackState.stopRequested.store(false);
  //   playbackState.seekRequested.store(false);
  //   playbackState.isPaused.store(false);
  //
  //   // Reset ring indices completely on fresh boot
  //   flushQueue();
  //
  //   openALSA();
  //
  //   decodeThread = std::thread(&WavCodec::decoderWorker, this);
  //   playbackThread = std::thread(&WavCodec::playbackWorker, this);
  // }

  void stop() {
    if (!playbackState.isPlaying.load())
      return;

    playbackState.stopRequested.store(true);
    playbackState.isPaused.store(false);

    // Broadcast wakeups to break any active condition variable locks
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      queueNotEmpty.notify_all();
      queueNotFull.notify_all();
    }

    // Fast, non-blocking spin wait for the detached loops to finish dropping
    // data
    while (playbackState.isPlaying.load()) {
      std::this_thread::yield();
    }

    closeALSA();
  }

  void togglePause() {
    bool paused = !playbackState.isPaused.load();
    playbackState.isPaused.store(paused);

    // Wake up both threads so they instantly realize the pause state changed
    queueNotEmpty.notify_all();
    queueNotFull.notify_all();
  }

  bool isPaused() const { return playbackState.isPaused.load(); }

  int getSampleRate() const { return sampleRate; }

  int getNumChannels() const { return channels; }

  int getBytesPerSample() const { return bytesPerSample; }

  int getAudioFormat() const { return audioFormat; }

  int getTotalFrames() const { return totalFrames; }

  int getCurrentFrame() const { return playbackState.currentFrameIndex.load(); }

  float getPlaybackProgress() const {

    if (totalFrames == 0)
      return 0.0f;

    return static_cast<float>(playbackState.currentFrameIndex.load()) /
           static_cast<float>(totalFrames);
  }
};
