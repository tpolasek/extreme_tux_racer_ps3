/* --------------------------------------------------------------------
EXTREME TUXRACER

Copyright (C) 2010 Extreme Tux Racer Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
---------------------------------------------------------------------*/

#include "game_ctrl.h"
#include "spx.h"
#include "course.h"
#include "env.h"
#include "audio.h"
#include "textures.h"
#include "tux.h"
#include "physics.h"

// --------------------------------------------------------------------
//				player administration
// --------------------------------------------------------------------

CPlayers Players;

CPlayers::~CPlayers() {
	ResetControls();
	for (std::size_t i = 0; i < avatars.size(); i++)
		delete avatars[i].texture;
}

void CPlayers::SetSinglePlayer(const std::string& name) {
	plyr.clear();
	plyr.emplace_back(name, avatars.empty() ? nullptr : &avatars[0]);
}

void CPlayers::ResetControls() {
	for (std::size_t i=0; i<plyr.size(); i++) {
		delete plyr[i].ctrl;
		plyr[i].ctrl = nullptr;
	}
}

// called in module regist.cpp:
void CPlayers::AllocControl(std::size_t player) {
	if (player >= plyr.size()) return;
	if (plyr[player].ctrl != nullptr) return;
	plyr[player].ctrl = new CControl;
}

// ----------------------- avatars ------------------------------------

bool CPlayers::LoadAvatars() {
	CSPList list;

	if (!list.Load(param.player_dir, "avatars.lst")) {
		Message("could not load avators.lst");
		return false;
	}

	avatars.reserve(list.size());
	for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line) {
		std::string filename = SPStrN(*line, "file", "unknown");
		TTexture* texture = new TTexture();
		if (texture && texture->Load(param.player_dir, filename)) {
			avatars.emplace_back(filename, texture);
		} else
			delete texture;
	}
	return true;
}

TTexture* CPlayers::GetAvatarTexture(std::size_t avatar) const {
	if (avatar >= avatars.size()) return 0;
	return avatars[avatar].texture;
}

const std::string& CPlayers::GetDirectAvatarName(std::size_t avatar) const {
	if (avatar >= avatars.size()) return emptyString;
	return avatars[avatar].filename;
}

// ********************************************************************
//				Character Administration
// ********************************************************************

CKeyframe* TCharacter::GetKeyframe(TFrameType frametype) {
	if (frametype < 0 || frametype >= NUM_FRAME_TYPES) return nullptr;
	return &frames[frametype];
}


CCharacter Char;

static const std::string char_type_index = "[spheres]0[3d]1";

CCharacter::~CCharacter() {
	for (std::size_t i = 0; i < CharList.size(); i++) {
		delete CharList[i].preview;
		delete CharList[i].shape;
	}
}

bool CCharacter::LoadCharacterList() {
	CSPList list;

	if (!list.Load(param.char_dir, "characters.lst")) {
		Message("could not load characters.lst");
		return false;
	}

	CharList.resize(list.size());
	std::size_t i = 0;
	for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line, i++) {
		CharList[i].name = SPStrN(*line, "name");
		CharList[i].dir = SPStrN(*line, "dir");
		std::string typestr = SPStrN(*line, "type", "unknown");
		CharList[i].type = SPIntN(char_type_index, typestr, -1);

		std::string charpath = MakePathStr(param.char_dir, CharList[i].dir);
		if (DirExists(charpath.c_str())) {
			std::string previewfile = charpath + SEP "preview.png";

			TCharacter* ch = &CharList[i];
			ch->preview = new TTexture();
			if (!ch->preview->Load(previewfile, false)) {
				Message("could not load previewfile of character");
			}

			ch->shape = new CCharShape;
			if (ch->shape->Load(charpath, "shape.lst", false) == false) {
				delete ch->shape;
				ch->shape = nullptr;
				Message("could not load character shape");
			}

			ch->frames[0].Load(charpath, "start.lst");
			ch->finishframesok = true;
			ch->frames[1].Load(charpath, "finish.lst");
			if (ch->frames[1].loaded == false) ch->finishframesok = false;
			ch->frames[2].Load(charpath, "wonrace.lst");
			if (ch->frames[2].loaded == false) ch->finishframesok = false;
			ch->frames[3].Load(charpath, "lostrace.lst");
			if (ch->frames[3].loaded == false) ch->finishframesok = false;
		}
	}
	return !CharList.empty();
}

void CCharacter::LoadCharacterPreviews() {
	for (std::size_t i = 0; i < CharList.size(); i++) {
		if (CharList[i].preview != nullptr) continue;

		std::string charpath = MakePathStr(param.char_dir, CharList[i].dir);
		std::string previewfile = charpath + SEP "preview.png";
		CharList[i].preview = new TTexture();
		if (!CharList[i].preview->Load(previewfile, false)) {
			delete CharList[i].preview;
			CharList[i].preview = nullptr;
			Message("could not load previewfile of character");
		}
	}
}

void CCharacter::FreeCharacterPreviews() {
	for (std::size_t i=0; i<CharList.size(); i++) {
		delete CharList[i].preview;
		CharList[i].preview = 0;
	}
}
