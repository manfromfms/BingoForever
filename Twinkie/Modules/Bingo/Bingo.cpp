#include "Bingo.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <random>
#include <set>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
	const uint8_t MSG_HELLO = 1;
	const uint8_t MSG_WELCOME = 2;
	const uint8_t MSG_STATE = 3;
	const uint8_t MSG_SET_TEAM = 4;
	const uint8_t MSG_BIND_UID = 5;
	const uint8_t MSG_CLAIM = 6;
	const uint8_t MSG_MAP_REQUEST = 7;
	const uint8_t MSG_MAP_CHUNK = 8;
	const uint8_t MSG_ERROR = 9;

	const uint64_t FnvOffset = 14695981039346656037ULL;
	const uint64_t FnvPrime = 1099511628211ULL;
	const uint32_t MapChunkSize = 64u * 1024u;

	const char* MedalRuleNames[] = { "None", "Bronze", "Silver", "Gold", "Author" };

	const ImVec4 TeamColors[] = {
		ImVec4(0.86f, 0.25f, 0.25f, 1.f),
		ImVec4(0.25f, 0.45f, 0.95f, 1.f),
		ImVec4(0.25f, 0.75f, 0.35f, 1.f),
		ImVec4(0.95f, 0.75f, 0.20f, 1.f),
		ImVec4(0.70f, 0.35f, 0.85f, 1.f),
		ImVec4(0.95f, 0.50f, 0.20f, 1.f),
		ImVec4(0.20f, 0.75f, 0.80f, 1.f),
		ImVec4(0.90f, 0.40f, 0.65f, 1.f)
	};

	struct Writer
	{
		std::vector<uint8_t> Data = {};

		void U8(uint8_t Value)
		{
			Data.push_back(Value);
		}

		void U32(uint32_t Value)
		{
			for (int Idx = 0; Idx < 4; Idx++)
			{
				Data.push_back((uint8_t)((Value >> (Idx * 8)) & 0xFF));
			}
		}

		void I32(int32_t Value)
		{
			U32((uint32_t)Value);
		}

		void U64(uint64_t Value)
		{
			for (int Idx = 0; Idx < 8; Idx++)
			{
				Data.push_back((uint8_t)((Value >> (Idx * 8)) & 0xFF));
			}
		}

		void Str(const std::string& Value)
		{
			U32((uint32_t)Value.size());
			Data.insert(Data.end(), Value.begin(), Value.end());
		}

		void Bytes(const uint8_t* Value, size_t Size)
		{
			U32((uint32_t)Size);
			Data.insert(Data.end(), Value, Value + Size);
		}
	};

	struct Reader
	{
		const uint8_t* Data = nullptr;
		size_t Size = 0;
		size_t Pos = 0;
		bool Ok = true;

		Reader(const uint8_t* InData, size_t InSize)
		{
			Data = InData;
			Size = InSize;
		}

		bool Ensure(size_t Count)
		{
			if (!Ok || Count > Size - Pos)
			{
				Ok = false;
				return false;
			}
			return true;
		}

		uint8_t U8()
		{
			if (!Ensure(1)) return 0;
			return Data[Pos++];
		}

		uint32_t U32()
		{
			if (!Ensure(4)) return 0;
			uint32_t Value = (uint32_t)Data[Pos] |
				((uint32_t)Data[Pos + 1] << 8) |
				((uint32_t)Data[Pos + 2] << 16) |
				((uint32_t)Data[Pos + 3] << 24);
			Pos += 4;
			return Value;
		}

		int32_t I32()
		{
			return (int32_t)U32();
		}

		uint64_t U64()
		{
			if (!Ensure(8)) return 0;
			uint64_t Value = 0;
			for (int Idx = 0; Idx < 8; Idx++)
			{
				Value |= (uint64_t)Data[Pos + Idx] << (Idx * 8);
			}
			Pos += 8;
			return Value;
		}

		std::string Str()
		{
			uint32_t Length = U32();
			if (!Ok || !Ensure(Length)) return "";
			std::string Value((const char*)Data + Pos, Length);
			Pos += Length;
			return Value;
		}

		std::vector<uint8_t> Bytes()
		{
			uint32_t Length = U32();
			if (!Ok || !Ensure(Length)) return {};
			std::vector<uint8_t> Value(Data + Pos, Data + Pos + Length);
			Pos += Length;
			return Value;
		}
	};

	std::string ToLower(std::string Value)
	{
		std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character)
		{
			return (char)std::tolower(Character);
		});
		return Value;
	}

	std::string WideToUtf8(const std::wstring& Value)
	{
		if (Value.empty()) return "";

		int SizeNeeded = WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), (int)Value.size(), nullptr, 0, nullptr, nullptr);
		if (SizeNeeded <= 0) return "";

		std::string Result((size_t)SizeNeeded, '\0');
		WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), (int)Value.size(), Result.data(), SizeNeeded, nullptr, nullptr);
		return Result;
	}

	bool IsChallengeFile(const std::filesystem::path& Path)
	{
		std::string Name = ToLower(WideToUtf8(Path.filename().wstring()));
		return Name.size() > 14 && Name.ends_with(".challenge.gbx");
	}

	std::string MapDisplayName(const std::filesystem::path& Path)
	{
		std::string Name = WideToUtf8(Path.filename().wstring());
		if (Name.size() > 14) Name.resize(Name.size() - 14);
		return Name;
	}

	bool LooksLikeDisplayName(const std::string& Value)
	{
		if (Value.empty() || Value.size() > 64) return false;

		bool HasText = false;
		for (unsigned char Ch : Value)
		{
			if (Ch < 0x20 || Ch == 0x7f) return false;
			if (Ch >= 0x80 || std::isalnum(Ch)) HasText = true;
		}

		return HasText;
	}

	std::string ReadProfileNameFromFile(const std::filesystem::path& Path)
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File) return {};

		std::vector<unsigned char> Data(1024);
		File.read((char*)Data.data(), (std::streamsize)Data.size());
		Data.resize((size_t)File.gcount());

		for (size_t Idx = 0; Idx + 4 <= Data.size(); Idx++)
		{
			uint32_t Length = (uint32_t)Data[Idx] | ((uint32_t)Data[Idx + 1] << 8) | ((uint32_t)Data[Idx + 2] << 16) | ((uint32_t)Data[Idx + 3] << 24);
			if (Length == 0 || Length > 64) continue;
			if (Idx + 4 + Length > Data.size()) continue;

			std::string Value((const char*)Data.data() + Idx + 4, Length);
			if (LooksLikeDisplayName(Value)) return Value;
		}

		return {};
	}

	bool ContainsProfileName(const std::vector<unsigned char>& Data, const std::string& Name)
	{
		if (Name.empty()) return false;

		auto IsNameContinuation = [](unsigned char Ch)
		{
			return std::isdigit(Ch) || Ch == '_' || Ch == '-';
		};

		auto It = Data.begin();
		while (It != Data.end())
		{
			It = std::search(It, Data.end(), Name.begin(), Name.end());
			if (It == Data.end()) break;

			size_t Pos = (size_t)(It - Data.begin());
			size_t End = Pos + Name.size();
			bool AfterOk = End >= Data.size() || !IsNameContinuation(Data[End]);
			if (AfterOk) return true;

			It++;
		}

		return false;
	}

	bool HashFile(const std::filesystem::path& Path, uint64_t& Hash, uint64_t& Size)
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File) return false;

		Hash = FnvOffset;
		Size = 0;

		char Buffer[65536];
		while (File)
		{
			File.read(Buffer, sizeof(Buffer));
			std::streamsize Count = File.gcount();
			for (std::streamsize Idx = 0; Idx < Count; Idx++)
			{
				Hash ^= (uint8_t)Buffer[Idx];
				Hash *= FnvPrime;
			}
			Size += (uint64_t)Count;
		}

		return Size > 0;
	}

	std::string HashToHex(uint64_t Hash)
	{
		std::ostringstream Stream;
		Stream << std::hex << std::setw(16) << std::setfill('0') << Hash;
		return Stream.str();
	}

	uint16_t ReadU16(const uint8_t* Data)
	{
		return (uint16_t)Data[0] | ((uint16_t)Data[1] << 8);
	}

	uint32_t ReadU32(const uint8_t* Data)
	{
		return (uint32_t)Data[0] | ((uint32_t)Data[1] << 8) | ((uint32_t)Data[2] << 16) | ((uint32_t)Data[3] << 24);
	}

	bool Lzo1xDecompress(const uint8_t* In, size_t InLen, uint8_t* Out, size_t OutLen)
	{
		size_t Ip = 0;
		size_t Op = 0;
		size_t T = 0;
		size_t Match = 0;

		auto NeedIp = [&](size_t Count) { return Ip + Count <= InLen; };
		auto NeedOp = [&](size_t Count) { return Op + Count <= OutLen; };

		if (!NeedIp(1)) return false;
		T = In[Ip];
		if (T > 17)
		{
			Ip++;
			T -= 17;
			if (T < 4) goto match_next;
			if (!NeedOp(T) || !NeedIp(T)) return false;
			for (size_t Idx = 0; Idx < T; Idx++) Out[Op++] = In[Ip++];
			goto first_literal_run;
		}

		for (;;)
		{
			if (!NeedIp(3)) return false;
			T = In[Ip++];
			if (T >= 16) goto match;
			if (T == 0)
			{
				while (In[Ip] == 0) { T += 255; Ip++; if (!NeedIp(1)) return false; }
				T += 15 + In[Ip++];
			}
			{
				size_t Count = T + 3;
				if (!NeedOp(Count) || !NeedIp(Count)) return false;
				for (size_t Idx = 0; Idx < Count; Idx++) Out[Op++] = In[Ip++];
			}

		first_literal_run:
			if (!NeedIp(1)) return false;
			T = In[Ip++];
			if (T >= 16) goto match;
			Match = Op - (1 + 0x800);
			Match -= T >> 2;
			if (!NeedIp(1)) return false;
			Match -= (size_t)In[Ip++] << 2;
			if (Match >= Op) return false;
			if (!NeedOp(3)) return false;
			Out[Op++] = Out[Match++];
			Out[Op++] = Out[Match++];
			Out[Op++] = Out[Match];
			goto match_done;

			for (;;)
			{
			match:
				if (T >= 64)
				{
					Match = Op - 1;
					Match -= (T >> 2) & 7;
					if (!NeedIp(1)) return false;
					Match -= (size_t)In[Ip++] << 3;
					T = (T >> 5) - 1;
				}
				else if (T >= 32)
				{
					T &= 31;
					if (T == 0)
					{
						while (In[Ip] == 0) { T += 255; Ip++; if (!NeedIp(1)) return false; }
						T += 31 + In[Ip++];
					}
					if (!NeedIp(2)) return false;
					Match = Op - 1;
					Match -= ((size_t)In[Ip] >> 2) + ((size_t)In[Ip + 1] << 6);
					Ip += 2;
				}
				else if (T >= 16)
				{
					Match = Op;
					Match -= (T & 8) << 11;
					T &= 7;
					if (T == 0)
					{
						while (In[Ip] == 0) { T += 255; Ip++; if (!NeedIp(1)) return false; }
						T += 7 + In[Ip++];
					}
					if (!NeedIp(2)) return false;
					Match -= ((size_t)In[Ip] >> 2) + ((size_t)In[Ip + 1] << 6);
					Ip += 2;
					if (Match == Op) goto eof_found;
					Match -= 0x4000;
				}
				else
				{
					Match = Op - 1;
					Match -= T >> 2;
					if (!NeedIp(1)) return false;
					Match -= (size_t)In[Ip++] << 2;
					if (Match >= Op) return false;
					if (!NeedOp(2)) return false;
					Out[Op++] = Out[Match++];
					Out[Op++] = Out[Match];
					goto match_done;
				}

			copy_match:
				if (Match >= Op) return false;
				if (!NeedOp(T + 2)) return false;
				for (size_t Idx = 0; Idx < T + 2; Idx++) Out[Op++] = Out[Match++];

			match_done:
				if (!NeedIp(2)) return false;
				T = In[Ip - 2] & 3;
				if (T == 0) break;

			match_next:
				if (!NeedOp(T) || !NeedIp(T + 3)) return false;
				for (size_t Idx = 0; Idx < T; Idx++) Out[Op++] = In[Ip++];
				T = In[Ip++];
			}
		}

	eof_found:
		return Op == OutLen;
	}

	bool ReadMedalTimes(const std::filesystem::path& Path, BingoMapInfo& Info)
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File) return false;

		std::vector<uint8_t> Data((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
		if (Data.size() < 32 || std::memcmp(Data.data(), "GBX", 3) != 0) return false;

		uint16_t Version = ReadU16(&Data[3]);
		size_t Offset = 5 + 3;
		if (Version >= 4) Offset += 1;
		if (Offset + 8 > Data.size()) return false;

		Offset += 4;
		if (Version >= 6)
		{
			uint32_t UserDataSize = ReadU32(&Data[Offset]);
			Offset += 4;
			if (Offset + UserDataSize > Data.size()) return false;
			Offset += UserDataSize;
		}

		if (Offset + 8 > Data.size()) return false;
		Offset += 4;
		uint32_t ExternalNodes = ReadU32(&Data[Offset]);
		Offset += 4;
		if (ExternalNodes != 0) return false;
		if (Offset + 8 > Data.size()) return false;

		uint32_t DataSize = ReadU32(&Data[Offset]);
		uint32_t CompressedSize = ReadU32(&Data[Offset + 4]);
		Offset += 8;
		if (DataSize == 0 || DataSize > 64u * 1024u * 1024u) return false;
		if (CompressedSize == 0 || Offset + CompressedSize > Data.size()) return false;

		std::vector<uint8_t> Body(DataSize);
		if (!Lzo1xDecompress(Data.data() + Offset, CompressedSize, Body.data(), Body.size())) return false;

		for (size_t Idx = 0; Idx + 20 <= Body.size(); Idx++)
		{
			uint32_t ChunkId = ReadU32(&Body[Idx]);
			if (ChunkId != 0x0305B004 && ChunkId != 0x2400C004) continue;

			int Bronze = (int)ReadU32(&Body[Idx + 4]);
			int Silver = (int)ReadU32(&Body[Idx + 8]);
			int Gold = (int)ReadU32(&Body[Idx + 12]);
			int Author = (int)ReadU32(&Body[Idx + 16]);

			if (Author <= 0 || Gold < Author || Silver < Gold || Bronze < Silver) continue;
			if (Bronze > 24 * 60 * 60 * 1000) continue;

			Info.BronzeMs = Bronze;
			Info.SilverMs = Silver;
			Info.GoldMs = Gold;
			Info.AuthorMs = Author;
			return true;
		}

		return false;
	}

	std::string SquareLabel(int Index, int BoardSize)
	{
		if (BoardSize <= 0) return "?";
		int Row = Index / BoardSize;
		int Col = Index % BoardSize;
		return std::string(1, (char)('A' + Col)) + std::to_string(Row + 1);
	}

	ImVec4 NotificationJoinColor() { return ImVec4(0.45f, 0.85f, 0.45f, 1.f); }
	ImVec4 NotificationTeamColor() { return ImVec4(0.45f, 0.70f, 1.00f, 1.f); }
	ImVec4 NotificationWinColor() { return ImVec4(1.00f, 0.80f, 0.30f, 1.f); }
	ImVec4 NotificationGraceColor() { return ImVec4(1.00f, 0.60f, 0.25f, 1.f); }
	ImVec4 NotificationTimeColor() { return ImVec4(1.00f, 0.90f, 0.40f, 1.f); }
	ImVec4 NotificationClaimColor() { return ImVec4(0.90f, 0.90f, 0.90f, 1.f); }
	ImVec4 NotificationReclaimColor() { return ImVec4(1.00f, 0.55f, 0.85f, 1.f); }

	const int TimeNoticeThresholds[4] = { 30, 10, 5, 1 };

	ImVec4 Lighten(const ImVec4& Color, float Amount)
	{
		return ImVec4(
			std::min(1.f, Color.x + Amount),
			std::min(1.f, Color.y + Amount),
			std::min(1.f, Color.z + Amount),
			Color.w);
	}

	ImVec4 Darken(const ImVec4& Color, float Amount)
	{
		return ImVec4(
			std::max(0.f, Color.x - Amount),
			std::max(0.f, Color.y - Amount),
			std::max(0.f, Color.z - Amount),
			Color.w);
	}

	ImVec4 TeamColor(int TeamId)
	{
		if (TeamId < 0) return ImVec4(0.15f, 0.15f, 0.18f, 1.f);
		return TeamColors[TeamId % (int)(sizeof(TeamColors) / sizeof(TeamColors[0]))];
	}

	ImVec4 TextColorFor(const ImVec4& Background)
	{
		float Luminance = Background.x * 0.299f + Background.y * 0.587f + Background.z * 0.114f;
		return Luminance > 0.55f ? ImVec4(0.f, 0.f, 0.f, 1.f) : ImVec4(1.f, 1.f, 1.f, 1.f);
	}

	bool MeetsMedalRule(TwinkTrackmania* Twinkie, int TimeMs, MedalRequirement Rule)
	{
		if (Rule == MedalRequirement::None) return true;
		if (Twinkie->ChallengeUsesScore()) return false;

		ChallengeInfo Info = Twinkie->GetChallengeInfo();
		unsigned int Threshold = Info.AuthorTime;

		switch (Rule)
		{
		case MedalRequirement::Bronze:
			Threshold = Info.BronzeTime;
			break;
		case MedalRequirement::Silver:
			Threshold = Info.SilverTime;
			break;
		case MedalRequirement::Gold:
			Threshold = Info.GoldTime;
			break;
		case MedalRequirement::Author:
			Threshold = Info.AuthorTime;
			break;
		default:
			return true;
		}

		if (Threshold == MAXDWORD) return false;
		return TimeMs <= (int)Threshold;
	}

	int CountLinesForTeam(const BingoMatchState& Match, int TeamId)
	{
		int Size = Match.Settings.BoardSize;
		if (Size <= 0 || Match.Board.size() < (size_t)(Size * Size)) return 0;

		int Count = 0;

		for (int Row = 0; Row < Size; Row++)
		{
			bool Full = true;
			for (int Col = 0; Col < Size; Col++)
			{
				if (Match.Board[Row * Size + Col].OwnerTeam != TeamId)
				{
					Full = false;
					break;
				}
			}
			if (Full) Count++;
		}

		for (int Col = 0; Col < Size; Col++)
		{
			bool Full = true;
			for (int Row = 0; Row < Size; Row++)
			{
				if (Match.Board[Row * Size + Col].OwnerTeam != TeamId)
				{
					Full = false;
					break;
				}
			}
			if (Full) Count++;
		}

		bool Diagonal = true;
		for (int Idx = 0; Idx < Size; Idx++)
		{
			if (Match.Board[Idx * Size + Idx].OwnerTeam != TeamId)
			{
				Diagonal = false;
				break;
			}
		}
		if (Diagonal) Count++;

		Diagonal = true;
		for (int Idx = 0; Idx < Size; Idx++)
		{
			if (Match.Board[Idx * Size + (Size - 1 - Idx)].OwnerTeam != TeamId)
			{
				Diagonal = false;
				break;
			}
		}
		if (Diagonal) Count++;

		return Count;
	}

	void SerializeState(Writer& Out, const BingoMatchState& Match)
	{
		Out.U8((uint8_t)Match.Phase);
		Out.I32(Match.Settings.BoardSize);
		Out.I32(Match.Settings.TeamCount);
		Out.I32((int32_t)Match.Settings.MedalRule);
		Out.I32(Match.Settings.MatchMinutes);
		Out.I32(Match.Settings.GraceMinutes);
		Out.U8(Match.Settings.AllowNoLogin ? 1 : 0);

		Out.U32((uint32_t)Match.Teams.size());
		for (const BingoTeam& Team : Match.Teams)
		{
			Out.I32(Team.Id);
			Out.Str(Team.Name);
		}

		Out.U32((uint32_t)Match.Players.size());
		for (const BingoPlayer& Player : Match.Players)
		{
			Out.I32(Player.Id);
			Out.Str(Player.Name);
			Out.I32(Player.TeamId);
			Out.U8(Player.LoggedIn ? 1 : 0);
		}

		Out.U32((uint32_t)Match.Board.size());
		for (const BingoSquare& Square : Match.Board)
		{
			Out.U64(Square.Map.Hash);
			Out.U64(Square.Map.Size);
			Out.Str(Square.Map.DisplayName);
			Out.I32(Square.Map.BronzeMs);
			Out.I32(Square.Map.SilverMs);
			Out.I32(Square.Map.GoldMs);
			Out.I32(Square.Map.AuthorMs);
			Out.I32(Square.OwnerTeam);
			Out.I32(Square.RecordMs);
			Out.Str(Square.RecordPlayer);
			Out.Str(Square.MapUid);
		}

		Out.I32(Match.WinnerTeam);
		Out.U8(Match.IsDraw ? 1 : 0);
	}

	bool DeserializeState(Reader& In, BingoMatchState& Match)
	{
		Match = {};

		uint8_t Phase = In.U8();
		if (Phase > (uint8_t)BingoPhase::Finished) In.Ok = false;

		Match.Phase = (BingoPhase)Phase;
		Match.Settings.BoardSize = In.I32();
		Match.Settings.TeamCount = In.I32();
		Match.Settings.MedalRule = (MedalRequirement)In.I32();
		Match.Settings.MatchMinutes = In.I32();
		Match.Settings.GraceMinutes = In.I32();
		Match.Settings.AllowNoLogin = In.U8() != 0;

		if (Match.Settings.BoardSize < 3 || Match.Settings.BoardSize > 7) In.Ok = false;
		if (Match.Settings.TeamCount < 2 || Match.Settings.TeamCount > 8) In.Ok = false;
		if ((int)Match.Settings.MedalRule < 0 || (int)Match.Settings.MedalRule > 4) In.Ok = false;

		uint32_t TeamCount = In.U32();
		if (TeamCount > 64) In.Ok = false;
		for (uint32_t Idx = 0; Idx < TeamCount && In.Ok; Idx++)
		{
			BingoTeam Team = {};
			Team.Id = In.I32();
			Team.Name = In.Str();
			Match.Teams.push_back(Team);
		}

		uint32_t PlayerCount = In.U32();
		if (PlayerCount > 256) In.Ok = false;
		for (uint32_t Idx = 0; Idx < PlayerCount && In.Ok; Idx++)
		{
			BingoPlayer Player = {};
			Player.Id = In.I32();
			Player.Name = In.Str();
			Player.TeamId = In.I32();
			Player.LoggedIn = In.U8() != 0;
			Match.Players.push_back(Player);
		}

		uint32_t BoardCount = In.U32();
		if (BoardCount > 128) In.Ok = false;
		for (uint32_t Idx = 0; Idx < BoardCount && In.Ok; Idx++)
		{
			BingoSquare Square = {};
			Square.Map.Hash = In.U64();
			Square.Map.Size = In.U64();
			Square.Map.DisplayName = In.Str();
			Square.Map.BronzeMs = In.I32();
			Square.Map.SilverMs = In.I32();
			Square.Map.GoldMs = In.I32();
			Square.Map.AuthorMs = In.I32();
			Square.OwnerTeam = In.I32();
			Square.RecordMs = In.I32();
			Square.RecordPlayer = In.Str();
			Square.MapUid = In.Str();
			Match.Board.push_back(Square);
		}

		Match.WinnerTeam = In.I32();
		Match.IsDraw = In.U8() != 0;

		return In.Ok;
	}

	bool BrowseForFolder(std::string& OutPath)
	{
		HRESULT ComResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		BROWSEINFOW Info = {};
		Info.lpszTitle = L"Select Bingo map folder";
		Info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

		LPITEMIDLIST Item = SHBrowseForFolderW(&Info);
		if (!Item)
		{
			if (SUCCEEDED(ComResult)) CoUninitialize();
			return false;
		}

		wchar_t Path[MAX_PATH] = {};
		bool Success = SHGetPathFromIDListW(Item, Path);
		CoTaskMemFree(Item);

		if (SUCCEEDED(ComResult)) CoUninitialize();
		if (!Success) return false;

		OutPath = WideToUtf8(std::wstring(Path));
		return true;
	}
}

BingoModule::BingoModule(TwinkTrackmania& Twinkie, TwinkLogs& Logger, const bool* UiRenderEnabled)
{
	this->UiRenderEnabled = UiRenderEnabled;
	this->Twinkie = &Twinkie;
	this->Logger = &Logger;
	this->Name = "Bingo";
	this->FancyName = "Bingo";
}

BingoModule::~BingoModule()
{
	StopConnection();
}

void BingoModule::Render()
{
}

void BingoModule::RenderMenuItem()
{
	using namespace ImGui;

	if (MenuItem(ICON_FK_TH " Bingo", "", Enabled))
	{
		Enabled = !Enabled;
	}
}

void BingoModule::SettingsInit(SettingMgr& Settings)
{
	Tab& Bingo = Settings["Bingo"];

	Bingo["Host IP"].GetAsString(&HostIp);
	Bingo["Port"].GetAsInt(&Port);
	Bingo["Map folder"].GetAsString(&MapFolder);
	Bingo["Board size"].GetAsInt(&BoardSize);
	Bingo["Team count"].GetAsInt(&TeamCount);
	Bingo["Medal rule"].GetAsInt(&MedalRule);
	Bingo["Match minutes"].GetAsInt(&MatchMinutes);
	Bingo["Grace minutes"].GetAsInt(&GraceMinutes);
	Bingo["Allow no login"].GetAsBool(&AllowNoLogin);
	Bingo["Cell size"].GetAsFloat(&CellSize);

	BoardSize = std::clamp(BoardSize, 3, 7);
	TeamCount = std::clamp(TeamCount, 2, 8);
	MedalRule = std::clamp(MedalRule, 0, 4);
	MatchMinutes = std::clamp(MatchMinutes, 0, 1440);
	GraceMinutes = std::clamp(GraceMinutes, 0, 120);
	Port = std::clamp(Port, 1, 65535);
	CellSize = std::clamp(CellSize, 32.f, 160.f);

	if (HostIp.empty()) HostIp = "127.0.0.1";

	RefreshBuffers();
}

void BingoModule::SettingsSave(SettingMgr& Settings)
{
	Tab& Bingo = Settings["Bingo"];

	Bingo["Host IP"].Set(HostIp);
	Bingo["Port"].Set(Port);
	Bingo["Map folder"].Set(MapFolder);
	Bingo["Board size"].Set(BoardSize);
	Bingo["Team count"].Set(TeamCount);
	Bingo["Medal rule"].Set(MedalRule);
	Bingo["Match minutes"].Set(MatchMinutes);
	Bingo["Grace minutes"].Set(GraceMinutes);
	Bingo["Allow no login"].Set(AllowNoLogin);
	Bingo["Cell size"].Set(CellSize);
}

BingoMatchSettings BingoModule::GetConfig() const
{
	BingoMatchSettings Settings = {};
	Settings.BoardSize = std::clamp(BoardSize, 3, 7);
	Settings.TeamCount = std::clamp(TeamCount, 2, 8);
	Settings.MedalRule = (MedalRequirement)std::clamp(MedalRule, 0, 4);
	Settings.MatchMinutes = std::clamp(MatchMinutes, 0, 1440);
	Settings.GraceMinutes = std::clamp(GraceMinutes, 0, 120);
	Settings.AllowNoLogin = AllowNoLogin;
	return Settings;
}

void BingoModule::SetStatus(const std::string& Message)
{
	StatusMessage = Message;
}

void BingoModule::RefreshBuffers()
{
	std::snprintf(HostIpBuffer, sizeof(HostIpBuffer), "%s", HostIp.c_str());
	std::snprintf(MapFolderBuffer, sizeof(MapFolderBuffer), "%s", MapFolder.c_str());
}

void BingoModule::RefreshIdentity()
{
	std::string ProfileName = GetProfileDisplayName();
	if (ProfileName.empty()) ProfileName = Twinkie->GetPlayerName();
	if (ProfileName.empty()) ProfileName = "Player";
	if (ProfileName.size() > 40) ProfileName.resize(40);
	PlayerName = ProfileName;
	LoggedIn = Twinkie->IsLoggedIn();
	Logger->PrintInternalArgs("Bingo identity: {} (Nadeo login: {})", PlayerName, LoggedIn ? "yes" : "no");
}

std::filesystem::path BingoModule::GetDocumentsFilePath() const
{
	std::string Documents = GetDocumentsFolder();
	if (Documents.empty()) return {};
	return std::filesystem::path(Twinkie->UTF8ToWString(Documents)) / L"TrackMania";
}

std::filesystem::path BingoModule::GetDownloadFolder() const
{
	if (!DownloadFolderCache.empty()) return DownloadFolderCache;

	std::filesystem::path Documents = GetDocumentsFilePath();
	if (Documents.empty()) return {};
	std::filesystem::path Folder = Documents / L"Tracks" / L"Challenges" / L"Downloaded" / L"BingoForever";
	std::error_code Error;
	std::filesystem::create_directories(Folder, Error);
	DownloadFolderCache = Folder;
	return Folder;
}

std::filesystem::path BingoModule::GetDefaultMapFolder() const
{
	std::filesystem::path Documents = GetDocumentsFilePath();
	if (Documents.empty()) return {};
	return Documents / L"Tracks" / L"Challenges" / L"Downloaded";
}

std::string BingoModule::GetProfileDisplayName() const
{
	std::filesystem::path Documents = GetDocumentsFilePath();
	if (Documents.empty()) return {};

	std::filesystem::path ProfilesFolder = Documents / L"Profiles";
	std::error_code Error;
	if (!std::filesystem::exists(ProfilesFolder, Error)) return {};

	std::vector<std::filesystem::path> Profiles = {};
	for (const auto& Entry : std::filesystem::directory_iterator(ProfilesFolder, Error))
	{
		if (Error) break;
		if (!Entry.is_regular_file(Error)) continue;

		std::wstring Name = Entry.path().filename().wstring();
		if (Name.size() > 12 && Name.ends_with(L".Profile.Gbx")) Profiles.push_back(Entry.path());
	}

	if (Profiles.empty()) return {};

	std::sort(Profiles.begin(), Profiles.end(), [](const std::filesystem::path& Left, const std::filesystem::path& Right)
	{
		return Left.filename().wstring().size() > Right.filename().wstring().size();
	});

	std::vector<unsigned char> Config = {};
	{
		std::ifstream File(Documents / L"Config" / L"Default.SystemConfig.Gbx", std::ios::binary);
		if (File)
		{
			Config.assign(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
		}
	}

	auto BaseName = [](const std::filesystem::path& Path)
	{
		std::string Name = Path.filename().string();
		if (Name.size() > 12) Name.resize(Name.size() - 12);
		return Name;
	};

	const std::filesystem::path* Active = nullptr;
	if (!Config.empty())
	{
		for (const std::filesystem::path& Profile : Profiles)
		{
			if (ContainsProfileName(Config, BaseName(Profile)))
			{
				Active = &Profile;
				break;
			}
		}
	}

	if (!Active)
	{
		std::filesystem::file_time_type Newest = {};
		for (const std::filesystem::path& Profile : Profiles)
		{
			std::error_code TimeError;
			auto Time = std::filesystem::last_write_time(Profile, TimeError);
			if (TimeError) continue;
			if (!Active || Time > Newest)
			{
				Active = &Profile;
				Newest = Time;
			}
		}
	}

	if (Active)
	{
		std::string Name = ReadProfileNameFromFile(*Active);
		if (!Name.empty()) return Name;
	}

	for (const std::filesystem::path& Profile : Profiles)
	{
		std::string Name = ReadProfileNameFromFile(Profile);
		if (!Name.empty()) return Name;
	}

	return {};
}

std::filesystem::path BingoModule::GetMapDownloadPath(uint64_t Hash) const
{
	std::filesystem::path Folder = GetDownloadFolder();
	if (Folder.empty()) return {};
	return Folder / (HashToHex(Hash) + ".Challenge.Gbx");
}

BingoPlayer* BingoModule::GetLocalPlayer()
{
	for (BingoPlayer& Player : Match.Players)
	{
		if (Player.Id == LocalPlayerId) return &Player;
	}
	return nullptr;
}

const BingoPlayer* BingoModule::GetLocalPlayer() const
{
	for (const BingoPlayer& Player : Match.Players)
	{
		if (Player.Id == LocalPlayerId) return &Player;
	}
	return nullptr;
}

BingoSquare* BingoModule::GetSquare(int Index)
{
	if (Index < 0 || Index >= (int)Match.Board.size()) return nullptr;
	return &Match.Board[Index];
}

const BingoSquare* BingoModule::GetSquare(int Index) const
{
	if (Index < 0 || Index >= (int)Match.Board.size()) return nullptr;
	return &Match.Board[Index];
}

void BingoModule::ProcessEvents()
{
	for (BingoNetEvent& Event : Net.PollEvents())
	{
		HandleNetEvent(Event);
	}
}

void BingoModule::HandleNetEvent(const BingoNetEvent& Event)
{
	switch (Event.Type)
	{
	case BingoNetEventType::Connected:
	{
		if (Mode == ConnectionMode::Client)
		{
			Writer Out;
			Out.U8(MSG_HELLO);
			Out.Str(PlayerName);
			Out.I32(0);
			Out.U8(LoggedIn ? 1 : 0);
			Net.SendToServer(Out.Data);
			SetStatus("Connected, waiting for host");
		}
		break;
	}
	case BingoNetEventType::Disconnected:
	{
		if (Mode == ConnectionMode::Client)
		{
			std::string Reason = DisconnectReason;
			DisconnectReason.clear();
			StopConnection();
			SetStatus(Reason.empty() ? "Disconnected from host" : Reason);
		}
		else if (Mode == ConnectionMode::Host && Event.ClientId != 0)
		{
			Match.Players.erase(std::remove_if(Match.Players.begin(), Match.Players.end(), [&](const BingoPlayer& Player)
			{
				return Player.Id == Event.ClientId;
			}), Match.Players.end());
			BroadcastState();
		}
		break;
	}
	case BingoNetEventType::Message:
	{
		HandleMessage(Event.ClientId, Event.Payload);
		break;
	}
	case BingoNetEventType::Error:
	{
		if (Mode == ConnectionMode::Client) DisconnectReason = Event.Text;
		SetStatus(Event.Text);
		break;
	}
	}
}

void BingoModule::HandleMessage(int ClientId, const std::vector<uint8_t>& Payload)
{
	if (Payload.empty()) return;

	if (Mode == ConnectionMode::Host)
	{
		HandleClientMessage(ClientId, Payload);
	}
	else if (Mode == ConnectionMode::Client)
	{
		HandleServerMessage(Payload);
	}
}

void BingoModule::HandleClientMessage(int ClientId, const std::vector<uint8_t>& Payload)
{
	Reader In(Payload.data() + 1, Payload.size() - 1);
	uint8_t Type = Payload[0];

	if (Type == MSG_HELLO)
	{
		std::string Name = In.Str();
		int Team = In.I32();
		bool LoggedIn = In.U8() != 0;
		if (!In.Ok) return;

		if (!Match.Settings.AllowNoLogin && !LoggedIn)
		{
			Writer Out;
			Out.U8(MSG_ERROR);
			Out.Str("This host requires a Nadeo login");
			Net.SendToClient(ClientId, Out.Data);
			Net.DisconnectClient(ClientId);
			SetStatus(Name.empty() ? "Rejected player without Nadeo login" : "Rejected " + Name + " (no Nadeo login)");
			return;
		}

		if (Name.empty()) Name = "Player " + std::to_string(ClientId);
		if (Name.size() > 40) Name.resize(40);
		Team = std::clamp(Team, 0, std::max(0, Match.Settings.TeamCount - 1));

		auto Existing = std::find_if(Match.Players.begin(), Match.Players.end(), [&](const BingoPlayer& Player)
		{
			return Player.Id == ClientId;
		});

		if (Existing == Match.Players.end())
		{
			BingoPlayer Player = {};
			Player.Id = ClientId;
			Player.Name = Name;
			Player.TeamId = Team;
			Player.LoggedIn = LoggedIn;
			Match.Players.push_back(Player);
		}
		else
		{
			Existing->Name = Name;
			Existing->TeamId = Team;
			Existing->LoggedIn = LoggedIn;
		}

		SendStateTo(ClientId);
		BroadcastState();
		SetStatus(Name + " joined");
		return;
	}

	if (Type == MSG_SET_TEAM)
	{
		int Team = In.I32();
		if (!In.Ok) return;
		Team = std::clamp(Team, 0, std::max(0, Match.Settings.TeamCount - 1));

		for (BingoPlayer& Player : Match.Players)
		{
			if (Player.Id == ClientId)
			{
				Player.TeamId = Team;
				BroadcastState();
				break;
			}
		}
		return;
	}

	if (Type == MSG_BIND_UID)
	{
		int SquareIndex = In.I32();
		std::string Uid = In.Str();
		if (!In.Ok) return;
		BindSquareUid(ClientId, SquareIndex, Uid);
		return;
	}

	if (Type == MSG_CLAIM)
	{
		int SquareIndex = In.I32();
		int TimeMs = In.I32();
		std::string Uid = In.Str();
		if (!In.Ok) return;
		AddClaim(ClientId, SquareIndex, TimeMs, Uid);
		return;
	}

	if (Type == MSG_MAP_REQUEST)
	{
		uint64_t Hash = In.U64();
		if (!In.Ok) return;
		ServeMap(ClientId, Hash);
		return;
	}
}

void BingoModule::HandleServerMessage(const std::vector<uint8_t>& Payload)
{
	Reader In(Payload.data() + 1, Payload.size() - 1);
	uint8_t Type = Payload[0];

	if (Type == MSG_WELCOME)
	{
		int PlayerId = In.I32();
		BingoMatchState NewMatch = {};
		if (!DeserializeState(In, NewMatch))
		{
			SetStatus("Received invalid match state");
			return;
		}

		bool BoardChanged = NewMatch.Board.size() != Match.Board.size();
		if (!BoardChanged)
		{
			for (size_t Idx = 0; Idx < NewMatch.Board.size(); Idx++)
			{
				if (NewMatch.Board[Idx].Map.Hash != Match.Board[Idx].Map.Hash)
				{
					BoardChanged = true;
					break;
				}
			}
		}

		bool MatchStarted = NewMatch.Phase == BingoPhase::Running && Match.Phase != BingoPhase::Running;

		if (BoardChanged)
		{
			for (auto& Pair : Downloads)
			{
				if (Pair.second.File.is_open()) Pair.second.File.close();
				std::error_code Error;
				std::filesystem::remove(Pair.second.TempPath, Error);
			}
			Downloads.clear();
			RequestedMaps.clear();
			FailedMaps.clear();
			PendingOpenSquare = -1;
			PendingSquare = -1;
			ActiveSquare = -1;
		}

		if (MatchStarted)
		{
			PendingOpenSquare = -1;
			PendingSquare = -1;
			ActiveSquare = -1;

			auto Now = std::chrono::steady_clock::now();
			MatchStartTime = Now;
			GraceDurationMs = std::max(0, NewMatch.Settings.GraceMinutes) * 60000;
			MatchDurationMs = std::max(0, NewMatch.Settings.MatchMinutes) * 60000;
			MatchTimerEnabled = MatchDurationMs > 0;
			GraceEndTime = Now + std::chrono::milliseconds(GraceDurationMs);
			MatchEndTime = Now + std::chrono::milliseconds(MatchDurationMs);
			ResetMatchNotices();
		}

		DetectStateNotifications(Match, NewMatch);
		LocalPlayerId = PlayerId;
		Match = NewMatch;
		Connected = true;
		EnsureMaps();
		SetStatus("Connected to host");
		return;
	}

	if (Type == MSG_STATE)
	{
		BingoMatchState NewMatch = {};
		if (!DeserializeState(In, NewMatch))
		{
			SetStatus("Received invalid match state");
			return;
		}

		bool BoardChanged = NewMatch.Board.size() != Match.Board.size();
		if (!BoardChanged)
		{
			for (size_t Idx = 0; Idx < NewMatch.Board.size(); Idx++)
			{
				if (NewMatch.Board[Idx].Map.Hash != Match.Board[Idx].Map.Hash)
				{
					BoardChanged = true;
					break;
				}
			}
		}

		bool MatchStarted = NewMatch.Phase == BingoPhase::Running && Match.Phase != BingoPhase::Running;

		if (BoardChanged)
		{
			for (auto& Pair : Downloads)
			{
				if (Pair.second.File.is_open()) Pair.second.File.close();
				std::error_code Error;
				std::filesystem::remove(Pair.second.TempPath, Error);
			}
			Downloads.clear();
			RequestedMaps.clear();
			FailedMaps.clear();
			PendingOpenSquare = -1;
			PendingSquare = -1;
			ActiveSquare = -1;
		}

		if (MatchStarted)
		{
			PendingOpenSquare = -1;
			PendingSquare = -1;
			ActiveSquare = -1;

			auto Now = std::chrono::steady_clock::now();
			MatchStartTime = Now;
			GraceDurationMs = std::max(0, NewMatch.Settings.GraceMinutes) * 60000;
			MatchDurationMs = std::max(0, NewMatch.Settings.MatchMinutes) * 60000;
			MatchTimerEnabled = MatchDurationMs > 0;
			GraceEndTime = Now + std::chrono::milliseconds(GraceDurationMs);
			MatchEndTime = Now + std::chrono::milliseconds(MatchDurationMs);
			ResetMatchNotices();
		}

		DetectStateNotifications(Match, NewMatch);
		Match = NewMatch;
		EnsureMaps();
		return;
	}

	if (Type == MSG_MAP_CHUNK)
	{
		HandleMapChunk(Payload);
		return;
	}

	if (Type == MSG_ERROR)
	{
		std::string Message = In.Str();
		if (In.Ok) SetStatus(Message);
		return;
	}
}

std::vector<uint8_t> BingoModule::BuildStateMessage(uint8_t Type, int PlayerId) const
{
	Writer Out;
	Out.U8(Type);
	if (PlayerId >= 0) Out.I32(PlayerId);
	SerializeState(Out, Match);
	return Out.Data;
}

void BingoModule::SendStateTo(int ClientId)
{
	if (Mode != ConnectionMode::Host) return;
	Net.SendToClient(ClientId, BuildStateMessage(MSG_WELCOME, ClientId));
}

void BingoModule::BroadcastState()
{
	if (Mode != ConnectionMode::Host) return;
	DetectStateNotifications(NotifiedMatch, Match);
	NotifiedMatch = Match;
	Net.Broadcast(BuildStateMessage(MSG_STATE, -1));
}

void BingoModule::StartHost()
{
	StopConnection();

	RefreshIdentity();

	if (!AllowNoLogin && !LoggedIn)
	{
		SetStatus("Nadeo login required to host");
		return;
	}

	if (MapFolder.empty())
	{
		MapFolder = Twinkie->WStringToUTF8(GetDefaultMapFolder().wstring());
		RefreshBuffers();
	}

	Mode = ConnectionMode::Host;
	Connected = true;
	LocalPlayerId = 0;
	NextPlayerId = 1;

	Match = {};
	Match.Phase = BingoPhase::Lobby;
	Match.Settings = GetConfig();
	SyncTeams();

	BingoPlayer HostPlayer = {};
	HostPlayer.Id = 0;
	HostPlayer.Name = PlayerName;
	HostPlayer.TeamId = 0;
	HostPlayer.LoggedIn = LoggedIn;
	Match.Players.push_back(HostPlayer);

	RescanMaps();

	Logger->PrintInternalArgs("Bingo host starting on port {}", Port);

	if (!Net.StartServer((uint16_t)Port))
	{
		Mode = ConnectionMode::None;
		Connected = false;
		Match = {};
		SetStatus("Could not host on port " + std::to_string(Port));
		return;
	}

	BroadcastState();
	Logger->PrintInternalArgs("Bingo hosting on port {}", Port);
	SetStatus("Hosting on port " + std::to_string(Port));
}

void BingoModule::StartClient()
{
	StopConnection();

	RefreshIdentity();

	if (!AllowNoLogin && !LoggedIn)
	{
		SetStatus("Nadeo login required to join");
		return;
	}

	Mode = ConnectionMode::Client;
	Connected = false;
	LocalPlayerId = 0;

	if (!Net.StartClient(HostIp, (uint16_t)Port))
	{
		Mode = ConnectionMode::None;
		SetStatus("Could not connect to " + HostIp + ":" + std::to_string(Port));
		return;
	}

	SetStatus("Connecting to " + HostIp + ":" + std::to_string(Port));
}

void BingoModule::StopConnection()
{
	Net.Stop();

	for (auto& Pair : Downloads)
	{
		if (Pair.second.File.is_open()) Pair.second.File.close();
		std::error_code Error;
		std::filesystem::remove(Pair.second.TempPath, Error);
	}

	Downloads.clear();
	RequestedMaps.clear();
	FailedMaps.clear();
	Notifications.clear();
	NotifiedMatch = {};
	Mode = ConnectionMode::None;
	Connected = false;
	LocalPlayerId = 0;
	Match = {};
	ActiveSquare = -1;
	PendingSquare = -1;
	PendingOpenSquare = -1;
	ActiveMapUid.clear();
	LastChallenge = 0;
	FinishHandled = false;
	LastState = TM::Finished;
	LastRaceTime = 0;
	GraceNoticeShown = false;
	TimeNoticesSent.fill(false);
}

void BingoModule::RescanMaps()
{
	if (MapFolder.empty())
	{
		MapFolder = Twinkie->WStringToUTF8(GetDefaultMapFolder().wstring());
		RefreshBuffers();
	}

	std::filesystem::path Folder = std::filesystem::path(Twinkie->UTF8ToWString(MapFolder));
	MapPoolPaths.clear();
	MapFiles.clear();
	AvailableMaps.clear();
	RequestedMaps.clear();

	std::error_code Error;
	if (Folder.empty() || !std::filesystem::exists(Folder, Error))
	{
		SetStatus("Map folder does not exist");
		return;
	}

	for (std::filesystem::recursive_directory_iterator It(Folder, std::filesystem::directory_options::skip_permission_denied, Error), End; It != End; It.increment(Error))
	{
		if (Error)
		{
			Error.clear();
			continue;
		}
		if (!It->is_regular_file(Error)) continue;
		if (IsChallengeFile(It->path())) MapPoolPaths.push_back(It->path());
	}

	std::sort(MapPoolPaths.begin(), MapPoolPaths.end());

	if (Mode == ConnectionMode::Host && Match.Phase == BingoPhase::Lobby && !Match.Board.empty())
	{
		Match.Board.clear();
		BroadcastState();
	}

	Logger->PrintInternalArgs("Bingo map pool: {} maps", MapPoolPaths.size());
	SetStatus("Found " + std::to_string(MapPoolPaths.size()) + " maps");
}

void BingoModule::SyncTeams()
{
	BingoMatchSettings Config = GetConfig();
	Match.Settings = Config;

	if ((int)Match.Teams.size() != Config.TeamCount)
	{
		Match.Teams.clear();
		for (int Idx = 0; Idx < Config.TeamCount; Idx++)
		{
			BingoTeam Team = {};
			Team.Id = Idx;
			Team.Name = "Team " + std::to_string(Idx + 1);
			Match.Teams.push_back(Team);
		}
	}

	for (BingoPlayer& Player : Match.Players)
	{
		Player.TeamId = std::clamp(Player.TeamId, 0, std::max(0, Config.TeamCount - 1));
	}
}

void BingoModule::GenerateBoard()
{
	if (Mode != ConnectionMode::Host) return;
	if (Match.Phase == BingoPhase::Running) return;

	BingoMatchSettings Config = GetConfig();
	Match.Settings = Config;
	SyncTeams();

	int BoardSizeValue = Config.BoardSize;
	int BoardCount = BoardSizeValue * BoardSizeValue;
	if ((int)MapPoolPaths.size() < BoardCount)
	{
		SetStatus("Map folder needs at least " + std::to_string(BoardCount) + " maps");
		return;
	}

	std::vector<std::filesystem::path> Shuffled = MapPoolPaths;
	std::mt19937 Random((unsigned int)std::random_device{}());
	std::shuffle(Shuffled.begin(), Shuffled.end(), Random);

	MapFiles.clear();
	AvailableMaps.clear();

	std::vector<BingoMapInfo> Selected = {};
	std::unordered_set<uint64_t> Seen = {};
	for (const std::filesystem::path& File : Shuffled)
	{
		if ((int)Selected.size() >= BoardCount) break;

		uint64_t Hash = 0;
		uint64_t Size = 0;
		if (!HashFile(File, Hash, Size)) continue;
		if (Seen.contains(Hash)) continue;

		Seen.insert(Hash);
		BingoMapInfo Info = {};
		Info.Hash = Hash;
		Info.Size = Size;
		Info.DisplayName = MapDisplayName(File);
		ReadMedalTimes(File, Info);
		Selected.push_back(Info);
		MapFiles[Hash] = File;
		AvailableMaps.insert(Hash);
	}

	if ((int)Selected.size() < BoardCount)
	{
		SetStatus("Map folder does not contain enough unique maps");
		return;
	}

	Match.Board.clear();
	Match.Board.reserve(BoardCount);
	for (const BingoMapInfo& Map : Selected)
	{
		BingoSquare Square = {};
		Square.Map = Map;
		Square.OwnerTeam = -1;
		Square.RecordMs = INT_MAX;
		Match.Board.push_back(Square);
	}

	Match.Phase = BingoPhase::Lobby;
	Match.WinnerTeam = -1;
	Match.IsDraw = false;
	GraceEvaluated = false;
	FailedMaps.clear();
	PendingSquare = -1;
	PendingOpenSquare = -1;
	ActiveSquare = -1;
	FinishHandled = false;

	BroadcastState();
	SetStatus("Generated " + std::to_string(BoardSizeValue) + "x" + std::to_string(BoardSizeValue) + " board");
}

void BingoModule::StartMatch()
{
	if (Mode != ConnectionMode::Host) return;
	if (Match.Phase == BingoPhase::Running) return;

	BingoMatchSettings Config = GetConfig();
	Match.Settings = Config;
	SyncTeams();

	int BoardCount = Config.BoardSize * Config.BoardSize;
	if ((int)Match.Board.size() != BoardCount)
	{
		GenerateBoard();
		if ((int)Match.Board.size() != BoardCount) return;
	}

	for (BingoSquare& Square : Match.Board)
	{
		Square.OwnerTeam = -1;
		Square.RecordMs = INT_MAX;
		Square.RecordPlayer.clear();
	}

	if (Match.Players.empty())
	{
		BingoPlayer HostPlayer = {};
		HostPlayer.Id = 0;
		HostPlayer.Name = PlayerName;
		HostPlayer.TeamId = 0;
		Match.Players.push_back(HostPlayer);
	}

	auto Now = std::chrono::steady_clock::now();
	Match.Phase = BingoPhase::Running;
	Match.WinnerTeam = -1;
	Match.IsDraw = false;
	MatchStartTime = Now;
	GraceDurationMs = std::max(0, Config.GraceMinutes) * 60000;
	MatchDurationMs = std::max(0, Config.MatchMinutes) * 60000;
	MatchTimerEnabled = MatchDurationMs > 0;
	GraceEndTime = Now + std::chrono::milliseconds(GraceDurationMs);
	MatchEndTime = Now + std::chrono::milliseconds(MatchDurationMs);
	GraceEvaluated = GraceDurationMs <= 0;
	PendingSquare = -1;
	PendingOpenSquare = -1;
	ActiveSquare = -1;
	FinishHandled = false;
	ResetMatchNotices();

	BroadcastState();
	SetStatus("Match started");
}

void BingoModule::FinishMatch(int WinnerTeam, bool IsDraw)
{
	if (Mode != ConnectionMode::Host) return;
	if (Match.Phase != BingoPhase::Running) return;

	Match.Phase = BingoPhase::Finished;
	Match.WinnerTeam = WinnerTeam;
	Match.IsDraw = IsDraw;

	BroadcastState();

	if (IsDraw)
	{
		SetStatus("Match ended in a draw");
	}
	else if (WinnerTeam >= 0 && WinnerTeam < (int)Match.Teams.size())
	{
		SetStatus(Match.Teams[WinnerTeam].Name + " wins");
	}
}

int BingoModule::CountSquares(int TeamId) const
{
	int Count = 0;
	for (const BingoSquare& Square : Match.Board)
	{
		if (Square.OwnerTeam == TeamId) Count++;
	}
	return Count;
}

std::vector<int> BingoModule::FindLineTeams() const
{
	std::set<int> Teams = {};
	int Size = Match.Settings.BoardSize;
	if (Size <= 0 || Match.Board.size() < (size_t)(Size * Size)) return {};

	for (int Row = 0; Row < Size; Row++)
	{
		int Team = Match.Board[Row * Size].OwnerTeam;
		bool Full = Team >= 0;
		for (int Col = 1; Col < Size && Full; Col++)
		{
			Full = Match.Board[Row * Size + Col].OwnerTeam == Team;
		}
		if (Full) Teams.insert(Team);
	}

	for (int Col = 0; Col < Size; Col++)
	{
		int Team = Match.Board[Col].OwnerTeam;
		bool Full = Team >= 0;
		for (int Row = 1; Row < Size && Full; Row++)
		{
			Full = Match.Board[Row * Size + Col].OwnerTeam == Team;
		}
		if (Full) Teams.insert(Team);
	}

	{
		int Team = Match.Board[0].OwnerTeam;
		bool Full = Team >= 0;
		for (int Idx = 1; Idx < Size && Full; Idx++)
		{
			Full = Match.Board[Idx * Size + Idx].OwnerTeam == Team;
		}
		if (Full) Teams.insert(Team);
	}

	{
		int Team = Match.Board[Size - 1].OwnerTeam;
		bool Full = Team >= 0;
		for (int Idx = 1; Idx < Size && Full; Idx++)
		{
			Full = Match.Board[Idx * Size + (Size - 1 - Idx)].OwnerTeam == Team;
		}
		if (Full) Teams.insert(Team);
	}

	return std::vector<int>(Teams.begin(), Teams.end());
}

bool BingoModule::IsLineTeam(int TeamId) const
{
	std::vector<int> Teams = FindLineTeams();
	return std::find(Teams.begin(), Teams.end(), TeamId) != Teams.end();
}

void BingoModule::EvaluateAfterGrace()
{
	std::vector<int> Teams = FindLineTeams();
	if (Teams.empty()) return;

	if (Teams.size() == 1)
	{
		FinishMatch(Teams[0], false);
		return;
	}

	int BestTeam = -1;
	int BestLines = -1;
	int BestSquares = -1;
	bool Tie = false;

	for (int Team : Teams)
	{
		int Lines = CountLinesForTeam(Match, Team);
		int Squares = CountSquares(Team);

		if (Lines > BestLines || (Lines == BestLines && Squares > BestSquares))
		{
			BestTeam = Team;
			BestLines = Lines;
			BestSquares = Squares;
			Tie = false;
		}
		else if (Lines == BestLines && Squares == BestSquares)
		{
			Tie = true;
		}
	}

	if (Tie || BestTeam < 0)
	{
		FinishMatch(-1, true);
	}
	else
	{
		FinishMatch(BestTeam, false);
	}
}

void BingoModule::ProcessHostTimers()
{
	if (Mode != ConnectionMode::Host) return;
	if (Match.Phase != BingoPhase::Running) return;

	auto Now = std::chrono::steady_clock::now();

	if (!GraceEvaluated && Now >= GraceEndTime)
	{
		GraceEvaluated = true;
		EvaluateAfterGrace();
		if (Match.Phase != BingoPhase::Running) return;
	}

	if (MatchTimerEnabled && Now >= MatchEndTime)
	{
		int BestTeam = -1;
		int BestCount = -1;
		bool Tie = false;

		for (const BingoTeam& Team : Match.Teams)
		{
			int Count = CountSquares(Team.Id);
			if (Count > BestCount)
			{
				BestCount = Count;
				BestTeam = Team.Id;
				Tie = false;
			}
			else if (Count == BestCount)
			{
				Tie = true;
			}
		}

		if (Tie || BestTeam < 0)
		{
			FinishMatch(-1, true);
		}
		else
		{
			FinishMatch(BestTeam, false);
		}
	}
}

void BingoModule::ProcessNotificationTimers()
{
	if (Match.Phase != BingoPhase::Running) return;

	auto Now = std::chrono::steady_clock::now();

	if (!GraceNoticeShown && GraceDurationMs > 0 && Now >= GraceEndTime)
	{
		GraceNoticeShown = true;
		AddNotification("Grace period finished - lines can now win", NotificationGraceColor());
	}

	if (!MatchTimerEnabled) return;

	long long LeftMs = std::chrono::duration_cast<std::chrono::milliseconds>(MatchEndTime - Now).count();

	for (int Idx = 0; Idx < 4; Idx++)
	{
		if (TimeNoticesSent[Idx]) continue;
		if (LeftMs > (long long)TimeNoticeThresholds[Idx] * 60000) continue;

		TimeNoticesSent[Idx] = true;
		if (LeftMs <= 0) continue;

		std::string Text = std::to_string(TimeNoticeThresholds[Idx]) + (TimeNoticeThresholds[Idx] == 1 ? " minute left" : " minutes left");
		AddNotification(Text, NotificationTimeColor());
	}
}

void BingoModule::ResetMatchNotices()
{
	GraceNoticeShown = false;
	TimeNoticesSent.fill(false);

	for (int Idx = 0; Idx < 4; Idx++)
	{
		if (MatchDurationMs <= TimeNoticeThresholds[Idx] * 60000) TimeNoticesSent[Idx] = true;
	}
}

void BingoModule::AddNotification(const std::string& Text, const ImVec4& Color)
{
	if (Text.empty()) return;
	AddNotification(std::vector<NotificationSpan>{ NotificationSpan{ Text, Color } });
}

void BingoModule::AddNotification(const std::vector<NotificationSpan>& Spans)
{
	bool AnyText = false;
	for (const NotificationSpan& Span : Spans)
	{
		if (!Span.Text.empty())
		{
			AnyText = true;
			break;
		}
	}
	if (!AnyText) return;

	Notification Entry = {};
	Entry.Spans = Spans;
	Entry.Time = std::chrono::steady_clock::now();
	Notifications.push_back(std::move(Entry));

	while (Notifications.size() > 12) Notifications.pop_front();
}

void BingoModule::CleanupNotifications()
{
	auto Now = std::chrono::steady_clock::now();
	while (!Notifications.empty() && Now - Notifications.front().Time > std::chrono::seconds(30))
	{
		Notifications.pop_front();
	}
}

void BingoModule::DetectStateNotifications(const BingoMatchState& Old, const BingoMatchState& New)
{
	bool Initial = Old.Players.empty() && Old.Board.empty() && Old.Phase == BingoPhase::Idle;
	if (Initial) return;

	for (const BingoPlayer& Player : New.Players)
	{
		auto Existing = std::find_if(Old.Players.begin(), Old.Players.end(), [&](const BingoPlayer& Candidate)
		{
			return Candidate.Id == Player.Id;
		});

		if (Existing == Old.Players.end())
		{
			AddNotification(std::vector<NotificationSpan>{
				NotificationSpan{ Player.Name, TeamColor(Player.TeamId) },
				NotificationSpan{ " joined", NotificationJoinColor() }
			});
			continue;
		}

		if (Existing->TeamId != Player.TeamId)
		{
			std::string TeamName = "Team " + std::to_string(Player.TeamId + 1);
			for (const BingoTeam& Team : New.Teams)
			{
				if (Team.Id == Player.TeamId)
				{
					TeamName = Team.Name;
					break;
				}
			}
			AddNotification(std::vector<NotificationSpan>{
				NotificationSpan{ Player.Name, TeamColor(Player.TeamId) },
				NotificationSpan{ " joined ", NotificationTeamColor() },
				NotificationSpan{ TeamName, TeamColor(Player.TeamId) }
			});
		}
	}

	if (Old.Board.size() == New.Board.size() && !Old.Board.empty())
	{
		for (size_t Idx = 0; Idx < New.Board.size(); Idx++)
		{
			const BingoSquare& OldSquare = Old.Board[Idx];
			const BingoSquare& NewSquare = New.Board[Idx];

			if (OldSquare.Map.Hash != NewSquare.Map.Hash) continue;
			if (OldSquare.OwnerTeam == NewSquare.OwnerTeam && OldSquare.RecordMs == NewSquare.RecordMs) continue;
			if (NewSquare.OwnerTeam < 0) continue;

			std::string Label = SquareLabel((int)Idx, New.Settings.BoardSize);
			std::string Time = Twinkie->FormatTmDuration((unsigned int)NewSquare.RecordMs);
			std::string PlayerName = NewSquare.RecordPlayer.empty() ? "Player" : NewSquare.RecordPlayer;

			if (OldSquare.OwnerTeam < 0)
			{
				AddNotification(std::vector<NotificationSpan>{
					NotificationSpan{ PlayerName, TeamColor(NewSquare.OwnerTeam) },
					NotificationSpan{ " claimed ", NotificationClaimColor() },
					NotificationSpan{ Label, NotificationClaimColor() },
					NotificationSpan{ " (" + Time + ")", NotificationClaimColor() }
				});
			}
			else
			{
				std::string TeamName = "Team " + std::to_string(NewSquare.OwnerTeam + 1);
				for (const BingoTeam& Team : New.Teams)
				{
					if (Team.Id == NewSquare.OwnerTeam)
					{
						TeamName = Team.Name;
						break;
					}
				}
				AddNotification(std::vector<NotificationSpan>{
					NotificationSpan{ PlayerName, TeamColor(NewSquare.OwnerTeam) },
					NotificationSpan{ " reclaimed ", NotificationReclaimColor() },
					NotificationSpan{ Label, NotificationReclaimColor() },
					NotificationSpan{ " for ", NotificationReclaimColor() },
					NotificationSpan{ TeamName, TeamColor(NewSquare.OwnerTeam) },
					NotificationSpan{ " (" + Time + ")", NotificationReclaimColor() }
				});
			}
		}
	}

	if (Old.Phase != BingoPhase::Finished && New.Phase == BingoPhase::Finished)
	{
		if (New.IsDraw)
		{
			AddNotification("Match ended in a draw", NotificationWinColor());
		}
		else if (New.WinnerTeam >= 0 && New.WinnerTeam < (int)New.Teams.size())
		{
			AddNotification(std::vector<NotificationSpan>{
				NotificationSpan{ New.Teams[New.WinnerTeam].Name, TeamColor(New.WinnerTeam) },
				NotificationSpan{ " wins!", NotificationWinColor() }
			});
		}
	}
}

void BingoModule::AddClaim(int ClientId, int SquareIndex, int TimeMs, const std::string& MapUid)
{
	if (Mode != ConnectionMode::Host) return;
	if (Match.Phase != BingoPhase::Running) return;

	BingoPlayer* Player = nullptr;
	for (BingoPlayer& Candidate : Match.Players)
	{
		if (Candidate.Id == ClientId)
		{
			Player = &Candidate;
			break;
		}
	}

	if (!Player) return;
	if (Player->TeamId < 0 || Player->TeamId >= (int)Match.Teams.size()) return;

	BingoSquare* Square = GetSquare(SquareIndex);
	if (!Square) return;
	if (TimeMs <= 0) return;

	if (Square->MapUid.empty() && !MapUid.empty())
	{
		Square->MapUid = MapUid;
	}
	else if (!Square->MapUid.empty() && !MapUid.empty() && Square->MapUid != MapUid)
	{
		return;
	}
	else if (!Square->MapUid.empty() && MapUid.empty())
	{
		return;
	}

	if (Square->OwnerTeam >= 0 && TimeMs >= Square->RecordMs) return;

	Square->OwnerTeam = Player->TeamId;
	Square->RecordMs = TimeMs;
	Square->RecordPlayer = Player->Name;

	BroadcastState();

	if (GraceEvaluated)
	{
		std::vector<int> Teams = FindLineTeams();
		if (!Teams.empty())
		{
			EvaluateAfterGrace();
			return;
		}
	}

	ProcessHostTimers();
}

void BingoModule::BindSquareUid(int ClientId, int SquareIndex, const std::string& MapUid)
{
	if (Mode != ConnectionMode::Host) return;
	if (MapUid.empty()) return;

	BingoSquare* Square = GetSquare(SquareIndex);
	if (!Square) return;
	if (!Square->MapUid.empty()) return;

	Square->MapUid = MapUid;
	BroadcastState();
}

void BingoModule::CheckFinish()
{
	auto Now = std::chrono::steady_clock::now();
	auto GetStableUid = [this]()
	{
		std::string Uid = Twinkie->GetChallengeUID();
		if (Uid.empty() || Uid == "Unassigned")
		{
			std::string Name = Twinkie->GetChallengeName();
			if (!Name.empty()) Uid = "name:" + Name;
			else Uid.clear();
		}
		return Uid;
	};
	uintptr_t Challenge = Twinkie->GetChallenge();

	if (Challenge == 0)
	{
		if (LastChallenge != 0)
		{
			LastChallenge = 0;
			ActiveSquare = -1;
			ActiveMapUid.clear();
			FinishHandled = false;
			LastState = TM::Finished;
			LastRaceTime = 0;
		}
	}
	else if (Challenge != LastChallenge)
	{
		LastChallenge = Challenge;
		FinishHandled = false;
		LastState = TM::Finished;
		LastRaceTime = 0;
		if (PendingSquare < 0)
		{
			ActiveSquare = -1;
			ActiveMapUid.clear();
		}
	}

	if (PendingSquare >= 0)
	{
		if (Now - PendingSince > std::chrono::seconds(60))
		{
			PendingSquare = -1;
			PendingFromUid.clear();
		}
		else if (Challenge != 0 && Twinkie->IsPlaying())
		{
			std::string RawUid = Twinkie->GetChallengeUID();
			std::string Uid = GetStableUid();
			BingoSquare* Square = GetSquare(PendingSquare);

			bool Ready = false;
			bool Mismatch = false;

			if (Square && !Uid.empty())
			{
				if (!Square->MapUid.empty())
				{
					if (Square->MapUid == Uid)
					{
						Ready = true;
					}
					else if (RawUid != PendingFromUid)
					{
						Mismatch = true;
					}
				}
				else
				{
					Ready = !RawUid.empty() && RawUid != "Unassigned" && RawUid != PendingFromUid;
				}
			}

			if (Mismatch)
			{
				SetStatus("Loaded map does not match the selected square");
				PendingSquare = -1;
				PendingFromUid.clear();
			}
			else if (Ready)
			{
				if (Square->MapUid.empty())
				{
					Square->MapUid = Uid;
					if (Mode == ConnectionMode::Host)
					{
						BroadcastState();
					}
					else if (Mode == ConnectionMode::Client && Connected)
					{
						Writer Out;
						Out.U8(MSG_BIND_UID);
						Out.I32(PendingSquare);
						Out.Str(Uid);
						Net.SendToServer(Out.Data);
					}
				}

				ActiveSquare = PendingSquare;
				ActiveMapUid = Uid;
				PendingSquare = -1;
				PendingFromUid.clear();
				FinishHandled = false;
				LastState = TM::Finished;
				LastRaceTime = 0;
			}
		}
	}

	if (!Twinkie->IsPlaying()) return;
	if (Twinkie->IsInEditor()) return;

	if (ActiveSquare >= 0 && Challenge != 0)
	{
		std::string Uid = GetStableUid();
		BingoSquare* Square = GetSquare(ActiveSquare);
		if (!Square || (!Square->MapUid.empty() && !Uid.empty() && Square->MapUid != Uid))
		{
			ActiveSquare = -1;
			ActiveMapUid.clear();
		}
	}

	long RaceTime = Twinkie->GetRaceTime();
	if (RaceTime > 0) LastRaceTime = RaceTime;

	TM::RaceState State = Twinkie->GetState();

	if (LastState != TM::Finished && State == TM::Finished && !FinishHandled)
	{
		FinishHandled = true;

		long Time = LastRaceTime > 0 ? LastRaceTime : RaceTime;
		if (Time > 0 && ActiveSquare >= 0 && Connected && Match.Phase == BingoPhase::Running)
		{
			BingoSquare* Square = GetSquare(ActiveSquare);
			if (Square)
			{
				bool Empty = Square->OwnerTeam < 0;
				bool Allowed = !Empty || MeetsMedalRule(Twinkie, (int)Time, Match.Settings.MedalRule);
				if (Twinkie->ChallengeUsesScore()) Allowed = false;

				if (Allowed)
				{
					std::string Uid = GetStableUid();

					if (Mode == ConnectionMode::Host)
					{
						AddClaim(LocalPlayerId, ActiveSquare, (int)Time, Uid);
					}
					else if (Mode == ConnectionMode::Client)
					{
						Writer Out;
						Out.U8(MSG_CLAIM);
						Out.I32(ActiveSquare);
						Out.I32((int)Time);
						Out.Str(Uid);
						Net.SendToServer(Out.Data);
					}

					SetStatus("Finished in " + Twinkie->FormatTmDuration((unsigned int)Time));
				}
			}
		}
	}

	if (LastState == TM::Finished && State != TM::Finished)
	{
		FinishHandled = false;
	}

	LastState = State;
}

void BingoModule::OpenSquare(int SquareIndex)
{
	if (Match.Phase != BingoPhase::Running && Match.Phase != BingoPhase::Finished) return;

	BingoSquare* Square = GetSquare(SquareIndex);
	if (!Square) return;

	if (!AvailableMaps.contains(Square->Map.Hash))
	{
		if (Mode == ConnectionMode::Client && Connected)
		{
			PendingOpenSquare = SquareIndex;
			FailedMaps.erase(Square->Map.Hash);
			if (!RequestedMaps.contains(Square->Map.Hash))
			{
				RequestedMaps.insert(Square->Map.Hash);
				Writer Out;
				Out.U8(MSG_MAP_REQUEST);
				Out.U64(Square->Map.Hash);
				Net.SendToServer(Out.Data);
			}
			SetStatus("Downloading map");
		}
		else
		{
			SetStatus("Map is not available");
		}
		return;
	}

	auto It = MapFiles.find(Square->Map.Hash);
	if (It == MapFiles.end())
	{
		SetStatus("Map path is missing");
		return;
	}

	if (LaunchMap(It->second))
	{
		PendingSquare = SquareIndex;
		PendingFromUid = Twinkie->GetChallengeUID();
		PendingSince = std::chrono::steady_clock::now();
		SetStatus("Opening " + Square->Map.DisplayName);
	}
	else
	{
		SetStatus("Could not launch map");
	}
}

bool BingoModule::LaunchMap(const std::filesystem::path& Path)
{
	std::error_code Error;
	if (!std::filesystem::exists(Path, Error)) return false;

	std::wstring ExePath(32768, L'\0');
	DWORD Length = GetModuleFileNameW(nullptr, ExePath.data(), (DWORD)ExePath.size());
	if (Length == 0 || Length >= ExePath.size()) return false;
	ExePath.resize(Length);

	std::wstring Arguments = L"\"" + ExePath + L"\" /useexedir /singleinst /file=\"" + Path.wstring() + L"\"";
	std::wstring WorkingDirectory = std::filesystem::path(ExePath).parent_path().wstring();

	STARTUPINFOW StartInfo = {};
	StartInfo.cb = sizeof(StartInfo);
	PROCESS_INFORMATION ProcessInfo = {};

	BOOL Started = CreateProcessW(
		ExePath.c_str(),
		Arguments.data(),
		nullptr,
		nullptr,
		FALSE,
		0,
		nullptr,
		WorkingDirectory.c_str(),
		&StartInfo,
		&ProcessInfo);

	if (!Started) return false;

	CloseHandle(ProcessInfo.hThread);
	CloseHandle(ProcessInfo.hProcess);
	return true;
}

void BingoModule::EnsureMaps()
{
	if (!Connected || Match.Board.empty()) return;
	if (Mode != ConnectionMode::Client) return;

	for (const BingoSquare& Square : Match.Board)
	{
		if (AvailableMaps.contains(Square.Map.Hash)) continue;

		std::filesystem::path Path = GetMapDownloadPath(Square.Map.Hash);
		std::error_code Error;
		if (!Path.empty() && std::filesystem::exists(Path, Error))
		{
			AvailableMaps.insert(Square.Map.Hash);
			MapFiles[Square.Map.Hash] = Path;
		}
	}

	if (RequestedMaps.size() >= 2) return;

	for (const BingoSquare& Square : Match.Board)
	{
		if (AvailableMaps.contains(Square.Map.Hash)) continue;
		if (RequestedMaps.contains(Square.Map.Hash)) continue;
		if (FailedMaps.contains(Square.Map.Hash)) continue;

		RequestedMaps.insert(Square.Map.Hash);
		Writer Out;
		Out.U8(MSG_MAP_REQUEST);
		Out.U64(Square.Map.Hash);
		Net.SendToServer(Out.Data);
		return;
	}
}

void BingoModule::HandleMapChunk(const std::vector<uint8_t>& Payload)
{
	Reader In(Payload.data() + 1, Payload.size() - 1);

	uint64_t Hash = In.U64();
	int Offset = In.I32();
	bool IsLast = In.U8() != 0;
	std::vector<uint8_t> Bytes = In.Bytes();
	if (!In.Ok) return;

	const BingoSquare* TargetSquare = nullptr;
	for (const BingoSquare& Square : Match.Board)
	{
		if (Square.Map.Hash == Hash)
		{
			TargetSquare = &Square;
			break;
		}
	}

	if (!TargetSquare) return;

	std::filesystem::path FinalPath = GetMapDownloadPath(Hash);
	if (FinalPath.empty()) return;

	auto It = Downloads.find(Hash);
	if (It == Downloads.end())
	{
		DownloadState State = {};
		State.TempPath = FinalPath;
		State.TempPath += L".part";
		State.Expected = TargetSquare->Map.Size;
		State.File.open(State.TempPath, std::ios::binary | std::ios::trunc);
		if (!State.File)
		{
			RequestedMaps.erase(Hash);
			FailedMaps.insert(Hash);
			SetStatus("Could not create map download");
			return;
		}
		It = Downloads.emplace(Hash, std::move(State)).first;
	}

	DownloadState& State = It->second;

	if (Offset < 0 || (uint64_t)Offset != State.Received)
	{
		State.File.close();
		std::error_code Error;
		std::filesystem::remove(State.TempPath, Error);
		Downloads.erase(It);
		RequestedMaps.erase(Hash);
		FailedMaps.insert(Hash);
		SetStatus("Map download was corrupted");
		return;
	}

	if (State.Received + Bytes.size() > State.Expected)
	{
		State.File.close();
		std::error_code Error;
		std::filesystem::remove(State.TempPath, Error);
		Downloads.erase(It);
		RequestedMaps.erase(Hash);
		FailedMaps.insert(Hash);
		SetStatus("Map download was too large");
		return;
	}

	State.File.write((const char*)Bytes.data(), (std::streamsize)Bytes.size());
	State.Received += Bytes.size();

	if (!IsLast) return;

	State.File.close();

	if (State.Received != State.Expected)
	{
		std::error_code Error;
		std::filesystem::remove(State.TempPath, Error);
		Downloads.erase(It);
		RequestedMaps.erase(Hash);
		FailedMaps.insert(Hash);
		SetStatus("Map download was incomplete");
		return;
	}

	uint64_t ActualHash = 0;
	uint64_t ActualSize = 0;
	if (!HashFile(State.TempPath, ActualHash, ActualSize) || ActualHash != Hash || ActualSize != State.Expected)
	{
		std::error_code Error;
		std::filesystem::remove(State.TempPath, Error);
		Downloads.erase(It);
		RequestedMaps.erase(Hash);
		FailedMaps.insert(Hash);
		SetStatus("Map download failed validation");
		return;
	}

	std::error_code Error;
	std::filesystem::remove(FinalPath, Error);
	std::filesystem::rename(State.TempPath, FinalPath, Error);
	if (Error)
	{
		std::filesystem::remove(State.TempPath, Error);
		Downloads.erase(It);
		RequestedMaps.erase(Hash);
		FailedMaps.insert(Hash);
		SetStatus("Could not save downloaded map");
		return;
	}

	Downloads.erase(It);
	RequestedMaps.erase(Hash);
	AvailableMaps.insert(Hash);
	MapFiles[Hash] = FinalPath;

	if (PendingOpenSquare >= 0)
	{
		const BingoSquare* Pending = GetSquare(PendingOpenSquare);
		if (Pending && Pending->Map.Hash == Hash)
		{
			int SquareIndex = PendingOpenSquare;
			PendingOpenSquare = -1;
			OpenSquare(SquareIndex);
		}
	}

	SetStatus("Map downloaded");
}

void BingoModule::ServeMap(int ClientId, uint64_t Hash)
{
	bool OnBoard = false;
	for (const BingoSquare& Square : Match.Board)
	{
		if (Square.Map.Hash == Hash)
		{
			OnBoard = true;
			break;
		}
	}

	if (!OnBoard)
	{
		Writer Out;
		Out.U8(MSG_ERROR);
		Out.Str("Requested map is not part of the board");
		Net.SendToClient(ClientId, Out.Data);
		return;
	}

	auto It = MapFiles.find(Hash);
	if (It == MapFiles.end())
	{
		Writer Out;
		Out.U8(MSG_ERROR);
		Out.Str("Host does not have the requested map");
		Net.SendToClient(ClientId, Out.Data);
		return;
	}

	std::ifstream File(It->second, std::ios::binary);
	if (!File)
	{
		Writer Out;
		Out.U8(MSG_ERROR);
		Out.Str("Host could not open the requested map");
		Net.SendToClient(ClientId, Out.Data);
		return;
	}

	std::vector<uint8_t> Data((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	if (Data.empty()) return;

	for (size_t Offset = 0; Offset < Data.size(); Offset += MapChunkSize)
	{
		size_t Size = std::min((size_t)MapChunkSize, Data.size() - Offset);
		bool IsLast = Offset + Size >= Data.size();

		Writer Out;
		Out.U8(MSG_MAP_CHUNK);
		Out.U64(Hash);
		Out.I32((int)Offset);
		Out.U8(IsLast ? 1 : 0);
		Out.Bytes(Data.data() + Offset, Size);
		Net.SendToClient(ClientId, Out.Data);
	}
}

void BingoModule::RenderAnyways()
{
	if (!Enabled) return;

	using namespace ImGui;

	RenderNotifications();

	auto Flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;

	bool ActiveMatch = Match.Phase == BingoPhase::Running || Match.Phase == BingoPhase::Finished;
	bool ShowBoard = !Match.Board.empty() && (ActiveMatch || Match.Phase == BingoPhase::Lobby);

	if (!ShowBoard)
	{
		auto StatusFlags = Flags;
		if (!*UiRenderEnabled) StatusFlags |= ImGuiWindowFlags_NoInputs;

		if (Begin("Bingo", nullptr, StatusFlags))
		{
			Text("Bingo");
			TextWrapped("%s", StatusMessage.empty() ? "Not connected" : StatusMessage.c_str());
			if (Mode == ConnectionMode::None)
			{
				TextWrapped("Open Twinkie Settings -> Bingo to host or join a match.");
			}
		}
		End();
		return;
	}

	if (Begin("Bingo", nullptr, Flags))
	{
		if (Match.Phase == BingoPhase::Finished)
		{
			if (Match.IsDraw)
			{
				Text("Draw");
			}
			else if (Match.WinnerTeam >= 0 && Match.WinnerTeam < (int)Match.Teams.size())
			{
				ImVec4 Color = TeamColor(Match.WinnerTeam);
				TextColored(Color, "%s wins", Match.Teams[Match.WinnerTeam].Name.c_str());
			}
		}
		else if (Match.Phase == BingoPhase::Lobby)
		{
			Text("Bingo - Lobby");
		}
		else
		{
			Text("Bingo");
		}

		int Size = Match.Settings.BoardSize;
		if (Size <= 0 || Match.Board.size() < (size_t)(Size * Size)) 
		{
			End();
			return;
		}

		PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
		PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
		PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

		for (int Row = 0; Row < Size; Row++)
		{
			for (int Col = 0; Col < Size; Col++)
			{
				int Index = Row * Size + Col;
				const BingoSquare& Square = Match.Board[Index];

				bool Downloaded = AvailableMaps.contains(Square.Map.Hash);
				ImVec4 Background = ImVec4(0.15f, 0.15f, 0.18f, 1.f);
				if (!Downloaded)
				{
					Background = ImVec4(1.f, 1.f, 1.f, 1.f);
				}
				else if (Square.OwnerTeam >= 0)
				{
					Background = TeamColor(Square.OwnerTeam);
				}

				ImVec4 TextColor = TextColorFor(Background);
				std::string Label = std::string(1, (char)('A' + Col)) + std::to_string(Row + 1);

				PushStyleColor(ImGuiCol_Button, Background);
				PushStyleColor(ImGuiCol_ButtonHovered, Lighten(Background, 0.12f));
				PushStyleColor(ImGuiCol_ButtonActive, Darken(Background, 0.12f));
				PushStyleColor(ImGuiCol_Text, TextColor);
				PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 1.f));

				if (Button(Label.c_str(), ImVec2(CellSize, CellSize)))
				{
					if (ActiveMatch) OpenSquare(Index);
				}

				PopStyleColor(5);

				if (IsItemHovered())
				{
					BeginTooltip();
					Text("%s  %s", Label.c_str(), Square.Map.DisplayName.c_str());
					Separator();

					if (Square.OwnerTeam >= 0)
					{
						std::string TeamName = "Team " + std::to_string(Square.OwnerTeam + 1);
						for (const BingoTeam& Team : Match.Teams)
						{
							if (Team.Id == Square.OwnerTeam)
							{
								TeamName = Team.Name;
								break;
							}
						}

						std::string Record = Twinkie->FormatTmDuration((unsigned int)Square.RecordMs);
						TextColored(TeamColor(Square.OwnerTeam), "Owner: %s", TeamName.c_str());
						Text("Target to reclaim: %s", Record.c_str());
						if (!Square.RecordPlayer.empty())
						{
							Text("Set by: ");
							SameLine(0.f, 0.f);
							TextColored(TeamColor(Square.OwnerTeam), "%s", Square.RecordPlayer.c_str());
						}
					}
					else
					{
						const char* RuleName = MedalRuleNames[std::clamp((int)Match.Settings.MedalRule, 0, 4)];
						int TargetMs = -1;

						switch (Match.Settings.MedalRule)
						{
						case MedalRequirement::Bronze: TargetMs = Square.Map.BronzeMs; break;
						case MedalRequirement::Silver: TargetMs = Square.Map.SilverMs; break;
						case MedalRequirement::Gold: TargetMs = Square.Map.GoldMs; break;
						case MedalRequirement::Author: TargetMs = Square.Map.AuthorMs; break;
						default: break;
						}

						if (Match.Settings.MedalRule == MedalRequirement::None)
						{
							Text("Target: any finish claims this square");
						}
						else if (TargetMs > 0)
						{
							std::string Target = Twinkie->FormatTmDuration((unsigned int)TargetMs);
							Text("Target: %s medal - %s", RuleName, Target.c_str());
						}
						else
						{
							Text("Target: %s medal - time unknown", RuleName);
						}
					}

					EndTooltip();
				}

				if (Col + 1 < Size) SameLine();
			}
		}

		PopStyleVar(3);
	}

	End();
}

void BingoModule::RenderInactive()
{
	ProcessEvents();
	ProcessHostTimers();
	ProcessNotificationTimers();
	CheckFinish();
	EnsureMaps();
	CleanupNotifications();
}

void BingoModule::RenderNotifications()
{
	CleanupNotifications();
	if (Notifications.empty()) return;

	using namespace ImGui;

	auto Flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
	if (!*UiRenderEnabled) Flags |= ImGuiWindowFlags_NoInputs;

	if (Begin("Bingo notifications", nullptr, Flags))
	{
		for (const Notification& Entry : Notifications)
		{
			bool First = true;
			for (const NotificationSpan& Span : Entry.Spans)
			{
				if (Span.Text.empty()) continue;
				if (!First) SameLine(0.f, 0.f);
				TextColored(Span.Color, "%s", Span.Text.c_str());
				First = false;
			}
		}
	}
	End();
}

void BingoModule::RenderSettings()
{
	using namespace ImGui;

	static bool LoggedSettingsOpen = false;
	if (!LoggedSettingsOpen)
	{
		Logger->PrintInternal("Bingo settings opened");
		RefreshIdentity();
		LoggedSettingsOpen = true;
	}

	if (StatusMessage.empty()) StatusMessage = "Idle";
	TextWrapped("%s", StatusMessage.c_str());
	Separator();

	std::snprintf(HostIpBuffer, sizeof(HostIpBuffer), "%s", HostIp.c_str());
	if (MapFolder.empty())
	{
		MapFolder = Twinkie->WStringToUTF8(GetDefaultMapFolder().wstring());
		std::snprintf(MapFolderBuffer, sizeof(MapFolderBuffer), "%s", MapFolder.c_str());
		Logger->PrintInternalArgs("Bingo map folder: {}", MapFolder);
	}
	std::snprintf(MapFolderBuffer, sizeof(MapFolderBuffer), "%s", MapFolder.c_str());

	const BingoPlayer* LocalPlayer = GetLocalPlayer();
	ImVec4 PlayerColor = LocalPlayer ? TeamColor(LocalPlayer->TeamId) : GetStyleColorVec4(ImGuiCol_Text);
	Text("Player: ");
	SameLine(0.f, 0.f);
	TextColored(PlayerColor, "%s", PlayerName.c_str());
	Text("Nadeo login: %s", LoggedIn ? "yes" : "no");

	InputText("Host IP", HostIpBuffer, sizeof(HostIpBuffer));
	HostIp = HostIpBuffer;
	if (HostIp.empty()) HostIp = "127.0.0.1";

	InputInt("Port", &Port);
	Port = std::clamp(Port, 1, 65535);

	if (Checkbox("Allow players without Nadeo login", &AllowNoLogin))
	{
		if (Mode == ConnectionMode::Host)
		{
			SyncTeams();
			BroadcastState();
		}
	}

	if (InputFloat("Cell size", &CellSize, 1.f, 8.f, "%.0f"))
	{
		CellSize = std::clamp(CellSize, 32.f, 160.f);
	}

	if (Mode == ConnectionMode::None)
	{
		if (Button("Host"))
		{
			StartHost();
		}
		SameLine();
		if (Button("Join"))
		{
			StartClient();
		}
	}
	else
	{
		if (Button("Disconnect"))
		{
			StopConnection();
			SetStatus("Disconnected");
		}

		SameLine();
		Text("%s", Mode == ConnectionMode::Host ? "Hosting" : "Connected as client");
	}

	Separator();

	if (Mode == ConnectionMode::Host)
	{
		SeparatorText("Match setup");

		InputText("Map folder", MapFolderBuffer, sizeof(MapFolderBuffer));
		MapFolder = MapFolderBuffer;

		SameLine();
		if (Button("Browse"))
		{
			std::string Selected = "";
			if (BrowseForFolder(Selected))
			{
				MapFolder = Selected;
				RefreshBuffers();
			}
		}

		SameLine();
		if (Button("Rescan"))
		{
			RescanMaps();
		}

		Text("Map pool: %d", (int)MapPoolPaths.size());

		bool Changed = false;

		bool ConfigEditable = Match.Phase == BingoPhase::Lobby || Match.Phase == BingoPhase::Idle;

		if (SliderInt("Board size", &BoardSize, 3, 7, "%d"))
		{
			Changed = true;
			if (ConfigEditable)
			{
				Match.Board.clear();
				SyncTeams();
			}
		}

		if (SliderInt("Team count", &TeamCount, 2, 8, "%d"))
		{
			Changed = true;
			if (ConfigEditable) SyncTeams();
		}

		if (Combo("Medal rule", &MedalRule, MedalRuleNames, IM_ARRAYSIZE(MedalRuleNames)))
		{
			Changed = true;
			if (ConfigEditable) SyncTeams();
		}

		if (InputInt("Match minutes", &MatchMinutes))
		{
			MatchMinutes = std::clamp(MatchMinutes, 0, 1440);
			Changed = true;
			if (ConfigEditable) SyncTeams();
		}

		if (InputInt("Grace minutes", &GraceMinutes))
		{
			GraceMinutes = std::clamp(GraceMinutes, 0, 120);
			Changed = true;
			if (ConfigEditable) SyncTeams();
		}

		if (Changed && ConfigEditable)
		{
			BroadcastState();
		}

		if (Button("Generate board"))
		{
			GenerateBoard();
		}

		SameLine();
		if (Button("Start match"))
		{
			StartMatch();
		}

		if (Match.Phase == BingoPhase::Running)
		{
			SameLine();
			if (Button("Stop match"))
			{
				FinishMatch(-1, true);
			}
		}
		else if (Match.Phase == BingoPhase::Finished)
		{
			SameLine();
			if (Button("Return to lobby"))
			{
				Match.Phase = BingoPhase::Lobby;
				Match.WinnerTeam = -1;
				Match.IsDraw = false;
				for (BingoSquare& Square : Match.Board)
				{
					Square.OwnerTeam = -1;
					Square.RecordMs = INT_MAX;
					Square.RecordPlayer.clear();
				}
				BroadcastState();
			}
		}
	}

	if (Match.Phase != BingoPhase::Idle)
	{
		SeparatorText("Match");

		const char* PhaseName = Match.Phase == BingoPhase::Lobby ? "Lobby" :
			Match.Phase == BingoPhase::Running ? "Running" : "Finished";
		Text("Phase: %s", PhaseName);
		Text("Board: %dx%d", Match.Settings.BoardSize, Match.Settings.BoardSize);
		Text("Teams: %d", (int)Match.Teams.size());
		Text("Players: %d", (int)Match.Players.size());

		if (Match.Phase == BingoPhase::Running && Mode == ConnectionMode::Host)
		{
			auto Now = std::chrono::steady_clock::now();
			if (MatchTimerEnabled)
			{
				auto Left = std::chrono::duration_cast<std::chrono::seconds>(MatchEndTime - Now).count();
				Text("Time left: %lld:%02lld", (long long)std::max<long long>(0, Left) / 60, (long long)std::max<long long>(0, Left) % 60);
			}
			else
			{
				Text("Time left: unlimited");
			}

			if (!GraceEvaluated)
			{
				auto Left = std::chrono::duration_cast<std::chrono::seconds>(GraceEndTime - Now).count();
				Text("Grace left: %lld:%02lld", (long long)std::max<long long>(0, Left) / 60, (long long)std::max<long long>(0, Left) % 60);
			}
		}

		if (Match.Phase == BingoPhase::Finished)
		{
			if (Match.IsDraw)
			{
				Text("Result: Draw");
			}
			else if (Match.WinnerTeam >= 0 && Match.WinnerTeam < (int)Match.Teams.size())
			{
				Text("Result: ");
				SameLine(0.f, 0.f);
				TextColored(TeamColor(Match.WinnerTeam), "%s", Match.Teams[Match.WinnerTeam].Name.c_str());
			}
		}

		if (Mode == ConnectionMode::Host || Connected)
		{
			BingoPlayer* LocalPlayer = GetLocalPlayer();
			if (LocalPlayer && !Match.Teams.empty())
			{
				const char* CurrentTeamName = "Team";
				LocalPlayer->TeamId = std::clamp(LocalPlayer->TeamId, 0, (int)Match.Teams.size() - 1);
				CurrentTeamName = Match.Teams[LocalPlayer->TeamId].Name.c_str();

				if (BeginCombo("Your team", CurrentTeamName))
				{
					for (const BingoTeam& Team : Match.Teams)
					{
						bool Selected = LocalPlayer->TeamId == Team.Id;
						if (Selectable(Team.Name.c_str(), Selected))
						{
							if (Mode == ConnectionMode::Host)
							{
								LocalPlayer->TeamId = Team.Id;
								BroadcastState();
							}
							else
							{
								Writer Out;
								Out.U8(MSG_SET_TEAM);
								Out.I32(Team.Id);
								Net.SendToServer(Out.Data);
							}
						}
					}
					EndCombo();
				}
			}
		}

		for (const BingoPlayer& Player : Match.Players)
		{
			const char* TeamName = "None";
			if (Player.TeamId >= 0 && Player.TeamId < (int)Match.Teams.size())
			{
				TeamName = Match.Teams[Player.TeamId].Name.c_str();
			}

			ImVec4 Color = TeamColor(Player.TeamId);
			TextColored(Color, "%s", Player.Name.c_str());
			SameLine(0.f, 0.f);
			Text(" - ");
			SameLine(0.f, 0.f);
			TextColored(Color, "%s", TeamName);
		}
	}
}
