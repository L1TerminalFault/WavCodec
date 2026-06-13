#include "wav.hxx"
#include <exception>

int main(int argc, char *argv[]) {
  try {
    WavCodec wav(argv[1]);
    printf("Audio Format: %d\n", wav.getAudioFormat());
    printf("Sample Rate: %d\n", wav.getSampleRate());
    printf("Channels: %d\n", wav.getNumChannels());
    printf("BytesPerSample: %d\n", wav.getBytesPerSample());
    wav.playback();
  } catch (const std::exception &e) {
    fprintf(stderr, "Error: %s\n", e.what());
    return 1;
  }

  return 0;
}
