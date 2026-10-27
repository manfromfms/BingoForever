#pragma once

#include "../../IModule.h"
#include "BingoNet.h"
#include "BingoTypes.h"

#include <chrono>
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BingoModule : public IModule
{
private:
	enum class ConnectionMode
	{
		None,
		Host,
		Client
	};

	struct DownloadState
	{
		std::ofstream File = {};
		std::filesystem::path TempPath = {};
		uint64_t Received = 0;
		uint64_t Expected = 0;
	};

	struct NotificationSpan
	{
		std::string Text = "";
		ImVec4 Color = {};
	};

	struct Notification
	{
		std::vector<NotificationSpan> Spans = {};
		std::chrono::steady_clock::time_point Time = {};
	};

	BingoNet Net = {};
	ConnectionMode Mode = ConnectionMode::None;
	bool Connected = false;
	int LocalPlayerId = 0;

	std::string StatusMessage = "";
	std::string PlayerName = "Player";
	std::string HostIp = "127.0.0.1";
	std::string DisconnectReason = "";
	bool LoggedIn = false;
	std::string MapFolder = "";
	int Port = 42123;
	int BoardSize = 5;
	int TeamCount = 2;
	int MedalRule = 0;
	int MatchMinutes = 90;
	int GraceMinutes = 5;
	bool AllowNoLogin = true;
	float CellSize = 64.f;

	char HostIpBuffer[64] = {};
	char MapFolderBuffer[512] = {};

	BingoMatchState Match = {};
	std::vector<std::filesystem::path> MapPoolPaths = {};
	std::unordered_map<uint64_t, std::filesystem::path> MapFiles = {};
	std::unordered_set<uint64_t> AvailableMaps = {};
	std::unordered_set<uint64_t> RequestedMaps = {};
	std::unordered_set<uint64_t> FailedMaps = {};
	mutable std::filesystem::path DownloadFolderCache = {};
	std::unordered_map<uint64_t, DownloadState> Downloads = {};

	int PendingSquare = -1;
	int PendingOpenSquare = -1;
	std::string PendingFromUid = "";
	std::chrono::steady_clock::time_point PendingSince = {};
	int ActiveSquare = -1;
	std::string ActiveMapUid = "";
	uintptr_t LastChallenge = 0;
	TM::RaceState LastState = TM::Finished;
	bool FinishHandled = false;
	long LastRaceTime = 0;

	bool MatchTimerEnabled = false;
	int MatchDurationMs = 0;
	int GraceDurationMs = 0;
	std::chrono::steady_clock::time_point MatchStartTime = {};
	std::chrono::steady_clock::time_point GraceEndTime = {};
	std::chrono::steady_clock::time_point MatchEndTime = {};
	bool GraceEvaluated = false;
	bool GraceNoticeShown = false;
	std::array<bool, 4> TimeNoticesSent = {};

	BingoMatchState NotifiedMatch = {};
	std::deque<Notification> Notifications = {};

	int NextPlayerId = 1;

	void ProcessEvents();
	void HandleNetEvent(const BingoNetEvent& Event);
	void HandleMessage(int ClientId, const std::vector<uint8_t>& Payload);
	void HandleServerMessage(const std::vector<uint8_t>& Payload);
	void HandleClientMessage(int ClientId, const std::vector<uint8_t>& Payload);

	void StartHost();
	void StartClient();
	void StopConnection();

	void RescanMaps();
	void GenerateBoard();
	void SyncTeams();
	void StartMatch();
	void FinishMatch(int WinnerTeam, bool IsDraw);
	void ProcessHostTimers();
	void ProcessNotificationTimers();
	void ResetMatchNotices();
	void RefreshIdentity();

	void AddNotification(const std::string& Text, const ImVec4& Color);
	void AddNotification(const std::vector<NotificationSpan>& Spans);
	void DetectStateNotifications(const BingoMatchState& Old, const BingoMatchState& New);
	void CleanupNotifications();
	void RenderNotifications();

	void CheckFinish();

	void OpenSquare(int SquareIndex);
	bool LaunchMap(const std::filesystem::path& Path);
	void EnsureMaps();
	void HandleMapChunk(const std::vector<uint8_t>& Payload);
	void ServeMap(int ClientId, uint64_t Hash);

	void BroadcastState();
	void SendStateTo(int ClientId);
	std::vector<uint8_t> BuildStateMessage(uint8_t Type, int PlayerId) const;

	void AddClaim(int ClientId, int SquareIndex, int TimeMs, const std::string& MapUid);
	void BindSquareUid(int ClientId, int SquareIndex, const std::string& MapUid);

	int CountSquares(int TeamId) const;
	std::vector<int> FindLineTeams() const;
	bool IsLineTeam(int TeamId) const;
	void EvaluateAfterGrace();

	BingoPlayer* GetLocalPlayer();
	const BingoPlayer* GetLocalPlayer() const;
	BingoSquare* GetSquare(int Index);
	const BingoSquare* GetSquare(int Index) const;

	std::filesystem::path GetDocumentsFilePath() const;
	std::filesystem::path GetDownloadFolder() const;
	std::filesystem::path GetDefaultMapFolder() const;
	std::filesystem::path GetMapDownloadPath(uint64_t Hash) const;
	std::string GetProfileDisplayName() const;

	BingoMatchSettings GetConfig() const;
	void SetStatus(const std::string& Message);
	void RefreshBuffers();

public:
	BingoModule(TwinkTrackmania& Twinkie, TwinkLogs& Logger, const bool* UiRenderEnabled);
	~BingoModule();

	virtual void Render();
	virtual void RenderAnyways();
	virtual void RenderInactive();
	virtual void RenderSettings();
	virtual void RenderMenuItem();

	virtual void SettingsInit(SettingMgr& Settings);
	virtual void SettingsSave(SettingMgr& Settings);

	virtual bool IsDebug() { return false; }
	virtual bool HasSettings() { return true; }
};
