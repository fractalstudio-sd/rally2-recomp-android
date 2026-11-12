// Online leaderboard, host side: results handed over by the game hook (leaderboard_guest.c) are appended to
// <storage>/leaderboard_pending.jsonl; the Android launcher uploads that file in the background. GPLv3.
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

static std::mutex g_lbLock;
static std::string g_lbPath;

void LeaderboardSetDir(const std::string &dir) { std::lock_guard<std::mutex> l(g_lbLock); g_lbPath = dir + "/leaderboard_pending.jsonl"; }

extern "C" void lb_submit(const char *json)
{
	std::string line(json);
	if (!line.empty() && line.back() == '}') { line.pop_back(); line += ",\"ts\":" + std::to_string((long long)time(nullptr)) + "}"; }
	fprintf(stderr, "[leaderboard] %s\n", line.c_str());
	std::lock_guard<std::mutex> l(g_lbLock);
	if (g_lbPath.empty()) return;
	if (FILE *f = fopen(g_lbPath.c_str(), "a")) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
}
