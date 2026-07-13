/* --------------------------------------------------------------------
EXTREME TUXRACER - PSL1GHT audio backend.

PSL1GHT libaudio exposes a fixed 48 kHz, interleaved float32 ring buffer.
A dedicated PPU thread waits for block-consumed events, mixes the game's
WAV effects and streamed Vorbis music, and fills the next hardware block.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_audio.h"
#include "ps3_tty.h"

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

constexpr int kOutputRate = 48000;
constexpr std::uint64_t kPhaseOne = UINT64_C(1) << 32;

bool readU16LE(std::FILE* fp, std::uint16_t& value) {
	std::uint8_t bytes[2];
	if (std::fread(bytes, 1, sizeof(bytes), fp) != sizeof(bytes)) return false;
	value = static_cast<std::uint16_t>(bytes[0]) |
	        static_cast<std::uint16_t>(bytes[1] << 8);
	return true;
}

bool readU32LE(std::FILE* fp, std::uint32_t& value) {
	std::uint8_t bytes[4];
	if (std::fread(bytes, 1, sizeof(bytes), fp) != sizeof(bytes)) return false;
	value = static_cast<std::uint32_t>(bytes[0]) |
	        (static_cast<std::uint32_t>(bytes[1]) << 8) |
	        (static_cast<std::uint32_t>(bytes[2]) << 16) |
	        (static_cast<std::uint32_t>(bytes[3]) << 24);
	return true;
}

int clampVolume(int volume) {
	return std::max(0, std::min(volume, 100));
}

std::uint64_t rateStep(int sourceRate) {
	if (sourceRate <= 0) return kPhaseOne;
	return (static_cast<std::uint64_t>(sourceRate) << 32) /
	       static_cast<std::uint64_t>(kOutputRate);
}

} // namespace

// ====================================================================
//                          SoundData (WAV)
// ====================================================================
SoundData::SoundData() : frames_(0), rate_(0) {}
SoundData::~SoundData() { release(); }

void SoundData::release() {
	samples_.clear();
	frames_ = 0;
	rate_ = 0;
}

bool SoundData::loadWav(const std::string& filename) {
	release();
	std::FILE* fp = std::fopen(filename.c_str(), "rb");
	if (!fp) return false;

	char id[4];
	std::uint32_t riffSize = 0;
	bool ok = std::fread(id, 1, sizeof(id), fp) == sizeof(id) &&
	          std::memcmp(id, "RIFF", 4) == 0 &&
	          readU32LE(fp, riffSize) &&
	          std::fread(id, 1, sizeof(id), fp) == sizeof(id) &&
	          std::memcmp(id, "WAVE", 4) == 0;
	(void)riffSize;
	if (!ok) {
		std::fclose(fp);
		return false;
	}

	std::uint16_t format = 0;
	std::uint16_t channels = 0;
	std::uint16_t bits = 0;
	std::uint32_t sampleRate = 0;
	bool haveFormat = false;
	bool haveData = false;
	std::vector<std::uint8_t> data;

	while (std::fread(id, 1, sizeof(id), fp) == sizeof(id)) {
		std::uint32_t chunkSize = 0;
		if (!readU32LE(fp, chunkSize)) break;

		if (std::memcmp(id, "fmt ", 4) == 0) {
			std::uint32_t byteRate = 0;
			std::uint16_t blockAlign = 0;
			if (chunkSize < 16 || !readU16LE(fp, format) ||
			    !readU16LE(fp, channels) || !readU32LE(fp, sampleRate) ||
			    !readU32LE(fp, byteRate) || !readU16LE(fp, blockAlign) ||
			    !readU16LE(fp, bits)) {
				ok = false;
				break;
			}
			(void)byteRate;
			(void)blockAlign;
			if (chunkSize > 16 && std::fseek(fp, static_cast<long>(chunkSize - 16), SEEK_CUR) != 0) {
				ok = false;
				break;
			}
			haveFormat = true;
		} else if (std::memcmp(id, "data", 4) == 0) {
			data.resize(chunkSize);
			if (chunkSize != 0 && std::fread(data.data(), 1, chunkSize, fp) != chunkSize) {
				ok = false;
				break;
			}
			haveData = true;
		} else if (std::fseek(fp, static_cast<long>(chunkSize), SEEK_CUR) != 0) {
			ok = false;
			break;
		}

		if ((chunkSize & 1U) != 0 && std::fseek(fp, 1, SEEK_CUR) != 0) {
			ok = false;
			break;
		}
	}
	std::fclose(fp);

	if (!ok || !haveFormat || !haveData || format != 1 ||
	    (channels != 1 && channels != 2) || (bits != 8 && bits != 16) ||
	    sampleRate == 0) {
		return false;
	}

	const std::size_t bytesPerSample = bits / 8;
	const std::size_t frameBytes = bytesPerSample * channels;
	if (frameBytes == 0 || data.size() < frameBytes || data.size() % frameBytes != 0) return false;

	frames_ = data.size() / frameBytes;
	rate_ = static_cast<int>(sampleRate);
	samples_.resize(frames_ * 2);
	for (std::size_t frame = 0; frame < frames_; ++frame) {
		for (std::size_t outChannel = 0; outChannel < 2; ++outChannel) {
			const std::size_t inChannel = channels == 1 ? 0 : outChannel;
			const std::size_t offset = frame * frameBytes + inChannel * bytesPerSample;
			Int16 sample;
			if (bits == 8) {
				sample = static_cast<Int16>((static_cast<int>(data[offset]) - 128) * 256);
			} else {
				const std::uint16_t value = static_cast<std::uint16_t>(data[offset]) |
				                            static_cast<std::uint16_t>(data[offset + 1] << 8);
				const int signedValue = value >= 0x8000U
					? static_cast<int>(value) - 0x10000
					: static_cast<int>(value);
				sample = static_cast<Int16>(signedValue);
			}
			samples_[frame * 2 + outChannel] = sample;
		}
	}
	return true;
}

// ====================================================================
//                          MusicStream (OGG)
// ====================================================================
struct MusicStream::Impl {
	OggVorbis_File vf;
	bool open;
	int rate;
	int channels;
	std::size_t frameCount;

	Impl() : open(false), rate(0), channels(0), frameCount(0) {
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

	vorbis_info* info = ov_info(&impl_->vf, -1);
	if (!info || info->rate <= 0 || info->channels <= 0) {
		ov_clear(&impl_->vf);
		std::memset(&impl_->vf, 0, sizeof(impl_->vf));
		return false;
	}

	const ogg_int64_t total = ov_pcm_total(&impl_->vf, -1);
	impl_->open = true;
	impl_->rate = static_cast<int>(info->rate);
	impl_->channels = info->channels;
	impl_->frameCount = total > 0 ? static_cast<std::size_t>(total) : 0;
	return true;
}

void MusicStream::close() {
	if (impl_->open) ov_clear(&impl_->vf);
	std::memset(&impl_->vf, 0, sizeof(impl_->vf));
	impl_->open = false;
	impl_->rate = 0;
	impl_->channels = 0;
	impl_->frameCount = 0;
}

bool MusicStream::isOpen() const noexcept { return impl_->open; }
int MusicStream::sampleRate() const noexcept { return impl_->rate; }
std::size_t MusicStream::frames() const noexcept { return impl_->frameCount; }

std::size_t MusicStream::read(Int16* dst, std::size_t frames, bool loop) {
	if (!impl_->open || !dst || frames == 0) return 0;

	std::size_t written = 0;
	int errors = 0;
	while (written < frames) {
		float** pcm = nullptr;
		int section = 0;
		const int request = static_cast<int>(std::min<std::size_t>(
			frames - written, static_cast<std::size_t>(std::numeric_limits<int>::max())));
		const long got = ov_read_float(&impl_->vf, &pcm, request, &section);
		if (got < 0) {
			if (++errors >= 8) break;
			continue;
		}
		if (got == 0) {
			if (!loop || ov_pcm_seek(&impl_->vf, 0) != 0) break;
			continue;
		}
		errors = 0;

		for (long frame = 0; frame < got; ++frame) {
			for (int channel = 0; channel < 2; ++channel) {
				const int sourceChannel = impl_->channels == 1 ? 0 : channel;
				float value = pcm[sourceChannel][frame];
				value = std::max(-1.0f, std::min(value, 1.0f));
				dst[(written + static_cast<std::size_t>(frame)) * 2 + channel] =
					static_cast<Int16>(value * 32767.0f);
			}
		}
		written += static_cast<std::size_t>(got);
	}
	return written;
}

void MusicStream::rewind() {
	if (impl_->open) (void)ov_pcm_seek(&impl_->vf, 0);
}

// ====================================================================
//                          AudioDevice
// ====================================================================
namespace {

struct Voice {
	const SoundData* data;
	std::uint64_t position;
	std::uint64_t step;
	int volume;
	bool loop;
	bool active;

	Voice()
		: data(nullptr), position(0), step(kPhaseOne), volume(100),
		  loop(false), active(false) {}
};

class MutexLock {
public:
	explicit MutexLock(sys_mutex_t mutex) : mutex_(mutex) { (void)sysMutexLock(mutex_, 0); }
	~MutexLock() { (void)sysMutexUnlock(mutex_); }

private:
	sys_mutex_t mutex_;
};

} // namespace

struct AudioDevice::Impl {
	std::atomic<bool> running;
	sys_ppu_thread_t thread;
	sys_mutex_t mutex;
	bool mutexCreated;

	u32 port;
	audioPortConfig config;
	sys_event_queue_t eventQueue;
	sys_ipc_key_t eventQueueKey;
	bool audioInitialized;
	bool portOpened;
	bool queueCreated;
	bool queueAttached;
	bool portStarted;
	bool threadCreated;

	Voice voices[AudioDevice::MAX_VOICES];
	MusicStream* music;
	int musicVolume;
	bool musicLoop;
	std::uint64_t musicPhase;
	std::uint64_t musicStep;
	Int16 musicCurrent[2];
	Int16 musicNext[2];
	bool musicPrimed;
	bool musicAtEnd;
	std::vector<Int16> musicCache;
	std::size_t musicCachePos;
	std::size_t musicCacheFrames;

	Impl()
		: running(false), thread(0), mutex(0), mutexCreated(false), port(0),
		  eventQueue(0), eventQueueKey(0), audioInitialized(false),
		  portOpened(false), queueCreated(false), queueAttached(false),
		  portStarted(false), threadCreated(false), music(nullptr),
		  musicVolume(100), musicLoop(false), musicPhase(0),
		  musicStep(kPhaseOne), musicPrimed(false), musicAtEnd(false),
		  musicCache(2048), musicCachePos(0), musicCacheFrames(0) {
		std::memset(&config, 0, sizeof(config));
		musicCurrent[0] = musicCurrent[1] = 0;
		musicNext[0] = musicNext[1] = 0;
	}

	void resetMusicResampler() {
		musicPhase = 0;
		musicStep = music ? rateStep(music->sampleRate()) : kPhaseOne;
		musicPrimed = false;
		musicAtEnd = false;
		musicCachePos = 0;
		musicCacheFrames = 0;
	}

	bool readMusicFrame(Int16 sample[2]) {
		if (!music) return false;
		if (musicCachePos >= musicCacheFrames) {
			musicCacheFrames = music->read(musicCache.data(), musicCache.size() / 2, musicLoop);
			musicCachePos = 0;
			if (musicCacheFrames == 0) return false;
		}
		sample[0] = musicCache[musicCachePos * 2];
		sample[1] = musicCache[musicCachePos * 2 + 1];
		++musicCachePos;
		return true;
	}

	bool primeMusic() {
		if (musicPrimed) return true;
		if (!readMusicFrame(musicCurrent)) {
			music = nullptr;
			return false;
		}
		if (!readMusicFrame(musicNext)) {
			musicNext[0] = musicCurrent[0];
			musicNext[1] = musicCurrent[1];
			musicAtEnd = true;
		}
		musicPrimed = true;
		return true;
	}

	void mixVoices(float* output, std::size_t frames) {
		for (int voiceIndex = 0; voiceIndex < AudioDevice::MAX_VOICES; ++voiceIndex) {
			Voice& voice = voices[voiceIndex];
			if (!voice.active || !voice.data || !voice.data->valid()) continue;

			const std::size_t sourceFrames = voice.data->frames();
			const Int16* source = voice.data->samples();
			if (sourceFrames == 0) {
				voice.active = false;
				continue;
			}
			const std::uint64_t sourceLength = static_cast<std::uint64_t>(sourceFrames) << 32;
			const float gain = static_cast<float>(voice.volume) / (100.0f * 32768.0f);

			for (std::size_t frame = 0; frame < frames; ++frame) {
				if (voice.position >= sourceLength) {
					if (!voice.loop) {
						voice.active = false;
						break;
					}
					voice.position %= sourceLength;
				}

				const std::size_t current = static_cast<std::size_t>(voice.position >> 32);
				std::size_t next = current + 1;
				if (next >= sourceFrames) next = voice.loop ? 0 : current;
				const float fraction = static_cast<float>(
					voice.position & (kPhaseOne - 1)) / static_cast<float>(kPhaseOne);
				for (int channel = 0; channel < 2; ++channel) {
					const float a = static_cast<float>(source[current * 2 + channel]);
					const float b = static_cast<float>(source[next * 2 + channel]);
					output[frame * 2 + channel] += (a + (b - a) * fraction) * gain;
				}
				voice.position += voice.step;
			}
		}
	}

	void mixMusic(float* output, std::size_t frames) {
		if (!music || musicVolume <= 0 || !primeMusic()) return;
		const float gain = static_cast<float>(musicVolume) / (100.0f * 32768.0f);

		for (std::size_t frame = 0; frame < frames && music; ++frame) {
			const float fraction = static_cast<float>(musicPhase) / static_cast<float>(kPhaseOne);
			for (int channel = 0; channel < 2; ++channel) {
				const float a = static_cast<float>(musicCurrent[channel]);
				const float b = static_cast<float>(musicNext[channel]);
				output[frame * 2 + channel] += (a + (b - a) * fraction) * gain;
			}

			musicPhase += musicStep;
			while (musicPhase >= kPhaseOne && music) {
				musicPhase -= kPhaseOne;
				if (musicAtEnd) {
					music = nullptr;
					musicPrimed = false;
					break;
				}
				musicCurrent[0] = musicNext[0];
				musicCurrent[1] = musicNext[1];
				if (!readMusicFrame(musicNext)) {
					musicNext[0] = musicCurrent[0];
					musicNext[1] = musicCurrent[1];
					musicAtEnd = true;
				}
			}
		}
	}

	void mixBlock(float* output) {
		const std::size_t samples = AUDIO_BLOCK_SAMPLES * 2;
		std::fill(output, output + samples, 0.0f);
		mixVoices(output, AUDIO_BLOCK_SAMPLES);
		mixMusic(output, AUDIO_BLOCK_SAMPLES);
		for (std::size_t sample = 0; sample < samples; ++sample)
			output[sample] = std::max(-1.0f, std::min(output[sample], 1.0f));
	}

	void audioLoop() {
		volatile std::uint64_t* readIndex =
			reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uintptr_t>(config.readIndex));
		float* audioData = reinterpret_cast<float*>(static_cast<std::uintptr_t>(config.audioDataStart));

		while (running.load(std::memory_order_acquire)) {
			sys_event_t event;
			if (sysEventQueueReceive(eventQueue, &event, 20000) != 0) continue;
			if (!running.load(std::memory_order_acquire)) break;

			const std::uint32_t block = static_cast<std::uint32_t>((*readIndex + 1) % config.numBlocks);
			float* destination = audioData + block * AUDIO_BLOCK_SAMPLES * 2;
			{
				MutexLock lock(mutex);
				mixBlock(destination);
			}
			__sync_synchronize();
		}
		sysThreadExit(0);
	}

	static void threadEntry(void* argument) {
		static_cast<Impl*>(argument)->audioLoop();
	}

	void closeHardware() {
		if (portStarted) {
			(void)audioPortStop(port);
			portStarted = false;
		}
		if (queueAttached) {
			(void)audioRemoveNotifyEventQueue(eventQueueKey);
			queueAttached = false;
		}
		if (portOpened) {
			(void)audioPortClose(port);
			portOpened = false;
		}
		if (queueCreated) {
			(void)sysEventQueueDestroy(eventQueue, 0);
			queueCreated = false;
		}
		if (audioInitialized) {
			(void)audioQuit();
			audioInitialized = false;
		}
	}
};

AudioDevice::AudioDevice() : impl_(new Impl) {}
AudioDevice::~AudioDevice() { shutdown(); }

bool AudioDevice::init(int sampleRate) {
	(void)sampleRate; // The PS3 libaudio output rate is always 48 kHz.
	if (impl_->running.load(std::memory_order_acquire)) return true;

	sys_mutex_attr_t mutexAttributes;
	sysMutexAttrInitialize(mutexAttributes);
	std::memcpy(mutexAttributes.name, "etraudio", 8);
	if (sysMutexCreate(&impl_->mutex, &mutexAttributes) != 0) {
		sysTtyTrace("[etr] audio: sysMutexCreate failed\n");
		return false;
	}
	impl_->mutexCreated = true;

	if (audioInit() != 0) {
		sysTtyTrace("[etr] audio: audioInit failed\n");
		shutdown();
		return false;
	}
	impl_->audioInitialized = true;

	audioPortParam parameters;
	std::memset(&parameters, 0, sizeof(parameters));
	parameters.numChannels = AUDIO_PORT_2CH;
	parameters.numBlocks = AUDIO_BLOCK_8;
	parameters.attrib = AUDIO_PORT_INITLEVEL;
	parameters.level = 1.0f;
	if (audioPortOpen(&parameters, &impl_->port) != 0) {
		sysTtyTrace("[etr] audio: audioPortOpen failed\n");
		shutdown();
		return false;
	}
	impl_->portOpened = true;

	if (audioGetPortConfig(impl_->port, &impl_->config) != 0 ||
	    impl_->config.channelCount != 2 || impl_->config.numBlocks == 0 ||
	    impl_->config.readIndex == 0 || impl_->config.audioDataStart == 0) {
		sysTtyTrace("[etr] audio: invalid port configuration\n");
		shutdown();
		return false;
	}

	float* audioData = reinterpret_cast<float*>(
		static_cast<std::uintptr_t>(impl_->config.audioDataStart));
	std::fill(audioData,
	          audioData + impl_->config.numBlocks * AUDIO_BLOCK_SAMPLES * 2,
	          0.0f);
	__sync_synchronize();

	if (audioCreateNotifyEventQueue(&impl_->eventQueue, &impl_->eventQueueKey) != 0) {
		sysTtyTrace("[etr] audio: event queue creation failed\n");
		shutdown();
		return false;
	}
	impl_->queueCreated = true;
	if (audioSetNotifyEventQueue(impl_->eventQueueKey) != 0) {
		sysTtyTrace("[etr] audio: event queue attachment failed\n");
		shutdown();
		return false;
	}
	impl_->queueAttached = true;
	(void)sysEventQueueDrain(impl_->eventQueue);

	if (audioPortStart(impl_->port) != 0) {
		sysTtyTrace("[etr] audio: audioPortStart failed\n");
		shutdown();
		return false;
	}
	impl_->portStarted = true;
	impl_->running.store(true, std::memory_order_release);

	static char threadName[] = "etr_audio";
	if (sysThreadCreate(&impl_->thread, Impl::threadEntry, impl_.get(),
	                    999, 128 * 1024, THREAD_JOINABLE, threadName) != 0) {
		sysTtyTrace("[etr] audio: audio thread creation failed\n");
		impl_->running.store(false, std::memory_order_release);
		shutdown();
		return false;
	}
	impl_->threadCreated = true;
	sysTtyTrace("[etr] audio: PSL1GHT 48 kHz stereo output ready\n");
	return true;
}

void AudioDevice::shutdown() {
	impl_->running.store(false, std::memory_order_release);
	if (impl_->threadCreated) {
		u64 result = 0;
		(void)sysThreadJoin(impl_->thread, &result);
		impl_->threadCreated = false;
	}
	impl_->closeHardware();

	if (impl_->mutexCreated) {
		{
			MutexLock lock(impl_->mutex);
			for (int i = 0; i < MAX_VOICES; ++i) impl_->voices[i].active = false;
			impl_->music = nullptr;
		}
		(void)sysMutexDestroy(impl_->mutex);
		impl_->mutexCreated = false;
	}
}

bool AudioDevice::isInitialized() const noexcept {
	return impl_->running.load(std::memory_order_acquire);
}

int AudioDevice::play(const SoundData& sound, int volume, bool loop) {
	if (!impl_->running.load(std::memory_order_acquire) || !impl_->mutexCreated || !sound.valid()) return -1;
	MutexLock lock(impl_->mutex);
	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice& voice = impl_->voices[i];
		if (!voice.active) {
			voice.data = &sound;
			voice.position = 0;
			voice.step = rateStep(sound.sampleRate());
			voice.volume = clampVolume(volume);
			voice.loop = loop;
			voice.active = true;
			return i;
		}
	}
	return -1;
}

void AudioDevice::stop(int voiceId) {
	if (!impl_->mutexCreated || voiceId < 0 || voiceId >= MAX_VOICES) return;
	MutexLock lock(impl_->mutex);
	impl_->voices[voiceId].active = false;
}

void AudioDevice::stopAll() {
	if (!impl_->mutexCreated) return;
	MutexLock lock(impl_->mutex);
	for (int i = 0; i < MAX_VOICES; ++i) impl_->voices[i].active = false;
}

void AudioDevice::setVolume(int voiceId, int volume) {
	if (!impl_->mutexCreated || voiceId < 0 || voiceId >= MAX_VOICES) return;
	MutexLock lock(impl_->mutex);
	impl_->voices[voiceId].volume = clampVolume(volume);
}

bool AudioDevice::isPlaying(int voiceId) const {
	if (!impl_->mutexCreated || voiceId < 0 || voiceId >= MAX_VOICES) return false;
	MutexLock lock(impl_->mutex);
	return impl_->voices[voiceId].active;
}

void AudioDevice::playMusic(MusicStream* stream, int volume, bool loop) {
	if (!impl_->mutexCreated) return;
	MutexLock lock(impl_->mutex);
	if (impl_->music == stream && impl_->musicLoop == loop) {
		impl_->musicVolume = clampVolume(volume);
		return;
	}
	impl_->music = stream;
	impl_->musicVolume = clampVolume(volume);
	impl_->musicLoop = loop;
	if (stream) stream->rewind();
	impl_->resetMusicResampler();
}

void AudioDevice::stopMusic() {
	if (!impl_->mutexCreated) return;
	MutexLock lock(impl_->mutex);
	impl_->music = nullptr;
	impl_->resetMusicResampler();
}

void AudioDevice::setMusicVolume(int volume) {
	if (!impl_->mutexCreated) return;
	MutexLock lock(impl_->mutex);
	impl_->musicVolume = clampVolume(volume);
}

bool AudioDevice::isMusicPlaying() const {
	if (!impl_->mutexCreated) return false;
	MutexLock lock(impl_->mutex);
	return impl_->music != nullptr;
}
