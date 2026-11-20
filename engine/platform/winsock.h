#pragma once
#include "windows.h"
#include <netdb.h>
#include <unistd.h>
struct WSAData {WORD wVersion,wHighVersion;char szDescription[257],szSystemStatus[129];WORD iMaxSockets,iMaxUdpDg;char*lpVendorInfo;};typedef WSAData WSADATA;
extern "C" {int WSAStartup(WORD,WSADATA*);int WSACleanup(void);}
