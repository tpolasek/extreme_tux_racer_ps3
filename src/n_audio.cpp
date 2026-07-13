/* --------------------------------------------------------------------
EXTREME TUXRACER - native audio, Linux backend.

Audio thread pattern: wait → mix block → write.
On Linux, snd_pcm_wait/snd_pcm_writei drain the device. The PS3 backend
uses sysEventQueueReceive and fills PSL1GHT libaudio DMA blocks directly.

Mixer is single-threaded inside the audio thread: iterates voices +
music, sums int16 samples, clips, writes to ALSA period buffer.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_audio.h"

#include <alsa/asoundlib.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ====================================================================
//                          SoundData (WAV)
// ====================================================================
SoundData::SoundData() : frames_(0), rate_(0) {}
SoundData::~SoundData() { release(); }

void SoundData::release() {
	samples_.clear();
	samples_.shrink_to_fit();
	frames_ = 0;
	rate_ = 0;
}

bool SoundData::loadWav(const std::string& filename) {
	release();
	std::FILE* fp = std::fopen(filename.c_str(), "rb");
	if (!fp) return false;

	char riff[5] = {0}, wave[5] = {0};
	std::uint32_t riffLen = 0;
	if (std::fread(riff, 1, 4, fp) != 4 || std::memcmp(riff, "RIFF", 4) != 0) { std::fclose(fp); return false; }
	if (std::fread(&riffLen, 4, 1, fp) != 1) { std::fclose(fp); return false; }
	if (std::fread(wave, 1, 4, fp) != 4 || std::memcmp(wave, "WAVE", 4) != 0) { std::fclose(fp); return false; }

	int  channels = 0, bitsPerSample = 0;
	std::uint32_t sampleRate = 0;
	bool haveFmt = false, haveData = false;
	std::vector<Int16> rawSamples;
	std::size_t rawFrames = 0;
	int rawChannels = 0;

	while (!std::feof(fp)) {
		char chunkId[5] = {0};
		std::uint32_t chunkSize = 0;
		if (std::fread(chunkId, 1, 4, fp) != 4) break;
		if (std::fread(&chunkSize, 4, 1, fp) != 1) break;

		if (std::memcmp(chunkId, "fmt ", 4) == 0) {
			std::uint16_t audioFormat = 0;
			(void)std::fread(&audioFormat, 2, 1, fp);
			(void)std::fread(&channels, 2, 1, fp);
			(void)std::fread(&sampleRate, 4, 1, fp);
			std::uint32_t byteRate = 0; (void)std::fread(&byteRate, 4, 1, fp);
			std::uint16_t blockAlign = 0; (void)std::fread(&blockAlign, 2, 1, fp);
			(void)std::fread(&bitsPerSample, 2, 1, fp);
			if (chunkSize > 16) std::fseek(fp, chunkSize - 16, SEEK_CUR);
			haveFmt = true;
		} else if (std::memcmp(chunkId, "data", 4) == 0) {
			if (!haveFmt) { std::fclose(fp); return false; }
			haveData = true;
			rawChannels = channels;

			std::size_t numSamples = chunkSize / (bitsPerSample / 8);
			if (bitsPerSample == 16) {
				rawSamples.resize(numSamples);
				if (std::fread(rawSamples.data(), 2, numSamples, fp) != numSamples) {
					std::fclose(fp); return false;
				}
				rawFrames = numSamples / channels;
			} else if (bitsPerSample == 8) {
				std::vector<std::uint8_t> u8(numSamples);
				if (std::fread(u8.data(), 1, numSamples, fp) != numSamples) {
					std::fclose(fp); return false;
				}
				rawSamples.resize(numSamples);
				for (std::size_t i = 0; i < numSamples; ++i)
					rawSamples[i] = static_cast<Int16>((static_cast<int>(u8[i]) - 128) << 8);
				rawFrames = numSamples / channels;
			} else {
				std::fclose(fp); return false;
			}
		} else {
			// skip unknown chunk
			std::fseek(fp, chunkSize, SEEK_CUR);
		}
		// chunks are word-aligned
		if (chunkSize & 1) std::fseek(fp, 1, SEEK_CUR);
	}
	std::fclose(fp);

	if (!haveFmt || !haveData || rawSamples.empty()) return false;
	if (channels != 1 && channels != 2) return false;

	// Normalize to interleaved stereo int16.
	if (channels == 2) {
		samples_.swap(rawSamples);
	} else {
		samples_.resize(rawFrames * 2);
		for (std::size_t i = 0; i < rawFrames; ++i) {
			samples_[i * 2 + 0] = rawSamples[i];
			samples_[i * 2 + 1] = rawSamples[i];
		}
	}
	frames_ = rawFrames;
	rate_ = static_cast<int>(sampleRate);
	return true;
}

// ====================================================================
//                          MusicStream (OGG)
// ====================================================================
struct MusicStream::Impl {
	OggVorbis_File vf;
	bool           open_;
	int            rate_;
	std::size_t    frames_; // 0 if unknown

	Impl() : open_(false), rate_(0), frames_(0) {
		std::memset(&vf, 0, sizeof(vf));
	}
};

MusicStream::MusicStream() : impl_(new Impl) {}
MusicStream::~MusicStream() { close(); }

bool MusicStream::open(const std::string& filename) {
	close();
	std::FILE* fp = std::fopen(filename.c_str(), "rb");
	if (!fp) return false;
	if (ov_open(fp, &impl_->vf, nullptr, 0) != 0) {
		std::fclose(fp);
		return false;
	}
	vorbis_info* vi = ov_info(&impl_->vf, -1);
	if (!vi) { ov_clear(&impl_->vf); return false; }
	impl_->rate_   = vi->rate;
	impl_->frames_ = static_cast<std::size_t>(ov_pcm_total(&impl_->vf, -1));
	impl_->open_   = true;
	return true;
}

void MusicStream::close() {
	if (impl_->open_) {
		ov_clear(&impl_->vf); // also closes the FILE*
		impl_->open_ = false;
	}
	std::memset(&impl_->vf, 0, sizeof(impl_->vf));
	impl_->rate_ = 0;
	impl_->frames_ = 0;
}

bool        MusicStream::isOpen()       const noexcept { return impl_->open_; }
int         MusicStream::sampleRate()   const noexcept { return impl_->rate_; }
std::size_t MusicStream::frames()       const noexcept { return impl_->frames_; }

void MusicStream::rewind() {
	if (!impl_->open_) return;
	ov_raw_seek(&impl_->vf, 0);
}

std::size_t MusicStream::read(Int16* dst, std::size_t frames, bool loop) {
	if (!impl_->open_) return 0;

	std::size_t written = 0;
	int section = 0;
	while (written < frames) {
		long want = static_cast<long>((frames - written) * 2); // int16 stereo samples
		long got = ov_read(&impl_->vf, reinterpret_cast<char*>(dst + written * 2),
		                   want * 2,  // bytes
		                   0, 2, 1, &section);
		if (got < 0) break; // error
		if (got == 0) {
			if (!loop) break;
			ov_raw_seek(&impl_->vf, 0);
			continue;
		}
		written += static_cast<std::size_t>(got) / 4; // 2 channels * 2 bytes
	}
	return written;
}

// ====================================================================
//                          AudioDevice
// ====================================================================
struct Voice {
	const SoundData* data;
	std::size_t      pos;     // in frames
	int              volume;  // 0..100
	bool             loop;
	bool             active;

	Voice() : data(nullptr), pos(0), volume(100), loop(false), active(false) {}
};

struct AudioDevice::Impl {
	std::atomic<bool>       running;
	std::thread             thread;
	mutable std::mutex      mtx;

	snd_pcm_t*              pcm;
	int                     rate;
	unsigned int            periodFrames;

	Voice                   voices[AudioDevice::MAX_VOICES];

	MusicStream*            music;
	int                     musicVolume;
	bool                    musicLoop;

	// Stereo float accumulation buffer (per period), then clamp → int16.
	std::vector<float>      mixBuf;

	Impl()
		: running(false), pcm(nullptr), rate(44100), periodFrames(256),
		  music(nullptr), musicVolume(100), musicLoop(false) {
		mixBuf.resize(periodFrames * 2);
	}

	// Mix one period of audio into mixBuf (interleaved stereo float, unclamped).
	void mixPeriod(unsigned int frames) {
		std::fill(mixBuf.begin(), mixBuf.end(), 0.0f);
		if (frames * 2 > mixBuf.size()) mixBuf.assign(frames * 2, 0.0f);

		// Voices (SFX). Mix directly into float buffer; mark inactive when finished.
		for (int i = 0; i < AudioDevice::MAX_VOICES; ++i) {
			Voice& v = voices[i];
			if (!v.active || !v.data) continue;
			const Int16* s = v.data->samples();
			std::size_t  sf = v.data->frames();
			if (sf == 0) { v.active = false; continue; }

			float gain = static_cast<float>(v.volume) / 100.0f;
			std::size_t out = 0;
			while (out < frames) {
				std::size_t remain = frames - out;
				std::size_t avail  = sf - v.pos;
				std::size_t n      = remain < avail ? remain : avail;
				for (std::size_t k = 0; k < n; ++k) {
					std::size_t src = (v.pos + k) * 2;
					std::size_t dst = (out + k) * 2;
					mixBuf[dst + 0] += gain * (static_cast<float>(s[src + 0]) / 32768.0f);
					mixBuf[dst + 1] += gain * (static_cast<float>(s[src + 1]) / 32768.0f);
				}
				out += n;
				v.pos += n;
				if (v.pos >= sf) {
					if (v.loop) {
						v.pos = 0;
					} else {
						v.active = false;
						break;
					}
				}
			}
		}

		// Music (one stream, exclusive).
		if (music && musicVolume > 0) {
			float gain = static_cast<float>(musicVolume) / 100.0f;
			// Decode directly into temp int16 buffer, then add to mix.
			std::vector<Int16> raw(frames * 2);
			std::size_t got = music->read(raw.data(), frames, musicLoop);
			for (std::size_t i = 0; i < got; ++i) {
				mixBuf[i * 2 + 0] += gain * (static_cast<float>(raw[i * 2 + 0]) / 32768.0f);
				mixBuf[i * 2 + 1] += gain * (static_cast<float>(raw[i * 2 + 1]) / 32768.0f);
			}
		}
	}

	void audioLoop() {
		std::vector<Int16> outBuf(periodFrames * 2);
		while (running.load()) {
			int ms = snd_pcm_wait(pcm, 100);
			if (ms <= 0) {
				if (ms == -EPIPE) snd_pcm_prepare(pcm);
				continue;
			}
			{
				std::lock_guard<std::mutex> lk(mtx);
				mixPeriod(periodFrames);
				for (unsigned int i = 0; i < periodFrames * 2; ++i) {
					float v = mixBuf[i];
					if (v > 1.0f) v = 1.0f;
					if (v < -1.0f) v = -1.0f;
					outBuf[i] = static_cast<Int16>(v * 32767.0f);
				}
			}
			snd_pcm_sframes_t fr = snd_pcm_writei(pcm, outBuf.data(), periodFrames);
			if (fr < 0) {
				if (fr == -EPIPE) snd_pcm_prepare(pcm);
				else break;
			}
		}
	}
};

AudioDevice::AudioDevice() : impl_(new Impl) {}
AudioDevice::~AudioDevice() { shutdown(); }

bool AudioDevice::init(int sampleRate) {
	if (impl_->running.load()) return true;
	impl_->rate = sampleRate;

	int err = snd_pcm_open(&impl_->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		impl_->pcm = nullptr;
		return false;
	}
	snd_pcm_hw_params_t* hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(impl_->pcm, hw);
	snd_pcm_hw_params_set_access(impl_->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(impl_->pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(impl_->pcm, hw, 2);
	unsigned int rate = static_cast<unsigned int>(sampleRate);
	snd_pcm_hw_params_set_rate_near(impl_->pcm, hw, &rate, nullptr);
	snd_pcm_uframes_t period = impl_->periodFrames;
	snd_pcm_hw_params_set_period_size_near(impl_->pcm, hw, &period, nullptr);
	snd_pcm_uframes_t buf = period * 4;
	snd_pcm_hw_params_set_buffer_size_near(impl_->pcm, hw, &buf);
	err = snd_pcm_hw_params(impl_->pcm, hw);
	if (err < 0) {
		snd_pcm_close(impl_->pcm); impl_->pcm = nullptr;
		return false;
	}
	snd_pcm_prepare(impl_->pcm);

	impl_->running.store(true);
	impl_->thread = std::thread([this]{ impl_->audioLoop(); });
	return true;
}

void AudioDevice::shutdown() {
	if (!impl_->running.load()) return;
	impl_->running.store(false);
	if (impl_->thread.joinable()) impl_->thread.join();
	if (impl_->pcm) {
		snd_pcm_drain(impl_->pcm);
		snd_pcm_close(impl_->pcm);
		impl_->pcm = nullptr;
	}
	std::lock_guard<std::mutex> lk(impl_->mtx);
	for (int i = 0; i < MAX_VOICES; ++i) impl_->voices[i].active = false;
	impl_->music = nullptr;
}

bool AudioDevice::isInitialized() const noexcept {
	return impl_->running.load();
}

int AudioDevice::play(const SoundData& sound, int volume, bool loop) {
	if (!impl_->running.load() || !sound.valid()) return -1;
	std::lock_guard<std::mutex> lk(impl_->mtx);
	for (int i = 0; i < MAX_VOICES; ++i) {
		if (!impl_->voices[i].active) {
			Voice& v = impl_->voices[i];
			v.data    = &sound;
			v.pos     = 0;
			v.volume  = volume;
			v.loop    = loop;
			v.active  = true;
			return i;
		}
	}
	return -1;
}

void AudioDevice::stop(int vid) {
	if (vid < 0 || vid >= MAX_VOICES) return;
	std::lock_guard<std::mutex> lk(impl_->mtx);
	impl_->voices[vid].active = false;
}

void AudioDevice::stopAll() {
	std::lock_guard<std::mutex> lk(impl_->mtx);
	for (int i = 0; i < MAX_VOICES; ++i) impl_->voices[i].active = false;
}

void AudioDevice::setVolume(int vid, int volume) {
	if (vid < 0 || vid >= MAX_VOICES) return;
	std::lock_guard<std::mutex> lk(impl_->mtx);
	impl_->voices[vid].volume = volume;
}

bool AudioDevice::isPlaying(int vid) const {
	if (vid < 0 || vid >= MAX_VOICES) return false;
	std::lock_guard<std::mutex> lk(impl_->mtx);
	return impl_->voices[vid].active;
}

void AudioDevice::playMusic(MusicStream* stream, int volume, bool loop) {
	std::lock_guard<std::mutex> lk(impl_->mtx);
	if (impl_->music == stream && impl_->musicLoop == loop) {
		impl_->musicVolume = volume;
		return;
	}
	impl_->music       = stream;
	impl_->musicVolume = volume;
	impl_->musicLoop   = loop;
	if (stream) stream->rewind();
}

void AudioDevice::stopMusic() {
	std::lock_guard<std::mutex> lk(impl_->mtx);
	impl_->music = nullptr;
}

void AudioDevice::setMusicVolume(int volume) {
	std::lock_guard<std::mutex> lk(impl_->mtx);
	impl_->musicVolume = volume;
}

bool AudioDevice::isMusicPlaying() const {
	std::lock_guard<std::mutex> lk(impl_->mtx);
	return impl_->music != nullptr;
}
