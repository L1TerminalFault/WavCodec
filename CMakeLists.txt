cmake_minimum_required(VERSION 4.0)
project(WavCodecProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ALSA REQUIRED)

# Add all your implementation files here
add_executable(wavcodec main.cxx wav.hxx)
target_link_libraries(wavcodec PRIVATE ALSA::ALSA)
