#include "BingoNet.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

namespace
{
	const uint32_t MaxFrameSize = 2u * 1024u * 1024u;

	void WriteU32(std::vector<uint8_t>& Buffer, uint32_t Value)
	{
		Buffer.push_back((uint8_t)(Value & 0xFF));
		Buffer.push_back((uint8_t)((Value >> 8) & 0xFF));
		Buffer.push_back((uint8_t)((Value >> 16) & 0xFF));
		Buffer.push_back((uint8_t)((Value >> 24) & 0xFF));
	}

	uint32_t ReadU32(const uint8_t* Data)
	{
		return (uint32_t)Data[0] | ((uint32_t)Data[1] << 8) | ((uint32_t)Data[2] << 16) | ((uint32_t)Data[3] << 24);
	}

	void SetNonBlocking(SOCKET Socket)
	{
		u_long Mode = 1;
		ioctlsocket(Socket, FIONBIO, &Mode);
	}
}

struct BingoNet::Impl
{
	struct Client
	{
		SOCKET Socket = INVALID_SOCKET;
		int Id = 0;
		std::vector<uint8_t> In = {};
		std::vector<uint8_t> Out = {};
		bool Closing = false;
	};

	std::atomic<bool> Running = false;
	bool Server = false;
	bool WsaStarted = false;
	SOCKET ListenSocket = INVALID_SOCKET;
	std::thread Worker = {};
	int NextClientId = 1;
	std::unordered_map<int, std::unique_ptr<Client>> Clients = {};
	mutable std::mutex Mutex = {};
	std::vector<BingoNetEvent> Events = {};

	void QueueEventLocked(BingoNetEvent Event)
	{
		Events.push_back(std::move(Event));
	}

	void QueueMessageLocked(int ClientId, std::vector<uint8_t> Payload)
	{
		BingoNetEvent Event = {};
		Event.Type = BingoNetEventType::Message;
		Event.ClientId = ClientId;
		Event.Payload = std::move(Payload);
		QueueEventLocked(std::move(Event));
	}

	void CloseClientLocked(int ClientId, bool Notify)
	{
		auto It = Clients.find(ClientId);
		if (It == Clients.end()) return;

		if (It->second->Socket != INVALID_SOCKET)
		{
			closesocket(It->second->Socket);
		}

		if (Notify)
		{
			BingoNetEvent Event = {};
			Event.Type = BingoNetEventType::Disconnected;
			Event.ClientId = ClientId;
			QueueEventLocked(std::move(Event));
		}

		Clients.erase(It);
	}

	void QueueFrameLocked(Client& Target, const std::vector<uint8_t>& Payload)
	{
		WriteU32(Target.Out, (uint32_t)Payload.size());
		Target.Out.insert(Target.Out.end(), Payload.begin(), Payload.end());
	}

	void ProcessRead(Client& Source)
	{
		uint8_t Buffer[4096];
		while (true)
		{
			int Received = recv(Source.Socket, (char*)Buffer, sizeof(Buffer), 0);
			if (Received == 0)
			{
				Source.Closing = true;
				return;
			}
			if (Received == SOCKET_ERROR)
			{
				int Error = WSAGetLastError();
				if (Error == WSAEWOULDBLOCK) return;
				Source.Closing = true;
				return;
			}

			Source.In.insert(Source.In.end(), Buffer, Buffer + Received);

			while (Source.In.size() >= 4)
			{
				uint32_t Size = ReadU32(Source.In.data());
				if (Size > MaxFrameSize)
				{
					Source.Closing = true;
					return;
				}
				if (Size == 0)
				{
					Source.In.erase(Source.In.begin(), Source.In.begin() + 4);
					continue;
				}
				if (Source.In.size() < (size_t)Size + 4) break;

				std::vector<uint8_t> Payload(Source.In.begin() + 4, Source.In.begin() + 4 + Size);
				Source.In.erase(Source.In.begin(), Source.In.begin() + 4 + Size);
				QueueMessageLocked(Source.Id, std::move(Payload));
			}
		}
	}

	void ProcessWrite(Client& Target)
	{
		while (!Target.Out.empty())
		{
			int Sent = send(Target.Socket, (const char*)Target.Out.data(), (int)Target.Out.size(), 0);
			if (Sent == SOCKET_ERROR)
			{
				int Error = WSAGetLastError();
				if (Error == WSAEWOULDBLOCK) return;
				Target.Closing = true;
				return;
			}
			if (Sent <= 0) return;
			Target.Out.erase(Target.Out.begin(), Target.Out.begin() + Sent);
		}

		if (Target.Closing)
		{
			return;
		}
	}

	void AcceptClients()
	{
		while (true)
		{
			sockaddr_in Address = {};
			int Length = sizeof(Address);
			SOCKET Accepted = accept(ListenSocket, (sockaddr*)&Address, &Length);
			if (Accepted == INVALID_SOCKET)
			{
				int Error = WSAGetLastError();
				if (Error == WSAEWOULDBLOCK) return;
				return;
			}

			SetNonBlocking(Accepted);
			int NoDelay = 1;
			setsockopt(Accepted, IPPROTO_TCP, TCP_NODELAY, (const char*)&NoDelay, sizeof(NoDelay));

			auto NewClient = std::make_unique<Client>();
			NewClient->Socket = Accepted;
			NewClient->Id = NextClientId++;

			BingoNetEvent Event = {};
			Event.Type = BingoNetEventType::Connected;
			Event.ClientId = NewClient->Id;
			QueueEventLocked(std::move(Event));

			int Id = NewClient->Id;
			Clients[Id] = std::move(NewClient);
		}
	}

	void Run()
	{
		while (Running)
		{
			fd_set ReadSet;
			fd_set WriteSet;
			FD_ZERO(&ReadSet);
			FD_ZERO(&WriteSet);

			SOCKET MaxSocket = INVALID_SOCKET;

			{
				std::lock_guard<std::mutex> Lock(Mutex);

				if (Server && ListenSocket != INVALID_SOCKET)
				{
					FD_SET(ListenSocket, &ReadSet);
					MaxSocket = ListenSocket;
				}

				for (auto& Pair : Clients)
				{
					Client& Target = *Pair.second;
					if (Target.Socket == INVALID_SOCKET) continue;
					FD_SET(Target.Socket, &ReadSet);
					if (!Target.Out.empty()) FD_SET(Target.Socket, &WriteSet);
					if (Target.Socket > MaxSocket) MaxSocket = Target.Socket;
				}
			}

			if (MaxSocket == INVALID_SOCKET)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			timeval Timeout = {};
			Timeout.tv_sec = 0;
			Timeout.tv_usec = 100000;

			int Selected = select(0, &ReadSet, &WriteSet, nullptr, &Timeout);
			if (Selected == SOCKET_ERROR) continue;
			if (Selected <= 0) continue;

			std::vector<int> ToClose = {};

			{
				std::lock_guard<std::mutex> Lock(Mutex);

				if (Server && ListenSocket != INVALID_SOCKET && FD_ISSET(ListenSocket, &ReadSet))
				{
					AcceptClients();
				}

				for (auto& Pair : Clients)
				{
					Client& Target = *Pair.second;
					if (Target.Socket == INVALID_SOCKET) continue;
					if (FD_ISSET(Target.Socket, &ReadSet))
					{
						ProcessRead(Target);
					}
					if (Target.Socket != INVALID_SOCKET && FD_ISSET(Target.Socket, &WriteSet))
					{
						ProcessWrite(Target);
					}
					if (Target.Closing)
					{
						ToClose.push_back(Target.Id);
					}
				}
			}

			for (int ClientId : ToClose)
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				CloseClientLocked(ClientId, true);
			}
		}
	}

	void CloseAll()
	{
		if (ListenSocket != INVALID_SOCKET)
		{
			closesocket(ListenSocket);
			ListenSocket = INVALID_SOCKET;
		}

		for (auto& Pair : Clients)
		{
			if (Pair.second->Socket != INVALID_SOCKET)
			{
				closesocket(Pair.second->Socket);
			}
		}
		Clients.clear();
	}

	bool BeginWinsock()
	{
		if (WsaStarted) return true;

		WSADATA Data = {};
		if (WSAStartup(MAKEWORD(2, 2), &Data) != 0) return false;
		WsaStarted = true;
		return true;
	}

	void EndWinsock()
	{
		if (!WsaStarted) return;
		WSACleanup();
		WsaStarted = false;
	}
};

BingoNet::BingoNet()
{
	P = std::make_unique<Impl>();
}

BingoNet::~BingoNet()
{
	Stop();
}

bool BingoNet::StartServer(uint16_t Port)
{
	Stop();

	if (!P->BeginWinsock())
	{
		return false;
	}

	SOCKET Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (Listener == INVALID_SOCKET)
	{
		P->EndWinsock();
		return false;
	}

	int Reuse = 1;
	setsockopt(Listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&Reuse, sizeof(Reuse));

	sockaddr_in Address = {};
	Address.sin_family = AF_INET;
	Address.sin_addr.s_addr = htonl(INADDR_ANY);
	Address.sin_port = htons(Port);

	if (bind(Listener, (sockaddr*)&Address, sizeof(Address)) == SOCKET_ERROR)
	{
		closesocket(Listener);
		P->EndWinsock();
		return false;
	}

	if (listen(Listener, SOMAXCONN) == SOCKET_ERROR)
	{
		closesocket(Listener);
		P->EndWinsock();
		return false;
	}

	SetNonBlocking(Listener);

	P->Server = true;
	P->ListenSocket = Listener;
	P->Running = true;
	P->Worker = std::thread([this]() { P->Run(); });
	return true;
}

bool BingoNet::StartClient(const std::string& Host, uint16_t Port)
{
	Stop();

	if (!P->BeginWinsock())
	{
		return false;
	}

	addrinfo Hints = {};
	Hints.ai_family = AF_INET;
	Hints.ai_socktype = SOCK_STREAM;
	Hints.ai_protocol = IPPROTO_TCP;

	addrinfo* Result = nullptr;
	std::string PortString = std::to_string(Port);
	if (getaddrinfo(Host.c_str(), PortString.c_str(), &Hints, &Result) != 0 || !Result)
	{
		P->EndWinsock();
		return false;
	}

	SOCKET Socket = socket(Result->ai_family, Result->ai_socktype, Result->ai_protocol);
	if (Socket == INVALID_SOCKET)
	{
		freeaddrinfo(Result);
		P->EndWinsock();
		return false;
	}

	SetNonBlocking(Socket);

	int Connected = connect(Socket, Result->ai_addr, (int)Result->ai_addrlen);
	freeaddrinfo(Result);

	if (Connected == SOCKET_ERROR)
	{
		int Error = WSAGetLastError();
		if (Error != WSAEWOULDBLOCK && Error != WSAEINPROGRESS)
		{
			closesocket(Socket);
			P->EndWinsock();
			return false;
		}

		fd_set WriteSet;
		fd_set ErrorSet;
		FD_ZERO(&WriteSet);
		FD_ZERO(&ErrorSet);
		FD_SET(Socket, &WriteSet);
		FD_SET(Socket, &ErrorSet);

		timeval Timeout = {};
		Timeout.tv_sec = 5;
		Timeout.tv_usec = 0;

		int Selected = select(0, nullptr, &WriteSet, &ErrorSet, &Timeout);
		if (Selected <= 0 || FD_ISSET(Socket, &ErrorSet))
		{
			closesocket(Socket);
			P->EndWinsock();
			return false;
		}

		int SocketError = 0;
		int Length = sizeof(SocketError);
		getsockopt(Socket, SOL_SOCKET, SO_ERROR, (char*)&SocketError, &Length);
		if (SocketError != 0)
		{
			closesocket(Socket);
			P->EndWinsock();
			return false;
		}
	}

	int NoDelay = 1;
	setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&NoDelay, sizeof(NoDelay));

	auto Client = std::make_unique<Impl::Client>();
	Client->Socket = Socket;
	Client->Id = 0;

	{
		std::lock_guard<std::mutex> Lock(P->Mutex);
		P->Clients[0] = std::move(Client);
		BingoNetEvent Event = {};
		Event.Type = BingoNetEventType::Connected;
		Event.ClientId = 0;
		P->QueueEventLocked(std::move(Event));
	}

	P->Server = false;
	P->Running = true;
	P->Worker = std::thread([this]() { P->Run(); });
	return true;
}

void BingoNet::Stop()
{
	if (!P) return;

	if (P->Running)
	{
		P->Running = false;
		if (P->Worker.joinable())
		{
			P->Worker.join();
		}
	}

	{
		std::lock_guard<std::mutex> Lock(P->Mutex);
		P->CloseAll();
		P->Server = false;
	}

	P->EndWinsock();
}

bool BingoNet::IsActive() const
{
	return P->Running;
}

bool BingoNet::IsServer() const
{
	return P->Server;
}

bool BingoNet::IsConnected() const
{
	std::lock_guard<std::mutex> Lock(P->Mutex);
	return P->Clients.find(0) != P->Clients.end();
}

int BingoNet::GetClientCount() const
{
	std::lock_guard<std::mutex> Lock(P->Mutex);
	return (int)P->Clients.size();
}

void BingoNet::SendToServer(const std::vector<uint8_t>& Payload)
{
	SendToClient(0, Payload);
}

void BingoNet::SendToClient(int ClientId, const std::vector<uint8_t>& Payload)
{
	if (!P->Running) return;

	std::lock_guard<std::mutex> Lock(P->Mutex);
	auto It = P->Clients.find(ClientId);
	if (It == P->Clients.end()) return;
	if (It->second->Closing) return;
	P->QueueFrameLocked(*It->second, Payload);
}

void BingoNet::Broadcast(const std::vector<uint8_t>& Payload)
{
	if (!P->Running) return;

	std::lock_guard<std::mutex> Lock(P->Mutex);
	for (auto& Pair : P->Clients)
	{
		if (Pair.second->Closing) continue;
		P->QueueFrameLocked(*Pair.second, Payload);
	}
}

void BingoNet::DisconnectClient(int ClientId)
{
	std::lock_guard<std::mutex> Lock(P->Mutex);
	P->CloseClientLocked(ClientId, true);
}

std::vector<BingoNetEvent> BingoNet::PollEvents()
{
	std::lock_guard<std::mutex> Lock(P->Mutex);
	std::vector<BingoNetEvent> Result = {};
	Result.swap(P->Events);
	return Result;
}
