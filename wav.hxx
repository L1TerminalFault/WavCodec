#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

constexpr static inline int DECODE_BUFFER_SAMPLES = 16384;
constexpr static inline int MAX_QUEUE_BUFFERS = 8;

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
  std::vector<float> samples;
  int frames = 0;
};

class WavCodec {
private:
  FILE *file = nullptr;

  int audioFormat = 0;
  int sampleRate = 0;
  int channels = 0;
  int bytesPerSample = 0;
  int frameSize = 0;

  int totalFrames = 0;
  int totalSamples = 0;

  int audioDataOffset = 0;
  int audioDataSize = 0;

  AudioType audioType = AudioType::PCM16;

  struct {
    std::atomic<int> currentFrameIndex{0};
    std::atomic<bool> isPlaying{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> seekRequested{false};
    std::atomic<int> seekFrame{0};
  } playbackState;

  std::thread decodeThread;
  std::thread playbackThread;

  std::queue<AudioChunk> queue;
  std::mutex queueMutex;
  std::condition_variable queueNotEmpty;
  std::condition_variable queueNotFull;

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

    long location = audioDataOffset + sampleOffset;

    fseek(file, location, SEEK_SET);

    uint8_t raw[sampleCount];

    fread(raw, sizeof(uint8_t), sampleCount, file);

    for (int i = 0; i < sampleCount; ++i)
      output[i] = pcm8_to_float(raw[i]);
  }

  void decodePCM16(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + (sampleOffset * sizeof(int16_t));

    fseek(file, location, SEEK_SET);

    int16_t raw[sampleCount];

    fread(raw, sizeof(int16_t), sampleCount, file);

    for (int i = 0; i < sampleCount; ++i)
      output[i] = pcm16_to_float(raw[i]);
  }

  void decodePCM24(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + (sampleOffset * 3);

    fseek(file, location, SEEK_SET);

    uint8_t raw[sampleCount * 3];

    fread(raw, 1, sampleCount * 3, file);

    for (int i = 0; i < sampleCount; ++i) {

      int idx = i * 3;

      int32_t sample =
          (raw[idx + 0]) | (raw[idx + 1] << 8) | (raw[idx + 2] << 16);

      if (sample & 0x800000)
        sample |= ~0xFFFFFF;

      output[i] = pcm24_to_float(sample);
    }
  }

  void decodePCM32(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + (sampleOffset * sizeof(int32_t));

    fseek(file, location, SEEK_SET);

    int32_t raw[sampleCount];

    fread(raw, sizeof(int32_t), sampleCount, file);

    for (int i = 0; i < sampleCount; ++i)
      output[i] = pcm32_to_float(raw[i]);
  }

  void decodeIEEEFloat(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + (sampleOffset * sizeof(float));

    fseek(file, location, SEEK_SET);

    fread(output, sizeof(float), sampleCount, file);
  }

  void decodeALaw(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + sampleOffset;

    fseek(file, location, SEEK_SET);

    uint8_t raw[sampleCount];

    fread(raw, sizeof(uint8_t), sampleCount, file);

    for (int i = 0; i < sampleCount; ++i)
      output[i] = decodeALawSample(raw[i]);
  }

  void decodeMuLaw(float *output, int sampleOffset, int sampleCount) {

    long location = audioDataOffset + sampleOffset;

    fseek(file, location, SEEK_SET);

    uint8_t raw[sampleCount];

    fread(raw, sizeof(uint8_t), sampleCount, file);

    for (int i = 0; i < sampleCount; ++i)
      output[i] = decodeMuLawSample(raw[i]);
  }

  void decodeSamples(float *output, int sampleOffset, int sampleCount) {

    switch (audioType) {

    case AudioType::PCM8:
      decodePCM8(output, sampleOffset, sampleCount);
      break;

    case AudioType::PCM16:
      decodePCM16(output, sampleOffset, sampleCount);
      break;

    case AudioType::PCM24:
      decodePCM24(output, sampleOffset, sampleCount);
      break;

    case AudioType::PCM32:
      decodePCM32(output, sampleOffset, sampleCount);
      break;

    case AudioType::IEEE:
      decodeIEEEFloat(output, sampleOffset, sampleCount);
      break;

    case AudioType::ALAW_CODEC:
      decodeALaw(output, sampleOffset, sampleCount);
      break;

    case AudioType::MULAW_CODEC:
      decodeMuLaw(output, sampleOffset, sampleCount);
      break;
    }
  }

private:
  void flushQueue() {

    std::lock_guard<std::mutex> lock(queueMutex);

    while (!queue.empty())
      queue.pop();
  }

  void decoderWorker() {

    int localFrame = playbackState.currentFrameIndex.load();

    int sampleOffset = localFrame * channels;

    while (!playbackState.stopRequested.load()) {

      if (playbackState.seekRequested.exchange(false)) {
        flushQueue();

        localFrame = playbackState.seekFrame.load();

        playbackState.currentFrameIndex.store(localFrame);

        sampleOffset = localFrame * channels;
      }

      {
        std::unique_lock<std::mutex> lock(queueMutex);

        queueNotFull.wait(lock, [&]() {
          return queue.size() < MAX_QUEUE_BUFFERS ||
                 playbackState.stopRequested.load();
        });

        if (playbackState.stopRequested.load())
          break;
      }

      if (sampleOffset >= totalSamples)
        break;

      int sampleCount =
          std::min(DECODE_BUFFER_SAMPLES, totalSamples - sampleOffset);

      AudioChunk chunk;

      chunk.samples.resize(sampleCount);

      decodeSamples(chunk.samples.data(), sampleOffset, sampleCount);

      chunk.frames = sampleCount / channels;

      {
        std::lock_guard<std::mutex> lock(queueMutex);

        queue.push(std::move(chunk));
      }

      queueNotEmpty.notify_one();

      sampleOffset += sampleCount;
    }

    queueNotEmpty.notify_all();
  }

  void playbackWorker() {

    while (!playbackState.stopRequested.load()) {

      AudioChunk chunk;

      {
        std::unique_lock<std::mutex> lock(queueMutex);

        queueNotEmpty.wait(lock, [&]() {
          return !queue.empty() || playbackState.stopRequested.load();
        });

        if (playbackState.stopRequested.load())
          break;

        chunk = std::move(queue.front());

        queue.pop();
      }

      queueNotFull.notify_one();

      if (playbackState.seekRequested.load()) {
        snd_pcm_drop(pcmHandle);

        snd_pcm_prepare(pcmHandle);

        continue;
      }

      const float *data = chunk.samples.data();

      int remainingFrames = chunk.frames;

      while (remainingFrames > 0 && !playbackState.stopRequested.load()) {

        snd_pcm_sframes_t written =
            snd_pcm_writei(pcmHandle, data, remainingFrames);

        if (written == -EPIPE) {
          snd_pcm_prepare(pcmHandle);

          continue;
        }

        if (written < 0) {
          snd_pcm_prepare(pcmHandle);

          continue;
        }

        remainingFrames -= written;

        data += written * channels;

        playbackState.currentFrameIndex.fetch_add(written);
      }
    }
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

  void seek(int frame) {

    if (frame < 0)
      frame = 0;

    if (frame >= totalFrames)
      frame = totalFrames - 1;

    playbackState.seekFrame.store(frame);

    playbackState.seekRequested.store(true);
  }

  void stop() {

    playbackState.stopRequested.store(true);

    queueNotEmpty.notify_all();
    queueNotFull.notify_all();

    if (decodeThread.joinable())
      decodeThread.join();

    if (playbackThread.joinable())
      playbackThread.join();

    closeALSA();

    playbackState.isPlaying.store(false);
  }

public:
  WavCodec(const char *filename) {

    file = fopen(filename, "rb");

    if (!file)
      throw std::runtime_error("Could not open WAV file");

    RIFF_Header riff;

    fread(&riff, sizeof(RIFF_Header), 1, file);

    if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
      throw std::runtime_error("Invalid WAV file");
    }

    while (true) {

      Chunk_Header chunk;

      if (fread(&chunk, sizeof(chunk), 1, file) != 1)
        break;

      long chunkStart = ftell(file);

      if (memcmp(chunk.id, "fmt ", 4) == 0) {
        FMT_Header fmt;

        fread(&fmt, sizeof(fmt), 1, file);

        audioFormat = fmt.audio_format;

        channels = fmt.num_channels;

        sampleRate = fmt.sample_rate;

        bytesPerSample = fmt.bits_per_sample / 8;

        frameSize = channels * bytesPerSample;

        if (audioFormat == AudioFormat::EXTENSIBLE) {
          FMT_Extensible ext;

          fread(&ext, sizeof(ext), 1, file);

          if (isPCMGuid(ext.subFormat)) {
            audioFormat = AudioFormat::PCM;
          } else if (isFloatGuid(ext.subFormat)) {
            audioFormat = AudioFormat::IEEE_FLOAT;
          }
        }
      }

      else if (memcmp(chunk.id, "data", 4) == 0) {
        audioDataOffset = ftell(file);

        audioDataSize = chunk.size;

        totalFrames = chunk.size / frameSize;

        totalSamples = totalFrames * channels;
      }

      fseek(file, chunkStart + chunk.size + (chunk.size & 1), SEEK_SET);
    }

    detectAudioType();
  }

  ~WavCodec() {

    stop();

    if (file) {

      fclose(file);

      file = nullptr;
    }
  }

  void playback() {

    if (playbackState.isPlaying.exchange(true)) {
      return;
    }

    playbackState.stopRequested.store(false);

    playbackState.seekRequested.store(false);

    openALSA();

    decodeThread = std::thread(&WavCodec::decoderWorker, this);

    playbackThread = std::thread(&WavCodec::playbackWorker, this);

    decodeThread.join();

    {
      std::unique_lock<std::mutex> lock(queueMutex);

      queueNotEmpty.wait(lock, [&]() { return queue.empty(); });
    }

    playbackState.stopRequested.store(true);

    queueNotEmpty.notify_all();
    queueNotFull.notify_all();

    if (playbackThread.joinable()) {
      playbackThread.join();
    }

    closeALSA();

    playbackState.isPlaying.store(false);
  }

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
