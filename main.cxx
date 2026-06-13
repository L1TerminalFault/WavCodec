#include "wav.hxx"
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <termios.h>
#include <unistd.h>

// Helper to put the terminal into non-blocking, unbuffered character input mode
void setKeyboardRawMode(bool enable) {
  static struct termios oldt, newt;
  if (enable) {
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Disable buffering and character echoing
    newt.c_cc[VMIN] = 0;              // Non-blocking read threshold
    newt.c_cc[VTIME] = 0;             // Non-blocking timeout window
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  } else {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore normal terminal state
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <filename.wav>\n", argv[0]);
    return 1;
  }

  try {
    WavCodec wav(argv[1]);
    printf("--- WAV Player Control Panel ---\n");
    printf("File Name:      %s\n", argv[1]);
    printf("Sample Rate:    %d Hz | Channels: %d\n", wav.getSampleRate(),
           wav.getNumChannels());
    printf("Total Frames:   %d\n", wav.getTotalFrames());
    printf("--------------------------------\n");
    printf("Controls: [->] Seek Forward | [<-] Seek Backward\n");
    printf("          [R]  Instant Restart From Beginning\n");
    printf("          [Space] Toggle Stop/Play | [Q] Exit Player\n");
    printf("--------------------------------\n\n");

    // 1. Kick off playback asynchronously in the background
    wav.playback();
    setKeyboardRawMode(true);

    bool running = true;
    char buf; // Single byte buffer is perfect for Vim keys

    while (running) {
      ssize_t n = read(STDIN_FILENO, &buf, sizeof(buf));

      if (n == 1) {
        char key = buf;

        if (key == 'q' || key == 'Q') {
          running = false;
        }
        // [K] - Vim Play / Pause Toggle
        else if (key == 'k' || key == 'K') {
          wav.togglePause();
        }
        // [L] - Vim Seek Forward (5 seconds)
        else if (key == 'l' || key == 'L') {
          int target = wav.getCurrentFrame() + (wav.getSampleRate() * 5);
          wav.seek(target);
        }
        // [H] - Vim Seek Backward (5 seconds)
        else if (key == 'h' || key == 'H') {
          int target = wav.getCurrentFrame() - (wav.getSampleRate() * 5);
          wav.seek(target);
        }
        // [R] - Instant Restart from Beginning
        else if (key == 'r' || key == 'R') {
          wav.seek(0);
          if (wav.isPaused())
            wav.togglePause();
          wav.playback();
        }
      }

      // Real-time status progress bar refresh
      printf("\rProgress: [%d / %d] (%.1f%%)   ", wav.getCurrentFrame(),
             wav.getTotalFrames(), wav.getPlaybackProgress() * 100.0f);
      fflush(stdout);

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean shutdown
    setKeyboardRawMode(false);
    printf("\nShutting down audio pipeline cleanly...\n");
    wav.stop();

  } catch (const std::exception &e) {
    setKeyboardRawMode(false);
    fprintf(stderr, "\nFatal Runtime Error: %s\n", e.what());
    return 1;
  }

  return 0;
}
