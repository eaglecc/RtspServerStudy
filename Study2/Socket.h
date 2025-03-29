#pragma once
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>

class Socket
{
public:
    Socket();
    static int createTcpSocket();
    static int createUdpSocket();
    static int bindSocketAddr(int sockfd, const char* ip, int port);
    static int Listen(int socketfd,int listenNum);
    static int acceptClient(int sockfd, char* ip, int* port);
    static int Closesocket(int tcpSocket);
};

