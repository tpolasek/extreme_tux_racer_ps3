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

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "states.h"
#include "ogl.h"
#include "winsys.h"
#include <csignal>

extern volatile sig_atomic_t g_sigint_received;

State::Manager State::manager(Winsys);

State::Manager::~Manager() {
	if (current)
		current->Exit();
}

void State::Manager::Run(State& entranceState) {
	current = &entranceState;
	current->Enter();
	while (!quit) {
		if (g_sigint_received) {
			quit = true;
			break;
		}
		PollEvent();
		if (next)
			EnterNextState();
		CallLoopFunction();
	}
	current->Exit();
	previous = current;
	current = nullptr;
}

void State::Manager::EnterNextState() {
	current->Exit();
	previous = current;
	current = next;
	next = nullptr;
	current->Enter();
}

void State::Manager::PollEvent() {
	sf::Event event;

	while (Winsys.PollEvent(event)) {
		if (!next) {
			switch (event.type) {
				case sf::Event::JoystickMoved: {
					float val = event.joystickMove.position / 100.f;
					current->Jaxis(event.joystickMove.axis == sf::Joystick::X ? 0 : 1, val);
					break;
				}
				case sf::Event::JoystickButtonPressed:
				case sf::Event::JoystickButtonReleased:
					current->Jbutt(event.joystickButton.button, event.type == sf::Event::JoystickButtonPressed);
					break;

				case sf::Event::Closed:
					quit = true;
					break;

				default:
					break;
			}
		}
	}
}

void State::Manager::CallLoopFunction() {
	check_gl_error();

	g_game.time_step = std::max(0.0001f, timer.getElapsedTime().asSeconds());
	timer.restart();
	current->Loop(g_game.time_step);
}
