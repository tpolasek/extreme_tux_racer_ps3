/* --------------------------------------------------------------------
EXTREME TUXRACER

Per-course best-time persistence.
---------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "highscore.h"
#include "bh.h"
#include "course.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>

namespace {

std::map<std::string, double> scores;
bool scores_loaded = false;

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
		char* end = nullptr;
		double value = std::strtod(line.c_str() + separator + 1, &end);
		if (!key.empty() && end != line.c_str() + separator + 1 &&
		    std::isfinite(value) && value > 0.0) {
			scores[key] = value;
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
		file << score.first << '\t' << score.second << '\n';
	}
}

} // namespace

double GetCourseHighScore() {
	LoadScores();
	const std::string key = CourseKey();
	auto score = scores.find(key);
	return score == scores.end() ? 0.0 : score->second;
}

bool SubmitCourseHighScore(double time) {
	LoadScores();
	const std::string key = CourseKey();
	if (key.empty() || !std::isfinite(time) || time <= 0.0) return false;

	auto score = scores.find(key);
	if (score != scores.end() && time >= score->second) return false;

	scores[key] = time;
	SaveScores();
	return true;
}
