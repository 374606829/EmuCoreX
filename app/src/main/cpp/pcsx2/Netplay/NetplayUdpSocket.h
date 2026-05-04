// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <cstddef> // for intptr_t
typedef uintptr_t np_socket_t;
#define NP_INVALID_SOCKET (~(np_socket_t)0)
#else
typedef int np_socket_t;
#define NP_INVALID_SOCKET (-1)
#endif

namespace NetplayUdpSocket
{
	bool Init();
	void Shutdown();
	np_socket_t Open(unsigned short local_port, bool non_blocking);
	void Close(np_socket_t sock);
	int SendTo(np_socket_t sock, const char* host, unsigned short port, const uint8_t* data, int len);
	int RecvFrom(np_socket_t sock, uint8_t* buf, int buf_len, std::string& sender_ip, unsigned short& sender_port);
} // namespace NetplayUdpSocket
