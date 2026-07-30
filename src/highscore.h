/* --------------------------------------------------------------------
EXTREME TUXRACER

Per-course best-time persistence.
---------------------------------------------------------------------*/

#ifndef HIGHSCORE_H
#define HIGHSCORE_H

#include <cstddef>
#include <string>

struct TCourseHighScore {
	double time;
	std::string character_dir;
	std::size_t revision;
};

const TCourseHighScore& GetCourseHighScoreRecord();
double GetCourseHighScore();
bool SubmitCourseHighScore(double time);

#endif
