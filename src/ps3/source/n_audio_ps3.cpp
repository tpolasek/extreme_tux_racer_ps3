/* ETR PS3 audio backend (replaces n_audio.cpp) — stubbed for the MVP.
 *
 * Audio playback is out of scope for this milestone, but the game's music
 * loading path (audio.cpp::CMusic::LoadMusicList) is NOT optional: it indexes
 * the racing_themes.lst entries into musics[MusicIndex[item]], which would
 * throw / crash if the musics vector were empty. To keep the boot safe while
 * emitting no sound, MusicStream::open() reports success (a "valid" but
 * silent dummy stream) and all AudioDevice playback methods are no-ops.
 *
 * SoundData::loadWav() returns false so failed SFX loads are simply skipped
 * (LoadSoundList tolerates per-file failure).
 */
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_audio.h"

#include <cstring>

/* =====================================================================
 * SoundData
 * ===================================================================== */
SoundData::SoundData() : frames_(0), rate_(44100) {}
SoundData::~SoundData() { release(); }

bool SoundData::loadWav(const std::string& /*filename*/) {
	/* No SFX on PS3 for the MVP. Returning false lets CSound::LoadChunk log
	 * the skip and continue without inserting an entry. */
	return false;
}

void SoundData::release() {
	samples_.clear();
	frames_ = 0;
}

/* =====================================================================
 * MusicStream — silent but "openable"
 * ===================================================================== */
struct MusicStream::Impl {
	bool        open;
	int         rate;
	std::size_t frames;
	Impl() : open(false), rate(44100), frames(0) {}
};

MusicStream::MusicStream() : impl_(new Impl) {}
MusicStream::~MusicStream() = default;

bool MusicStream::open(const std::string& /*filename*/) {
	/* Pretend the stream opened successfully. This keeps MusicIndex fully
	 * populated for every name in music.lst so the racing-themes indexing
	 * (musics[MusicIndex[item]]) can never go out of bounds. read() emits
	 * silence (returns 0 frames), so nothing is heard. */
	impl_->open  = true;
	impl_->rate  = 44100;
	impl_->frames = 0;
	return true;
}

void MusicStream::close() { impl_->open = false; }
bool MusicStream::isOpen() const noexcept { return impl_->open; }
int  MusicStream::sampleRate() const noexcept { return impl_->rate; }
std::size_t MusicStream::frames() const noexcept { return impl_->frames; }

std::size_t MusicStream::read(Int16* /*dst*/, std::size_t frames, bool /*loop*/) {
	/* Silent: produce nothing. The audio thread (absent on PS3) never pulls,
	 * and callers treat 0 as "no data". */
	(void)frames;
	return 0;
}

void MusicStream::rewind() {}

/* =====================================================================
 * AudioDevice — fully inert
 * ===================================================================== */
struct AudioDevice::Impl {
	bool init;
	int  rate;
	Impl() : init(false), rate(44100) {}
};

AudioDevice::AudioDevice() : impl_(new Impl) {}
AudioDevice::~AudioDevice() = default;

bool AudioDevice::init(int sampleRate) {
	impl_->rate = sampleRate;
	impl_->init = true;
	return true;   /* report success so audioDevice() stays quiet */
}
void AudioDevice::shutdown() { impl_->init = false; }
bool AudioDevice::isInitialized() const noexcept { return impl_->init; }

int  AudioDevice::play(const SoundData&, int, bool) { return -1; }
void AudioDevice::stop(int)             { }
void AudioDevice::stopAll()             { }
void AudioDevice::setVolume(int, int)   { }
bool AudioDevice::isPlaying(int) const  { return false; }

void AudioDevice::playMusic(MusicStream*, int, bool) { }
void AudioDevice::stopMusic()                          { }
void AudioDevice::setMusicVolume(int)                  { }
bool AudioDevice::isMusicPlaying() const               { return false; }
