/* --------------------------------------------------------------------
EXTREME TUXRACER - native audio system

The Linux and PS3 backends use one dedicated audio thread to drain a
mix buffer to the output device in blocks. Linux targets ALSA; PS3 uses
PSL1GHT libaudio and its block-consumed event queue.

PS3 mapping:
    Linux (n_audio.cpp)            PS3 (n_audio_ps3.cpp)
    --------------------------     ---------------------------------
    snd_pcm_open                   audioInit / audioPortOpen
    std::thread                    sysThreadCreate
    std::mutex                     sysMutexCreate
    snd_pcm_wait                   sysEventQueueReceive
    snd_pcm_writei                 fill next float32 DMA block

The platform implementations live in n_audio.cpp and
ps3/source/n_audio_ps3.cpp respectively.
---------------------------------------------------------------------*/
#ifndef N_AUDIO_H
#define N_AUDIO_H

#include "n_color.h"  // Int16 typedef

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// --------------------------------------------------------------------
// SoundData — a fully-decoded PCM sound effect held in memory.
// WAV loader on Linux; on PS3 the same loader works (uses libc FILE*).
// --------------------------------------------------------------------
class SoundData {
public:
	SoundData();
	~SoundData();

	bool loadWav(const std::string& filename);
	void release();

	const Int16* samples() const noexcept { return samples_.data(); }
	std::size_t  frames() const noexcept { return frames_; }
	int          sampleRate() const noexcept { return rate_; }
	bool         valid() const noexcept { return !samples_.empty(); }

private:
	std::vector<Int16> samples_; // interleaved stereo (2 Int16 per frame)
	std::size_t        frames_;
	int                rate_;
};

// --------------------------------------------------------------------
// MusicStream — streaming OGG decoder.
// Both platforms use libvorbisfile; PS3 links the ps3dev portlib.
// --------------------------------------------------------------------
class MusicStream {
public:
	MusicStream();
	~MusicStream();
	MusicStream(const MusicStream&) = delete;
	MusicStream& operator=(const MusicStream&) = delete;

	bool        open(const std::string& filename);
	void        close();
	bool        isOpen() const noexcept;
	int         sampleRate() const noexcept;
	std::size_t frames() const noexcept; // total frames (0 if unknown)

	// Read up to `frames` stereo frames into `dst` (interleaved int16).
	// If `loop` is true and EOF is hit, wraps to start. Returns the actual
	// number of frames read (will equal `frames` while looping, < frames at
	// final EOF when not looping).
	std::size_t read(Int16* dst, std::size_t frames, bool loop);
	void        rewind();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

class AudioDevice;

// --------------------------------------------------------------------
// AudioDevice — spawns the audio thread, owns the mixer + device
// backend. Game thread calls play()/stop()/setVolume(); audio thread
// drains the mix buffer to ALSA (Linux) or libaudio (PS3).
// --------------------------------------------------------------------
class AudioDevice {
public:
	static constexpr int MAX_VOICES = 32;

	AudioDevice();
	~AudioDevice();
	AudioDevice(const AudioDevice&) = delete;
	AudioDevice& operator=(const AudioDevice&) = delete;

	bool init(int sampleRate = 44100);
	void shutdown();
	bool isInitialized() const noexcept;

	// --- SFX voices (called from game thread) ---
	// Returns voice id (0..MAX_VOICES-1) or -1 if no free slot / not init.
	int  play(const SoundData& sound, int volume, bool loop);
	void stop(int vid);
	void stopAll();
	void setVolume(int vid, int volume); // 0..100
	bool isPlaying(int vid) const;

	// --- Music (called from game thread). One music stream at a time. ---
	void playMusic(MusicStream* stream, int volume, bool loop);
	void stopMusic();
	void setMusicVolume(int volume); // 0..100
	bool isMusicPlaying() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

#endif // N_AUDIO_H
