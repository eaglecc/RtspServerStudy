#pragma once
#include <stdio.h>
#include <time.h>
#include <string.h>

#define SERVER_RTP_PORT  55532
#define SERVER_RTCP_PORT 55533

class RTSPUtils
{
public:

    static int handleCmd_OPTIONS(char* result, int cseq);
    static int handleCmd_DESCRIBE(char* result, int cseq, char* url);
    static int handleCmd_SETUP(char* result, int cseq, int clientRtpPort);
    static int handleCmd_PLAY(char* result, int cseq);

};

