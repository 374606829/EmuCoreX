// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>

namespace NetplayFileHash
{
	// Android: filepath is a native filesystem path (content:// URIs should be resolved before calling)
	std::string CalculateFileMD5(const std::string& filepath);
	std::string CalculateFileCRC32(const std::string& filepath);
} // namespace NetplayFileHash
