#pragma once

#ifndef _MSC_VER
#define TWINKIE_TMF_IMPORT
#else
#define TWINKIE_TMF_IMPORT __declspec(dllimport)
#endif

#ifndef _MSC_VER
#define TWINKIE_TMF_NO_PRAGMA
#endif

#ifndef TWINKIE_TMF_NO_PRAGMA
#pragma comment(lib, "TmForeverLib/TmForever.lib")
#endif

#define TMF_API TWINKIE_TMF_IMPORT

class CFastStringInt
{
public:
    int Size;
    wchar_t* Cstr;

    TWINKIE_TMF_IMPORT __thiscall CFastStringInt(wchar_t const*);
};

class CMwNod {
public:
    void** vftable;
    unsigned int m_ReferenceCount;
    uintptr_t m_SystemFid;
    TM::CFastBuffer<CMwNod*>* m_Dependants;
    unsigned int u1;
};

class CSystemFids : public CMwNod
{

};

class CClassicArchive : public CMwNod
{
};

class TWINKIE_TMF_IMPORT CSystemArchiveNod : public CClassicArchive
{
public:
    enum EArchive
    {
        ZERO,
        ONE,
        TWO,
        THREE,
        FOUR,
        FIVE,
        SIX,
        SEVEN // <-- always use this one
    };

    static int __cdecl LoadFileFrom(CFastStringInt const&, CMwNod*&, CSystemFids*, EArchive);
};

class TWINKIE_TMF_IMPORT CSystemEngine : public CMwNod
{
public:
    CSystemFids* __thiscall GetLocationData();
};