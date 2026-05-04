// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from pcsx2-online EmulatorState.h — removed wx/App.h dependency

#pragma once

#include <cstdint>
#include <cstring>
#include "shoryu/archive.h"

struct EmulatorSyncState
{
	EmulatorSyncState()
	{
		memset(biosVersion, 0, sizeof(biosVersion));
		memset(discId, 0, sizeof(discId));
		skipMpeg = false;
	}

	char biosVersion[35];
	char discId[15];
	bool skipMpeg;

	void serialize(shoryu::oarchive& a) const
	{
		a.write((char*)discId, sizeof(discId));
		a.write((char*)biosVersion, sizeof(biosVersion));
		a.write((char*)&skipMpeg, sizeof(skipMpeg));
	}

	void deserialize(shoryu::iarchive& a)
	{
		a.read((char*)discId, sizeof(discId));
		a.read((char*)biosVersion, sizeof(biosVersion));
		a.read((char*)&skipMpeg, sizeof(skipMpeg));
	}
};
