/* --------------------------------------------------------------------
EXTREME TUXRACER

Copyright (C) 1999-2001 Jasmin F. Patry (Tuxracer)
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

#ifndef GUI_H
#define GUI_H

#include "bh.h"


#define CENTER -1


struct TRect {
	int left;
	int top;
	int width;
	int height;
};

struct TArea {
	int left;
	int right;
	int top;
	int bottom;
};


class TTexture;

// Abstract input actions driven entirely by the gamepad.
enum TInputAction {
	ACT_UP,
	ACT_DOWN,
	ACT_LEFT,
	ACT_RIGHT,
	ACT_CONFIRM,
	ACT_BACK
};

class TWidget {
protected:
	TVector2i position;
	bool active;
	bool visible;
	bool interactive;
public:
	bool focus;

	TWidget(int x, int y, int width, int height, bool interactive_ = true);
	virtual ~TWidget() {}
	virtual void Draw() const = 0;
	virtual void Action(TInputAction action) {}
	virtual void Focussed() {}
	virtual void Activated() {}
	bool focussed() const { return focus; }
	void SetActive(bool a) { active = a; if (!a) focus = false; Activated(); }
	void SetVisible(bool v) { visible = v; if (!v) focus = false; }
	bool GetActive() const { return active; }
	bool GetVisible() const { return visible; }
	bool GetInteractive() const { return interactive; }
};

class TLabel : public TWidget {
	sf::Text text;
public:
	TLabel(const std::string& string, int x, int y, const Color& color);
	void Focussed(bool masterFocus);
	void Draw() const;
	sf::Vector2f GetSize() const;
};
TLabel* AddLabel(const std::string& string, int x, int y, const Color& color);

class TFramedText : public TWidget {
	sf::RectangleShape frame;
	sf::Text text;
	bool borderFocus;
public:
	TFramedText(int x, int y, int width, int height, int line, const Color& backcol, const std::string& string, unsigned int ftsize, bool borderFocus_ = false);
	void Focussed(bool masterFocus);
	void Activated();
	void Draw() const;
	void SetString(const std::string& string) { text.setString(string); }
};
TFramedText* AddFramedText(int x, int y, int width, int height, int line, const Color& backcol, const std::string& text, unsigned int ftsize, bool borderFocus = false);

class TTextButton : public TWidget {
	sf::Text text;
public:
	TTextButton(int x, int y, const std::string& text_, int ftsize);
	void Focussed();
	void Draw() const;
};
TTextButton* AddTextButton(const std::string& text, int x, int y, int ftsize);
TTextButton* AddTextButtonN(const std::string& text, int x, int y, int rel_ftsize);

class TArrow : public TWidget {
	sf::Sprite sprite;
	bool down;
	void SetTexture();
public:
	TArrow(int x, int y, bool down_);
	void Activated();
	void Focussed();
	void Draw() const;
};
TArrow* AddArrow(int x, int y, bool down);

class TUpDown : public TWidget {
	TArrow up;
	TArrow down;
	TArrow& higher;
	TArrow& lower;
	int value;
	int minimum;
	int maximum;
	bool swapArrows;
public:
	TUpDown(int x, int y, int min_, int max_, int value_, int distance, bool swapArrows_);
	int GetValue() const { return value; }
	void SetValue(int value_);
	void SetMinimum(int min_);
	void SetMaximum(int max_);
	void Draw() const;
	void Action(TInputAction action);
};
TUpDown* AddUpDown(int x, int y, int minimum, int maximum, int value, int distance = 2, bool swapArrows = false);

// --------------------------------------------------------------------

void DrawGUI();
TWidget* ActionGUI(TInputAction action);
void SetFocus(TWidget* widget);
void IncreaseFocus();
void DecreaseFocus();
void ResetGUI();

// --------------------------------------------------------------------

void DrawFrameX(int x, int y, int w, int h, int line,
                const Color& backcol, const Color& framecol, float transp);
void DrawBonusExt(int y, std::size_t numraces, std::size_t num);
void DrawGUIBackground(float scale);
void DrawGUIFrame();

// --------------------------------------------------------------------

int AutoYPosN(int percent);
TArea AutoAreaN(int top_perc, int bott_perc, unsigned int w);

#endif
