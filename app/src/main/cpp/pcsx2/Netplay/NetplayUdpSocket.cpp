// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#endif

#include "Netplay/NetplayUdpSocket.h"

#include <cstring>

namespace NetplayUdpSocket
{

#ifdef _WIN32
static bool s_wsa_inited = false;
#endif

bool Init()
{
#ifdef _WIN32
	if (!s_wsa_inited)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return false;
		s_wsa_inited = true;
	}
#endif
	return true;
}

void Shutdown()
{
#ifdef _WIN32
	if (s_wsa_inited)
	{
		WSACleanup();
		s_wsa_inited = false;
	}
#endif
}

np_socket_t Open(unsigned short local_port, bool non_blocking)
{
#ifdef _WIN32
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return NP_INVALID_SOCKET;
#else
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		return NP_INVALID_SOCKET;
#endif

	if (local_port > 0)
	{
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(local_port);
		addr.sin_addr.s_addr = INADDR_ANY;
#ifdef _WIN32
		if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
#else
		if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
#endif
		{
			Close((np_socket_t)sock);
			return NP_INVALID_SOCKET;
		}
	}

	if (non_blocking)
	{
#ifdef _WIN32
		u_long mode = 1;
		ioctlsocket(sock, FIONBIO, &mode);
#else
		int flags = fcntl(sock, F_GETFL, 0);
		fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
	}

	return (np_socket_t)sock;
}

void Close(np_socket_t sock)
{
	if (sock == NP_INVALID_SOCKET)
		return;
#ifdef _WIN32
	closesocket((SOCKET)sock);
#else
	close((int)sock);
#endif
}

int SendTo(np_socket_t sock, const char* host, unsigned short port, const uint8_t* data, int len)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, nullptr, &hints, &res) != 0)
		return -1;
	addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
	freeaddrinfo(res);

#ifdef _WIN32
	return (int)sendto((SOCKET)sock, (const char*)data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
#else
	return (int)sendto((int)sock, (const char*)data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
#endif
}

int RecvFrom(np_socket_t sock, uint8_t* buf, int buf_len, std::string& sender_ip, unsigned short& sender_port)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
#ifdef _WIN32
	int addr_len = sizeof(addr);
	int n = (int)recvfrom((SOCKET)sock, (char*)buf, buf_len, 0, (struct sockaddr*)&addr, &addr_len);
#else
	socklen_t addr_len = sizeof(addr);
	int n = (int)recvfrom((int)sock, (char*)buf, buf_len, 0, (struct sockaddr*)&addr, &addr_len);
#endif
	if (n > 0)
	{
		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
		sender_ip = ip_str;
		sender_port = ntohs(addr.sin_port);
	}
	return n;
}

} // namespace NetplayUdpSocket
