#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class BingoNetEventType
{
	Connected,
	Disconnected,
	Message,
	Error
};

struct BingoNetEvent
{
	BingoNetEventType Type = BingoNetEventType::Error;
	int ClientId = 0;
	std::string Text = "";
	std::vector<uint8_t> Payload = {};
};

class BingoNet
{
public:
	BingoNet();
	~BingoNet();

	BingoNet(const BingoNet&) = delete;
	BingoNet& operator=(const BingoNet&) = delete;

	bool StartServer(uint16_t Port);
	bool StartClient(const std::string& Host, uint16_t Port);
	void Stop();

	bool IsActive() const;
	bool IsServer() const;
	bool IsConnected() const;
	int GetClientCount() const;

	void SendToServer(const std::vector<uint8_t>& Payload);
	void SendToClient(int ClientId, const std::vector<uint8_t>& Payload);
	void Broadcast(const std::vector<uint8_t>& Payload);
	void DisconnectClient(int ClientId);

	std::vector<BingoNetEvent> PollEvents();

private:
	struct Impl;
	std::unique_ptr<Impl> P;
};
