/* --------------------------------------------------------------------
EXTREME TUXRACER

Per-course best-time persistence.
---------------------------------------------------------------------*/

#include "highscore.h"
#include "bh.h"
#include "course.h"
#include "game_ctrl.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>

namespace {

std::map<std::string, TCourseHighScore> scores;
bool scores_loaded = false;
const TCourse* cached_course = nullptr;
const TCourseHighScore* cached_score = nullptr;
const TCourseHighScore empty_score = {0.0, std::string(), 0};

std::string ScoreFile() {
	return param.save_dir + SEP "highscores.txt";
}

std::string CourseKey() {
	if (g_game.course == nullptr) return std::string();
	return g_game.course->group + "/" + g_game.course->dir;
}

void LoadScores() {
	if (scores_loaded) return;
	scores_loaded = true;

	std::ifstream file(ScoreFile());
	std::string line;
	while (std::getline(file, line)) {
		std::size_t separator = line.find('\t');
		if (separator == std::string::npos) continue;

		std::string key = line.substr(0, separator);
		const char* value_start = line.c_str() + separator + 1;
		char* end = nullptr;
		double value = std::strtod(value_start, &end);
		if (!key.empty() && end != value_start &&
		    std::isfinite(value) && value > 0.0) {
			std::string character_dir;
			// The third column was added later; two-column saves remain valid.
			if (*end == '\t') character_dir = end + 1;
			scores[key] = {value, character_dir, 1};
		}
	}
}

void SaveScores() {
	std::ofstream file(ScoreFile());
	if (!file) {
		Message("Unable to save high scores", ScoreFile());
		return;
	}

	file << std::fixed << std::setprecision(6);
	for (const auto& score : scores) {
		file << score.first << '\t' << score.second.time;
		if (!score.second.character_dir.empty())
			file << '\t' << score.second.character_dir;
		file << '\n';
	}
}

const TCourseHighScore& CurrentScore() {
	LoadScores();
	// The course pointer is stable throughout a race, avoiding a map lookup
	// and course-key allocation on every HUD frame.
	if (cached_course == g_game.course && cached_score != nullptr)
		return *cached_score;

	cached_course = g_game.course;
	const std::string key = CourseKey();
	auto score = scores.find(key);
	cached_score = score == scores.end() ? &empty_score : &score->second;
	return *cached_score;
}

} // namespace

const TCourseHighScore& GetCourseHighScoreRecord() {
	return CurrentScore();
}

double GetCourseHighScore() {
	return CurrentScore().time;
}

bool SubmitCourseHighScore(double time) {
	LoadScores();
	const std::string key = CourseKey();
	if (key.empty() || !std::isfinite(time) || time <= 0.0) return false;

	auto score = scores.find(key);
	if (score != scores.end() && time >= score->second.time) return false;

	const std::string character_dir =
		g_game.character == nullptr ? std::string() : g_game.character->dir;
	if (score == scores.end()) {
		score = scores.emplace(key, TCourseHighScore{time, character_dir, 1}).first;
	} else {
		score->second.time = time;
		score->second.character_dir = character_dir;
		score->second.revision++;
	}
	cached_course = g_game.course;
	cached_score = &score->second;
	SaveScores();
	return true;
}
