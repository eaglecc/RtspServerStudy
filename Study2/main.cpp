#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include "RTP.h"
#include "Socket.h"
#include "RTSPUtils.h"

#define H264_FILE_NAME   "../data/star.h264"
#define SERVER_PORT      8554
#define SERVER_RTP_PORT  55532
#define SERVER_RTCP_PORT 55533
#define BUF_MAX_SIZE     (1024*1024)

static inline int startCode3(char* buf)
{
    if (buf[0] == 0 && buf[1] == 0 && buf[2] == 1)
        return 1;
    else
        return 0;
}

static inline int startCode4(char* buf)
{
    if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1)
        return 1;
    else
        return 0;
}

static char* findNextStartCode(char* buf, int len)
{
    int i;

    if (len < 3)
        return NULL;

    for (i = 0; i < len - 3; ++i)
    {
        if (startCode3(buf) || startCode4(buf))
            return buf;

        ++buf;
    }

    if (startCode3(buf))
        return buf;

    return NULL;
}

// 逐帧读取H264文件（NAL数据）
static int getFrameFromH264File(FILE* fp, char* frame, int size) {
    if (fp == NULL || frame == NULL || size <= 0)
        return -1;

    int rSize = fread(frame, 1, size, fp);
    if (rSize <= 0) {
        return -1;
    }

    if (!startCode3(frame) && !startCode4(frame))
        return -1;

    char* nextStartCode = findNextStartCode(frame + 3, rSize - 3);
    if (!nextStartCode) {
        return -1;
    }

    int frameSize = (nextStartCode - frame);
    fseek(fp, frameSize - rSize, SEEK_CUR);

    return frameSize;
}

static int rtpSendH264Frame(int serverRtpSockfd, const char* ip, int16_t port,
    struct RtpPacket* rtpPacket, char* frame, uint32_t frameSize)
{

    uint8_t naluType; // nalu第一个字节
    int sendBytes = 0;
    int ret;

    naluType = frame[0];

    printf("frameSize=%d \n", frameSize);

    if (frameSize <= RTP_MAX_PKT_SIZE) // nalu长度小于最大包长：（单NALU打包：一个RTP包 包含一个完整的 NALU）
    {

        //*   0 1 2 3 4 5 6 7 8 9
        //*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        //*  |F|NRI|  Type   | a single NAL unit ... |
        //*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        memcpy(rtpPacket->payload, frame, frameSize);
        ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, frameSize);
        if (ret < 0)
            return -1;

        rtpPacket->rtpHeader.seq++;
        sendBytes += ret;
        if ((naluType & 0x1F) == 7 || (naluType & 0x1F) == 8) // 如果是SPS、PPS就不需要加时间戳
            goto out;
    }
    else // nalu长度小于最大包场：分片模式
    {

        //*  0                   1                   2
        //*  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
        //* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        //* | FU indicator  |   FU header   |   FU payload   ...  |
        //* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        //*     FU Indicator
        //*    0 1 2 3 4 5 6 7
        //*   +-+-+-+-+-+-+-+-+
        //*   |F|NRI|  Type   |
        //*   +---------------+

        //*      FU Header
        //*    0 1 2 3 4 5 6 7
        //*   +-+-+-+-+-+-+-+-+
        //*   |S|E|R|  Type   |
        //*   +---------------+


        int pktNum = frameSize / RTP_MAX_PKT_SIZE;       // 有几个完整的包
        int remainPktSize = frameSize % RTP_MAX_PKT_SIZE; // 剩余不完整包的大小
        int i, pos = 1;

        // 发送完整的包
        for (i = 0; i < pktNum; i++)
        {
            rtpPacket->payload[0] = (naluType & 0x60) | 28;
            rtpPacket->payload[1] = naluType & 0x1F;

            if (i == 0) //第一包数据
                rtpPacket->payload[1] |= 0x80; // start
            else if (remainPktSize == 0 && i == pktNum - 1) //最后一包数据
                rtpPacket->payload[1] |= 0x40; // end

            memcpy(rtpPacket->payload + 2, frame + pos, RTP_MAX_PKT_SIZE);
            ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, RTP_MAX_PKT_SIZE + 2);
            if (ret < 0)
                return -1;

            rtpPacket->rtpHeader.seq++;
            sendBytes += ret;
            pos += RTP_MAX_PKT_SIZE;
        }

        // 发送剩余的数据
        if (remainPktSize > 0)
        {
            rtpPacket->payload[0] = (naluType & 0x60) | 28;
            rtpPacket->payload[1] = naluType & 0x1F;
            rtpPacket->payload[1] |= 0x40; //end

            memcpy(rtpPacket->payload + 2, frame + pos, remainPktSize + 2);
            ret = rtpSendPacketOverUdp(serverRtpSockfd, ip, port, rtpPacket, remainPktSize + 2);
            if (ret < 0)
                return -1;

            rtpPacket->rtpHeader.seq++;
            sendBytes += ret;
        }
    }
    rtpPacket->rtpHeader.timestamp += 90000 / 25;
out:

    return sendBytes;

}

static void doClient(int clientSockfd, const char* clientIP, int clientPort) {

    char method[40];
    char url[100];
    char version[40];
    int CSeq;

    int serverRtpSockfd = -1, serverRtcpSockfd = -1;
    int clientRtpPort, clientRtcpPort;
    char* rBuf = (char*)malloc(BUF_MAX_SIZE);
    char* sBuf = (char*)malloc(BUF_MAX_SIZE);
    if (!rBuf || !sBuf) {
        printf("Memory allocation failed\n");
        if (rBuf) free(rBuf);
        if (sBuf) free(sBuf);
        closesocket(clientSockfd);
        return;
    }

    while (true) {
        int recvLen;

        recvLen = recv(clientSockfd, rBuf, BUF_MAX_SIZE, 0);
        if (recvLen <= 0) {
            break;
        }

        rBuf[recvLen] = '\0';
        printf("%s rBuf = %s \n", __FUNCTION__, rBuf);

        const char* sep = "\n";
        char* line = strtok(rBuf, sep); // 按照seq分割字符串
        while (line) {
            // strstr：查找字符串line中是否包含（"OPTIONS"、"DESCRIBE"、"SETUP"、"PLAY"）字符串，返回 needle 在 haystack 中的指针，如果找不到返回 nullptr。
            if (strstr(line, "OPTIONS") ||
                strstr(line, "DESCRIBE") ||
                strstr(line, "SETUP") ||
                strstr(line, "PLAY")) {

                if (sscanf(line, "%s %s %s\r\n", method, url, version) != 3) {
                    // error
                }
            }
            else if (strstr(line, "CSeq")) {
                if (sscanf(line, "CSeq: %d\r\n", &CSeq) != 1) {
                    // error
                }
            }
            else if (!strncmp(line, "Transport:", strlen("Transport:"))) {
                // Transport: RTP/AVP/UDP;unicast;client_port=13358-13359
                // Transport: RTP/AVP;unicast;client_port=13358-13359

                if (sscanf(line, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                    &clientRtpPort, &clientRtcpPort) != 2) {
                    // error
                    printf("parse Transport error \n");
                }
            }
            line = strtok(NULL, sep); // line 会在调用 strtok 函数时变为下一个子字符串。
        }

        if (!strcmp(method, "OPTIONS")) {
            if (RTSPUtils::handleCmd_OPTIONS(sBuf, CSeq))
            {
                printf("failed to handle options\n");
                break;
            }
        }
        else if (!strcmp(method, "DESCRIBE")) {
            if (RTSPUtils::handleCmd_DESCRIBE(sBuf, CSeq, url))
            {
                printf("failed to handle describe\n");
                break;
            }
        }
        else if (!strcmp(method, "SETUP")) {
            if (RTSPUtils::handleCmd_SETUP(sBuf, CSeq, clientRtpPort))
            {
                printf("failed to handle setup\n");
                break;
            }

            serverRtpSockfd = Socket::createUdpSocket();
            serverRtcpSockfd = Socket::createUdpSocket();

            if (serverRtpSockfd < 0 || serverRtcpSockfd < 0)
            {
                printf("failed to create udp socket\n");
                break;
            }

            if (Socket::bindSocketAddr(serverRtpSockfd, "0.0.0.0", SERVER_RTP_PORT) < 0 ||
                Socket::bindSocketAddr(serverRtcpSockfd, "0.0.0.0", SERVER_RTCP_PORT) < 0)
            {
                printf("failed to bind addr\n");
                break;
            }

        }
        else if (!strcmp(method, "PLAY")) {
            if (RTSPUtils::handleCmd_PLAY(sBuf, CSeq))
            {
                printf("failed to handle play\n");
                break;
            }
        }
        else {
            printf("未定义的method = %s \n", method);
            break;
        }
        //printf("sBuf = %s \n", sBuf);
        printf("%s sBuf = %s \n", __FUNCTION__, sBuf);

        send(clientSockfd, sBuf, strlen(sBuf), 0);


        //开始播放，发送RTP包
        if (!strcmp(method, "PLAY")) {

            int frameSize, startCode;
            char* frame = (char*)malloc(500000);
            struct RtpPacket* rtpPacket = (struct RtpPacket*)malloc(500000);
            FILE* fp = fopen(H264_FILE_NAME, "rb");
            if (!fp) {
                printf("读取 %s 失败\n", H264_FILE_NAME);
                break;
            }
            rtpHeaderInit(rtpPacket, 0, 0, 0, RTP_VESION, RTP_PAYLOAD_TYPE_H264, 0,
                0, 0, 0x88923423);

            printf("start play\n");
            printf("client ip:%s\n", clientIP);
            printf("client port:%d\n", clientRtpPort);

            while (true) {
                frameSize = getFrameFromH264File(fp, frame, 500000);
                if (frameSize < 0)
                {
                    printf("读取%s结束,frameSize=%d \n", H264_FILE_NAME, frameSize);
                    break;
                }

                if (startCode3(frame))
                    startCode = 3;
                else
                    startCode = 4;

                frameSize -= startCode;
                rtpSendH264Frame(serverRtpSockfd, clientIP, clientRtpPort,
                    rtpPacket, frame + startCode, frameSize);

                Sleep(40);
                //usleep(40000);//1000/25 * 1000
            }
            free(frame);
            free(rtpPacket);

            break;
        }

        memset(method, 0, sizeof(method) / sizeof(char));
        memset(url, 0, sizeof(url) / sizeof(char));
        CSeq = 0;

    }

    closesocket(clientSockfd);
    if (serverRtpSockfd) {
        closesocket(serverRtpSockfd);
    }
    if (serverRtcpSockfd > 0) {
        closesocket(serverRtcpSockfd);
    }

    free(rBuf);
    free(sBuf);

}


int main(int argc, char* argv[])
{
    Socket socketInstance;
    int tcpSocket = Socket::createTcpSocket();
    if (tcpSocket < 0) {
        std::cerr << "Failed to create TCP socket" << std::endl;
        return -1;
    }

    int udpSocket = Socket::createUdpSocket();
    if (udpSocket < 0) {
        std::cerr << "Failed to create UDP socket" << std::endl;
        return -1;
    }


    if (Socket::bindSocketAddr(tcpSocket, "0.0.0.0", SERVER_PORT) < 0)
    {
        printf("failed to bind addr\n");
        return -1;
    }

    if (Socket::Listen(tcpSocket, 10) < 0)
    {
        printf("failed to listen\n");
        return -1;
    }

    printf("%s rtsp://127.0.0.1:%d\n", __FILE__, SERVER_PORT);

    while (true) {
        int clientSockfd;
        char clientIp[40];
        int clientPort;

        // RTSP 是基于TCP的，所以需要accept函数接收客户端连接
        clientSockfd = Socket::acceptClient(tcpSocket, clientIp, &clientPort);
        if (clientSockfd < 0)
        {
            printf("failed to accept client\n");
            return -1;
        }

        printf("accept client;client ip:%s,client port:%d\n", clientIp, clientPort);

        doClient(clientSockfd, clientIp, clientPort);
    }
    Socket::Closesocket(tcpSocket);

    return 0;
}