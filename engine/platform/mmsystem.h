#pragma once
#include "windows.h"
typedef HANDLE HMIXER,HMIXEROBJ,HMMIO;typedef UINT MMRESULT,MCIDEVICEID;typedef DWORD MMVERSION,FOURCC;
#pragma pack(push,1)
struct tWAVEFORMATEX{WORD wFormatTag,nChannels;DWORD nSamplesPerSec,nAvgBytesPerSec;WORD nBlockAlign,wBitsPerSample,cbSize;};
typedef tWAVEFORMATEX WAVEFORMATEX,*LPWAVEFORMATEX;typedef const WAVEFORMATEX*LPCWAVEFORMATEX;
#pragma pack(pop)
struct tagMIXERLINEA{DWORD cbStruct,dwDestination,dwSource,dwLineID,fdwLine;DWORD_PTR dwUser;DWORD dwComponentType,cChannels,cConnections,cControls;CHAR szShortName[16],szName[64];struct{DWORD dwType,dwDeviceID;WORD wMid,wPid;MMVERSION vDriverVersion;CHAR szPname[32];}Target;};typedef tagMIXERLINEA MIXERLINEA,*LPMIXERLINEA;
struct tagMIXERCONTROLA{DWORD cbStruct,dwControlID,dwControlType,fdwControl,cMultipleItems;CHAR szShortName[16],szName[64];union{struct{LONG lMinimum,lMaximum;};struct{DWORD dwMinimum,dwMaximum;};DWORD dwReserved[6];}Bounds;union{DWORD cSteps,cbCustomData,dwReserved[6];}Metrics;};typedef tagMIXERCONTROLA MIXERCONTROLA,*LPMIXERCONTROLA;
struct tagMIXERLINECONTROLSA{DWORD cbStruct,dwLineID;union{DWORD dwControlID,dwControlType;};DWORD cControls,cbmxctrl;LPMIXERCONTROLA pamxctrl;};typedef tagMIXERLINECONTROLSA MIXERLINECONTROLSA;
struct tMIXERCONTROLDETAILS{DWORD cbStruct,dwControlID,cChannels;union{HWND hwndOwner;DWORD cMultipleItems;};DWORD cbDetails;LPVOID paDetails;};typedef tMIXERCONTROLDETAILS MIXERCONTROLDETAILS;
struct tMIXERCONTROLDETAILS_UNSIGNED{DWORD dwValue;};typedef tMIXERCONTROLDETAILS_UNSIGNED MIXERCONTROLDETAILS_UNSIGNED;
struct _MMCKINFO{FOURCC ckid;DWORD cksize;FOURCC fccType;DWORD dwDataOffset,dwFlags;};typedef _MMCKINFO MMCKINFO,*LPMMCKINFO;
extern "C" DWORD timeGetTime(void);
typedef DWORD MCIERROR;
struct MCI_GENERIC_PARMS{DWORD_PTR dwCallback;};
struct MCI_PLAY_PARMS{DWORD_PTR dwCallback;DWORD dwFrom,dwTo;};
struct MCI_OPEN_PARMSA{DWORD_PTR dwCallback;MCIDEVICEID wDeviceID;LPCSTR lpstrDeviceType,lpstrElementName,lpstrAlias;};
struct tagMCI_STATUS_PARMS{DWORD_PTR dwCallback;DWORD_PTR dwReturn;DWORD dwItem,dwTrack;};
struct MCI_SET_PARMS{DWORD_PTR dwCallback;DWORD dwTimeFormat,dwAudio;};
struct MCI_SEEK_PARMS{DWORD_PTR dwCallback;DWORD dwTo;};
extern "C" {MCIERROR mciSendCommandA(MCIDEVICEID,UINT,DWORD_PTR,DWORD_PTR);BOOL mciGetErrorStringA(MCIERROR,LPSTR,UINT);}

typedef MCI_OPEN_PARMSA MCI_OPEN_PARMS;typedef tagMCI_STATUS_PARMS MCI_STATUS_PARMS;
extern "C" {MMRESULT mixerGetLineInfoA(HANDLE,MIXERLINEA*,DWORD);MMRESULT mixerGetLineControlsA(HANDLE,MIXERLINECONTROLSA*,DWORD);MMRESULT mixerGetControlDetailsA(HANDLE,MIXERCONTROLDETAILS*,DWORD);MMRESULT mixerSetControlDetails(HANDLE,MIXERCONTROLDETAILS*,DWORD);}
