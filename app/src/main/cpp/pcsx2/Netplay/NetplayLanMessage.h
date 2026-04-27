// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from pcsx2-online Message.h — removed App.h dependency

#pragma once

#include <cstring>
#include <algorithm>
#include "NetplayLanEmulatorState.h"
#include "shoryu/archive.h"

#define NETPLAY_ANALOG_STICKS
#ifdef NETPLAY_ANALOG_STICKS
#define NETPLAY_SYNC_NUM_INPUTS 6
#else
#define NETPLAY_SYNC_NUM_INPUTS 2
#endif

struct NetplayLanMessage
{
	char input[6];

	NetplayLanMessage()
	{
		static const unsigned char defaultInput[] = {0xff, 0xff, 0x7f, 0x7f, 0x7f, 0x7f};
		std::copy(defaultInput, defaultInput + sizeof(defaultInput), input);
	}

	void serialize(shoryu::oarchive& a) const
	{
		a.write((char*)input, NETPLAY_SYNC_NUM_INPUTS);
	}

	void deserialize(shoryu::iarchive& a)
	{
		a.read(input, NETPLAY_SYNC_NUM_INPUTS);
	}
};
