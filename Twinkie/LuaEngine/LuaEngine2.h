#pragma once

#ifdef _MSC_VER
#define TWINKIE_DLLIMPORT __declspec(dllimport)
#else
#define TWINKIE_DLLIMPORT
#endif
#include <string>
#include "include/lua.hpp"
#include "../imgui-dx9/imgui_internal.h"

struct ModuleInfo
{
	std::string Name = "";
	std::string Version = "";
	std::string Author = "";
	std::string Description = "";
};

extern lua_CFunction PrintFunc;

extern "C" 
{
	TWINKIE_DLLIMPORT lua_State* GetLuaState(const char* StateName);
	TWINKIE_DLLIMPORT bool LuaInited(const char* StateName);
	TWINKIE_DLLIMPORT void SetLuaPrintFunction(lua_CFunction PrintFunction);
	TWINKIE_DLLIMPORT void SetLuaPrintFunctionForState(const char* StateName, lua_CFunction PrintFunction);
	TWINKIE_DLLIMPORT void ExportImGuiForState(const char* StateName);
	TWINKIE_DLLIMPORT void InitLua(const char* StateName);
	TWINKIE_DLLIMPORT void RunMain(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunRender(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunRenderInterface(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunRenderMenuItem(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunRenderMainMenuItem(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunRenderSettings(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunLua(const char* StateName, const char* LuaScript, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void RunLuaFile(const char* StateName, const char* FileName, char* ErrorStrBuffer, size_t ErrorStrSize);
	TWINKIE_DLLIMPORT void UpdatePrintFunction(const char* StateName);
	TWINKIE_DLLIMPORT void CloseLua(const char* StateName);
	TWINKIE_DLLIMPORT void CloseLuaAll();
}

TWINKIE_DLLIMPORT ModuleInfo RunModuleInfo(const char* StateName, char* ErrorStrBuffer, size_t ErrorStrSize);
TWINKIE_DLLIMPORT void SetImGuiContext(ImGuiContext* Ctx);