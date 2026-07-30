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

#ifndef GAME_CTRL_H
#define GAME_CTRL_H

#include "bh.h"
#include "keyframe.h"
#include "spx.h"


enum TFrameType {
	START,
	FINISH,
	WONRACE,
	LOSTRACE,
	NUM_FRAME_TYPES
};


class TTexture;

struct TAvatar {
	std::string filename;
	TTexture* texture;

	TAvatar(const std::string& filename_, TTexture* texture_)
		: filename(filename_), texture(texture_)
	{}
};

struct TPlayer {
	std::string name;
	CControl *ctrl;
	const TAvatar* avatar;

	TPlayer(const std::string& name_ = emptyString, const TAvatar* avatar_ = nullptr)
		: name(name_), ctrl(nullptr), avatar(avatar_)
	{}
};

class CPlayers {
private:
	std::vector<TPlayer> plyr;
	std::vector<TAvatar> avatars;

	const TAvatar* FindAvatar(const std::string& name) const;
public:
	~CPlayers();

	TPlayer* GetPlayer(std::size_t index) { return &plyr[index]; }
	void SetSinglePlayer(const std::string& name);
	void ResetControls();
	void AllocControl(std::size_t player);
	bool LoadAvatars();
	std::size_t numAvatars() const { return avatars.size(); }
	std::size_t numPlayers() const { return plyr.size(); }

	TTexture* GetAvatarTexture(std::size_t avatar) const;
	const std::string& GetDirectAvatarName(std::size_t avatar) const;
};

extern CPlayers Players;

// -------------------------------- characters ------------------------

struct TCharacter {
	std::string name;
	std::string dir;
	TTexture* preview;
	CCharShape *shape;
	CKeyframe frames[NUM_FRAME_TYPES];
	int type;
	bool finishframesok;

	CKeyframe* GetKeyframe(TFrameType frametype);
};

class CCharacter {
public:
	std::vector<TCharacter> CharList;

	~CCharacter();

	bool LoadCharacterList();
	void LoadCharacterPreviews();
	void FreeCharacterPreviews();
};

extern CCharacter Char;


#endif
