/* --------------------------------------------------------------------
EXTREME TUXRACER

Copyright (C) 2010 Extreme Tux Racer Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
---------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "audio.h"
#include "n_audio.h"
#include "spx.h"

// the global instances of the 2 audio classes
CSound Sound;
CMusic Music;
#define MIX_MAX_VOLUME 100

// ---------------------------------------------------------------------------
// AudioDevice singleton — initialized lazily on first use. Both CSound
// (SFX voices) and CMusic (streamed music) share this single device +
// audio thread. The audio thread drains the mix buffer to ALSA in
// period-sized blocks (mirrors PS3 libaudio block-based DMA output).
// ---------------------------------------------------------------------------
namespace {
AudioDevice& audioDevice() {
	static AudioDevice dev;
	static bool inited = false;
	if (!inited) {
		dev.init(44100);
		inited = true;
	}
	return dev;
}
} // namespace

// ---------------------------------------------------------------------------
// TSound — a decoded WAV held in memory (SoundData) plus per-id playback
// state (currently active voice id, current volume, last loop flag).
// ---------------------------------------------------------------------------
struct TSound {
	SoundData data;
	int       voice_id; // -1 when not playing
	int       volume;   // 0..100
	bool      loop;     // last Play() loop argument (drives Halt semantics)

	explicit TSound(int volume_) : voice_id(-1), volume(volume_), loop(false) {}

	void setVolume(int v) { volume = v; }

	void Play(bool loop_) {
		// No-op if already playing.
		if (voice_id >= 0 && audioDevice().isPlaying(voice_id)) return;
		loop = loop_;
		voice_id = audioDevice().play(data, volume, loop_);
	}

	void stop() {
		if (voice_id >= 0) audioDevice().stop(voice_id);
		voice_id = -1;
	}
};

// --------------------------------------------------------------------
//				class CSound
// --------------------------------------------------------------------

CSound::~CSound() {
	FreeSounds();
}

bool CSound::LoadChunk(const std::string& name, const std::string& filename) {
	sounds.emplace_back(new TSound(param.sound_volume));
	if (!sounds.back()->data.loadWav(filename)) {
		return false;
	}
	SoundIndex[name] = sounds.size()-1;
	return true;
}

// Load all soundfiles listed in "/sounds/sounds.lst"
void CSound::LoadSoundList() {
	CSPList list;
	if (list.Load(param.sounds_dir, "sounds.lst")) {
		for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line) {
			std::string name = SPStrN(*line, "name");
			std::string soundfile = SPStrN(*line, "file");
			std::string path = MakePathStr(param.sounds_dir, soundfile);
			if (LoadChunk(name, path)) {
				// Per-entry [vol] scales the global sound_volume (0..1 multiplier).
				float vol = SPFloatN(*line, "vol", 1.0f);
				sounds.back()->setVolume(static_cast<int>(param.sound_volume * vol));
			}
		}
	}
}

void CSound::FreeSounds() {
	HaltAll();
	for (std::size_t i = 0; i < sounds.size(); i++)
		delete sounds[i];
	sounds.clear();
	SoundIndex.clear();
}

std::size_t CSound::GetSoundIdx(const std::string& name) const {
	try {
		return SoundIndex.at(name);
	} catch (...) {
		return -1;
	}
}

void CSound::SetVolume(std::size_t soundid, int volume) {
	if (soundid >= sounds.size()) return;

	volume = clamp(0, volume, MIX_MAX_VOLUME);
	sounds[soundid]->setVolume(volume);
	if (sounds[soundid]->voice_id >= 0)
		audioDevice().setVolume(sounds[soundid]->voice_id, volume);
}

void CSound::SetVolume(const std::string& name, int volume) {
	SetVolume(GetSoundIdx(name), volume);
}

// ------------------- play -------------------------------------------

void CSound::Play(std::size_t soundid, bool loop) {
	if (soundid >= sounds.size()) return;

	sounds[soundid]->Play(loop);
}

void CSound::Play(const std::string& name, bool loop) {
	Play(GetSoundIdx(name), loop);
}

void CSound::Play(std::size_t soundid, bool loop, int volume) {
	if (soundid >= sounds.size()) return;

	volume = clamp(0, volume, MIX_MAX_VOLUME);
	sounds[soundid]->setVolume(volume);
	sounds[soundid]->Play(loop);
}

void CSound::Play(const std::string& name, bool loop, int volume) {
	Play(GetSoundIdx(name), loop, volume);
}

void CSound::Halt(std::size_t soundid) {
	if (soundid >= sounds.size()) return;

	// Only looping sounds are halted by Halt().
	if (sounds[soundid]->loop)
		sounds[soundid]->stop();
}

void CSound::Halt(const std::string& name) {
	Halt(GetSoundIdx(name));
}

void CSound::HaltAll() {
	for (std::size_t i = 0; i < sounds.size(); i++) {
		sounds[i]->stop();
	}
}

// --------------------------------------------------------------------
//				class CMusic
// --------------------------------------------------------------------

CMusic::CMusic() {
	curr_music = nullptr;
	curr_volume = 10;
}
CMusic::~CMusic() {
	FreeMusics();
}

bool CMusic::LoadPiece(const std::string& name, const std::string& filename) {
	MusicStream* m = new MusicStream();
	if (!m->open(filename)) {
		Message("could not load music", filename);
		delete m;
		return false;
	}
	MusicIndex[name] = musics.size();
	musics.push_back(m);
	return true;
}

void CMusic::LoadMusicList() {
	// --- music ---
	CSPList list;
	if (list.Load(param.music_dir, "music.lst")) {
		musics.reserve(list.size());
		for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line) {
			std::string name = SPStrN(*line, "name");
			std::string musicfile = SPStrN(*line, "file");
			std::string path = MakePathStr(param.music_dir, musicfile);
			LoadPiece(name, path);
		}
	} else {
		Message("could not load music.lst");
		return;
	}

	// --- racing themes ---
	list.clear();
	ThemesIndex.clear();
	if (list.Load(param.music_dir, "racing_themes.lst")) {
		themes.resize(list.size());
		std::size_t i = 0;
		for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line, i++) {
			std::string name = SPStrN(*line, "name");
			ThemesIndex[name] = i;
			std::string item = SPStrN(*line, "race", "race_1");
			themes[i].situation[0] = musics[MusicIndex[item]];
			item = SPStrN(*line, "wonrace", "wonrace_1");
			themes[i].situation[1] = musics[MusicIndex[item]];
			item = SPStrN(*line, "lostrace", "lostrace_1");
			themes[i].situation[2] = musics[MusicIndex[item]];
		}
	} else Message("could not load racing_themes.lst");
}

void CMusic::FreeMusics() {
	Halt();
	for (std::size_t i = 0; i < musics.size(); i++)
		delete musics[i];
	musics.clear();
	MusicIndex.clear();

	themes.clear();
	ThemesIndex.clear();

	curr_music = nullptr;
}

std::size_t CMusic::GetMusicIdx(const std::string& name) const {
	try {
		return MusicIndex.at(name);
	} catch (...) {
		return -1;
	}
}

std::size_t CMusic::GetThemeIdx(const std::string& theme) const {
	try {
		return ThemesIndex.at(theme);
	} catch (...) {
		return -1;
	}
}

void CMusic::SetVolume(int volume) {
	volume = clamp(0, volume, MIX_MAX_VOLUME);
	audioDevice().setMusicVolume(volume);
	curr_volume = volume;
}

bool CMusic::Play(MusicStream* music, bool loop, int volume) {
	if (!music)
		return false;

	volume = clamp(0, volume, MIX_MAX_VOLUME);
	if (music != curr_music) {
		audioDevice().playMusic(music, volume, loop);
		curr_music = music;
	}
	return true;
}

bool CMusic::Play(std::size_t musid, bool loop) {
	if (musid >= musics.size()) return false;
	MusicStream* music = musics[musid];
	return Play(music, loop, curr_volume);
}

bool CMusic::Play(const std::string& name, bool loop) {
	return Play(GetMusicIdx(name), loop);
}

bool CMusic::Play(std::size_t musid, bool loop, int volume) {
	if (musid >= musics.size()) return false;
	MusicStream* music = musics[musid];
	return Play(music, loop, volume);
}

bool CMusic::Play(const std::string& name, bool loop, int volume) {
	return Play(GetMusicIdx(name), loop, volume);
}

bool CMusic::PlayTheme(std::size_t theme, ESituation situation) {
	if (theme >= themes.size()) return false;
	if (situation >= SITUATION_COUNT) return false;
	MusicStream* music = themes[theme].situation[situation];
	return Play(music, true, curr_volume);
}

void CMusic::Halt() {
	if (curr_music) {
		audioDevice().stopMusic();
		curr_music = nullptr;
	}
}
