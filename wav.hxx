#pragma once

#include <algorithm>
#include <alsa/asoundlib.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
// #include <fstream>
// #include <ios>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>

constexpr static inline size_t DECODE_BUFFER_SAMPLES = 16384;
constexpr static inline size_t MAX_QUEUE_BUFFERS = 8;

enum AudioFormat : uint16_t {
  PCM = 1,
  IEEE_FLOAT = 3,
  ALAW = 6,
  MULAW = 7,
  EXTENSIBLE = 0xFFFE
};

enum AudioType : uint8_t {
  PCM8 = 0,
  PCM16,
  PCM24,
  PCM32,
  IEEE,
  ALAW_CODEC,
  MULAW_CODEC
};

struct RIFF_Header {
  std::array<char, 4> riff;
  uint32_t file_size;
  std::array<char, 4> wave;
};

struct Chunk_Header {
  std::array<char, 4> id;
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
  std::array<uint8_t, 16> subFormat;
};

struct AudioChunk {
  std::array<float, DECODE_BUFFER_SAMPLES> samples;
  size_t frames;
  size_t sampleCount;
};

class WavCodec {
private:
  FILE *file;
  // std::ifstream file;
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

  [[maybe_unused]] std::array<uint8_t, 7> _padding = {
      0}; // Cache line padding to prevent false sharing

  std::thread decodeThread;
  std::thread playbackThread;

  std::array<AudioChunk, MAX_QUEUE_BUFFERS> ringBuffer = {};
  size_t ringBufferHead = 0;
  size_t ringBufferTail = 0;
  size_t ringBufferSize = 0;
  // std::queue<AudioChunk> queue;
  std::mutex queueMutex;
  std::condition_variable queueNotEmpty;
  std::condition_variable queueNotFull;

  struct {
    std::atomic<size_t> currentFrameIndex{0};
    std::atomic<size_t> seekFrame{0};
    std::atomic<bool> isPlaying{false};
    std::atomic<bool> isPaused{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> seekRequested{false};
    uint32_t _padding = 0; // Cache line padding to prevent false sharing
  } playbackState;

  static inline void (*decodeFn)(WavCodec *, float *, off_t, size_t) = nullptr;

  snd_pcm_t *pcmHandle = nullptr;

  static auto pcm8_to_float(uint8_t sample) {
    return (static_cast<float>(sample) - 128.0F) / 128.0F;
  }

  static auto pcm16_to_float(int16_t sample) {
    return static_cast<float>(sample) / 32768.0F;
  }

  static auto pcm24_to_float(int32_t sample) -> float {
    return static_cast<float>(sample) / 8388608.0F;
  }

  static auto pcm32_to_float(int32_t sample) {
    return static_cast<float>(sample) / 2147483648.0F;
  }

  static constexpr std::array<uint8_t, 16> KSDATAFORMAT_SUBTYPE_PCM = {
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  static constexpr std::array<uint8_t, 16> KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  static auto isPCMGuid(const uint8_t *guid) {

    return std::equal(guid, std::next(guid, 16),
                      KSDATAFORMAT_SUBTYPE_PCM.data());
  }

  static auto isFloatGuid(const uint8_t *guid) {
    return std::equal(guid, std::next(guid, 16),
                      KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.data());
  }

  static auto decodeALawSample(uint8_t value) {
    value ^= 0x55;

    int sign = value & 0x80;
    int exponent = (value & 0x70) >> 4;
    int mantissa = value & 0x0F;

    int sample = 0;

    if (exponent == 0) {
      sample = (mantissa << 4) + 8;
    } else {
      sample = ((mantissa << 4) + 0x108) << (exponent - 1);
    }

    return static_cast<bool>(sign) ? -(static_cast<float>(sample) / 32768.0F)
                                   : (static_cast<float>(sample) / 32768.0F);
  }

  static auto decodeMuLawSample(uint8_t value) {
    value = static_cast<uint8_t>(~value);

    int sign = value & 0x80;
    int exponent = (value >> 4) & 0x07;
    int mantissa = value & 0x0F;

    int sample = ((mantissa << 3) + 0x84) << exponent;

    sample -= 0x84;

    return static_cast<bool>(sign) ? -(static_cast<float>(sample) / 32768.0F)
                                   : (static_cast<float>(sample) / 32768.0F);
  }

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

  void decodePCM8(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location = audioDataOffset + sampleOffset;

    thread_local static std::array<uint8_t, DECODE_BUFFER_SAMPLES> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      (void)fread(raw.data(), sizeof(uint8_t), sampleCount, file);
    }

    for (uint i = 0; i < sampleCount; ++i)
      output[i] = pcm8_to_float(raw.at(i));
  }

  void decodePCM16(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location =
        audioDataOffset + (sampleOffset * static_cast<off_t>(sizeof(int16_t)));

    thread_local static std::array<int16_t, DECODE_BUFFER_SAMPLES> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      // file.seekg(location, std::ios::beg);
      (void)fseek(file, location, SEEK_SET);
      // file.read(reinterpret_cast<char *>(raw), sizeof(int16_t) *
      // sampleCount);
      fread(raw.data(), sizeof(int16_t), sampleCount, file);
    }

    for (uint i = 0; i < sampleCount; ++i)
      output[i] = pcm16_to_float(raw.at(i));
  }

  void decodePCM24(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location = audioDataOffset + (sampleOffset * 3);

    thread_local static std::array<uint8_t, DECODE_BUFFER_SAMPLES * 3> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      fread(raw.data(), 1, sampleCount * 3, file);
    }

    for (uint i = 0; i < sampleCount; ++i) {

      uint idx = i * 3;

      int32_t sample =
          (raw.at(idx + 0)) | (raw.at(idx + 1) << 8) | (raw.at(idx + 2) << 16);

      if (static_cast<bool>(sample & 0x800000))
        sample |= ~0xFFFFFF;

      output[i] = pcm24_to_float(sample);
    }
  }

  void decodePCM32(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location =
        audioDataOffset + (sampleOffset * static_cast<off_t>(sizeof(int32_t)));

    thread_local static std::array<int32_t, DECODE_BUFFER_SAMPLES> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      fread(raw.data(), sizeof(int32_t), sampleCount, file);
    }

    for (uint i = 0; i < sampleCount; ++i)
      output[i] = pcm32_to_float(raw.at(i));
  }

  void decodeIEEEFloat(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location =
        audioDataOffset + (sampleOffset * static_cast<off_t>(sizeof(float)));

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      fread(output, sizeof(float), sampleCount, file);
    }
  }

  void decodeALaw(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location = audioDataOffset + sampleOffset;

    thread_local static std::array<uint8_t, DECODE_BUFFER_SAMPLES> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      (void)fread(raw.data(), sizeof(uint8_t), sampleCount, file);
    }

    for (uint i = 0; i < sampleCount; ++i)
      output[i] = decodeALawSample(raw.at(i));
  }

  void decodeMuLaw(float *output, off_t sampleOffset, size_t sampleCount) {

    off_t location = audioDataOffset + sampleOffset;

    thread_local static std::array<uint8_t, DECODE_BUFFER_SAMPLES> raw;

    {
      std::lock_guard<std::mutex> lock(streamMutex);
      (void)fseek(file, location, SEEK_SET);

      fread(raw.data(), sizeof(uint8_t), sampleCount, file);
    }

    for (uint i = 0; i < sampleCount; ++i)
      output[i] = decodeMuLawSample(raw.at(i));
  }

  void decideDecoderFn() {

    switch (audioType) {

    case AudioType::PCM8:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodePCM8(data, off, count);
      };
      break;

    case AudioType::PCM16:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodePCM16(data, off, count);
      };
      break;

    case AudioType::PCM24:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodePCM24(data, off, count);
      };
      break;

    case AudioType::PCM32:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodePCM32(data, off, count);
      };
      break;

    case AudioType::IEEE:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodeIEEEFloat(data, off, count);
      };
      break;

    case AudioType::ALAW_CODEC:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodeALaw(data, off, count);
      };
      break;

    case AudioType::MULAW_CODEC:
      decodeFn = [](WavCodec *self, float *data, off_t off, size_t count) {
        self->decodeMuLaw(data, off, count);
      };
      break;
    default:
      throw std::runtime_error("Unsupported audio type");
    }
  }

  void flushQueue() {

    std::lock_guard<std::mutex> lock(queueMutex);

    ringBufferHead = 0;
    ringBufferTail = 0;
    ringBufferSize = 0;
  }

  void decoderWorker() {
    ulong localFrame = playbackState.currentFrameIndex.load();
    auto sampleOffset = static_cast<off_t>(localFrame * channels);

    while (!playbackState.stopRequested.load()) {
      // 1. SAFE SYNCHRONIZED SEEK CHECK
      if (playbackState.seekRequested.load()) {
        flushQueue(); // Resets ring indicators under lock
        localFrame = playbackState.seekFrame.load();
        playbackState.currentFrameIndex.store(localFrame);
        sampleOffset = static_cast<off_t>(localFrame * channels);
      }

      size_t targetSlot = 0;
      {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueNotFull.wait(lock, [&]() {
          return ringBufferSize < MAX_QUEUE_BUFFERS ||
                 playbackState.stopRequested.load() ||
                 playbackState.seekRequested.load();
        });

        if (playbackState.stopRequested.load()) {
          break;
        }

        if (playbackState.seekRequested.load()) {
          continue; // Instantly loops back up to handle the seek offset
                    // position safely
        }

        targetSlot = ringBufferHead;
      }

      if (static_cast<size_t>(sampleOffset) >= totalSamples) {
        break;
      }

      ulong sampleCount =
          std::min(static_cast<size_t>(DECODE_BUFFER_SAMPLES),
                   totalSamples - static_cast<size_t>(sampleOffset));

      decodeFn(this, ringBuffer.at(targetSlot).samples.data(), sampleOffset,
               sampleCount);
      ringBuffer.at(targetSlot).frames = sampleCount / channels;

      {
        std::lock_guard<std::mutex> lock(queueMutex);
        ringBufferHead = (ringBufferHead + 1) % MAX_QUEUE_BUFFERS;
        ringBufferSize++;
      }

      queueNotEmpty.notify_one();
      sampleOffset += static_cast<off_t>(sampleCount);
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

        if (playbackState.seekRequested.load()) {
          snd_pcm_drop(pcmHandle);
          snd_pcm_prepare(pcmHandle);
          playbackState.seekRequested.store(false);
          queueNotFull.notify_all();
          continue;
        }

        if (playbackState.isPaused.load()) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }

        targetSlot = ringBufferTail;
      }

      // CRITICAL: Extract tracking data locally
      const float *data = ringBuffer.at(targetSlot).samples.data();
      size_t remainingFrames = ringBuffer.at(targetSlot).frames;

      // WORKER UNLOCK PIPELINE: We stream frames without holding the queue
      // lock! This prevents the decoder thread from locking up while ALSA is
      // rendering.
      while (remainingFrames > 0 && !playbackState.stopRequested.load() &&
             !playbackState.isPaused.load() &&
             !playbackState.seekRequested.load()) {
        size_t framesToSubmit =
            std::min(static_cast<size_t>(512), remainingFrames);

        snd_pcm_sframes_t written =
            snd_pcm_writei(pcmHandle, data, framesToSubmit);

        if (written == -EPIPE) {
          snd_pcm_prepare(pcmHandle);
          continue;
        }
        if (written < 0) {
          snd_pcm_prepare(pcmHandle);
          continue;
        }

        remainingFrames -= static_cast<ulong>(written);
        data += static_cast<size_t>(written) * channels;
        playbackState.currentFrameIndex.fetch_add(static_cast<size_t>(written));
      }

      // If a seek or pause broke the loop early, skip clearing this slot
      if (playbackState.seekRequested.load() || playbackState.isPaused.load()) {
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(queueMutex);
        ringBufferTail = (ringBufferTail + 1) % MAX_QUEUE_BUFFERS;
        ringBufferSize--;
      }
      queueNotFull.notify_one();

      // Auto-shutdown checkpoint
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

    err = snd_pcm_set_params(
        pcmHandle, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
        static_cast<uint>(channels), static_cast<uint>(sampleRate), 1, 50000);

    if (err < 0)
      throw std::runtime_error(snd_strerror(err));
  }

  void closeALSA() {

    if (!static_cast<bool>(pcmHandle))
      return;

    snd_pcm_drain(pcmHandle);

    snd_pcm_close(pcmHandle);

    pcmHandle = nullptr;
  }

public:
  WavCodec(const char *filename) {

    // file.open(filename, std::ios::binary);
    file = fopen(filename, "rb");

    if (!static_cast<bool>(file))
      throw std::runtime_error("Could not open WAV file");

    RIFF_Header riff = {};

    // file.read(reinterpret_cast<char *>(&riff), sizeof(RIFF_Header));
    (void)fread(&riff, sizeof(RIFF_Header), 1, file);

    if (std::string_view(riff.riff.data(), 4) != "RIFF" ||
        std::string_view(riff.wave.data(), 4) != "WAVE") {
      throw std::runtime_error("Invalid WAV file");
    }

    while (true) {

      Chunk_Header chunk = {};

      // if (!file.read(reinterpret_cast<char *>(&chunk), sizeof(chunk)))
      if (fread(&chunk, sizeof(chunk), 1, file) != 1)
        break;

      // std::streamoff chunkStart = file.tellg();
      long chunkDataStart = ftell(file);

      if (std::string_view(chunk.id.data(), 4) == "fmt ") {
        FMT_Header fmt = {};

        // file.read(reinterpret_cast<char *>(&fmt), sizeof(fmt));
        fread(&fmt, sizeof(fmt), 1, file);

        audioFormat = fmt.audio_format;

        channels = fmt.num_channels;

        sampleRate = fmt.sample_rate;

        bytesPerSample = fmt.bits_per_sample / 8;

        frameSize = channels * bytesPerSample;

        if (audioFormat == AudioFormat::EXTENSIBLE) {
          FMT_Extensible ext = {};

          // file.read(reinterpret_cast<char *>(&ext), sizeof(ext));
          fread(&ext, sizeof(ext), 1, file);

          if (isPCMGuid(ext.subFormat.data())) {
            audioFormat = AudioFormat::PCM;
          } else if (isFloatGuid(ext.subFormat.data())) {
            audioFormat = AudioFormat::IEEE_FLOAT;
          }
        }
      }

      else if (std::string_view(chunk.id.data(), 4) == "data") {
        // audioDataOffset = file.tellg();
        audioDataOffset = ftell(file);

        audioDataSize = chunk.size;

        totalFrames = chunk.size / frameSize;

        totalSamples = totalFrames * channels;
      }

      // file.seekg(chunkStart + chunk.size + (chunk.size & 1), std::ios::beg);
      (void)fseek(
          file,
          static_cast<long>(chunkDataStart + chunk.size + (chunk.size & 1)),
          SEEK_SET);
    }

    detectAudioType();
    decideDecoderFn();
  }

  ~WavCodec() noexcept {

    stop();

    if (static_cast<bool>(file)) {

      // file.close();
      (void)fclose(file);

      file = nullptr;
    }
  }

  // --- FIX THE RULE OF FIVE ENFORCEMENT HERE ---
  // Explicitly disable duplicate copy and move actions to ensure thread safety
  WavCodec(const WavCodec &) = delete;
  WavCodec &operator=(const WavCodec &) = delete;
  WavCodec(WavCodec &&) = delete;
  WavCodec &operator=(WavCodec &&) = delete;

  // FIX 1: Change function argument signature to accept a SIGNED type (int64_t)
  // This allows negative math calculations to be evaluated properly without
  // unsigned underflows.
  void seek(int64_t frame) {

    // FIX 2: Calculate upper bounds safely using explicit type matches
    int64_t maxFrameBound = static_cast<int64_t>(totalFrames) - 1;

    // Protect against empty file edge cases where totalFrames could be 0
    maxFrameBound = std::max(maxFrameBound, static_cast<int64_t>(0));

    // FIX 3: Use std::clamp to securely constrain the signed integer bounds.
    // If frame is negative, it snaps cleanly to 0. If it overflows, it snaps to
    // the end.
    int64_t clampedFrame =
        std::clamp(frame, static_cast<int64_t>(0), maxFrameBound);

    // 1. Convert back to your unsigned size_t format ONLY after bounds are safe
    auto targetFrame = static_cast<size_t>(clampedFrame);

    playbackState.seekFrame.store(targetFrame);
    playbackState.seekRequested.store(true);

    // 2. If the playback engine had terminated because it hit the end, wake it
    // up now
    if (!playbackState.isPlaying.load()) {
      playbackState.currentFrameIndex.store(targetFrame);
      playback();
    } else {
      // 3. Thread pool is alive; broadcast to wake them from conditional sleeps
      std::lock_guard<std::mutex> lock(queueMutex);
      queueNotFull.notify_all();
      queueNotEmpty.notify_all();
    }
  }

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

  void stop() {
    if (!playbackState.isPlaying.load()) {
      return;
    }

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

  [[nodiscard]] auto isPaused() const -> bool {
    return playbackState.isPaused.load();
  }

  [[nodiscard]] auto getSampleRate() const -> size_t { return sampleRate; }

  [[nodiscard]] auto getNumChannels() const { return channels; }

  [[nodiscard]] auto getBytesPerSample() const { return bytesPerSample; }

  [[nodiscard]] auto getAudioFormat() const { return audioFormat; }

  [[nodiscard]] auto getTotalFrames() const { return totalFrames; }

  [[nodiscard]] auto getCurrentFrame() const {
    return playbackState.currentFrameIndex.load();
  }

  [[nodiscard]] auto getPlaybackProgress() const {

    if (totalFrames == 0) {
      return 0.0F;
    }

    return static_cast<float>(playbackState.currentFrameIndex.load()) /
           static_cast<float>(totalFrames);
  }
};
