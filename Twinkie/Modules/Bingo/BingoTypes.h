#pragma once

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

enum class BingoPhase : uint8_t
{
	Idle,
	Lobby,
	Running,
	Finished
};

enum class MedalRequirement : int32_t
{
	None,
	Bronze,
	Silver,
	Gold,
	Author
};

struct BingoMapInfo
{
	uint64_t Hash = 0;
	uint64_t Size = 0;
	std::string DisplayName = "";
	int BronzeMs = -1;
	int SilverMs = -1;
	int GoldMs = -1;
	int AuthorMs = -1;
};

struct BingoSquare
{
	BingoMapInfo Map = {};
	int OwnerTeam = -1;
	int RecordMs = INT_MAX;
	std::string RecordPlayer = "";
	std::string MapUid = "";
};

struct BingoTeam
{
	int Id = 0;
	std::string Name = "";
};

struct BingoPlayer
{
	int Id = 0;
	std::string Name = "";
	int TeamId = 0;
	bool LoggedIn = false;
};

struct BingoMatchSettings
{
	int BoardSize = 5;
	int TeamCount = 2;
	MedalRequirement MedalRule = MedalRequirement::None;
	int MatchMinutes = 90;
	int GraceMinutes = 5;
	bool AllowNoLogin = true;
};

struct BingoMatchState
{
	BingoPhase Phase = BingoPhase::Idle;
	BingoMatchSettings Settings = {};
	std::vector<BingoTeam> Teams = {};
	std::vector<BingoPlayer> Players = {};
	std::vector<BingoSquare> Board = {};
	int WinnerTeam = -1;
	bool IsDraw = false;
};
