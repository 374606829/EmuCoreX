// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ReShadePresetINI.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
	// Trim whitespace from both ends of `s` in-place. The MVP preset uses
	// only ASCII whitespace, but UTF-8 BOM at the start of a value is also
	// stripped here.
	void TrimInPlace(std::string& s)
	{
		auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
		while (!s.empty() && is_space(static_cast<unsigned char>(s.front())))
			s.erase(s.begin());
		while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))
			s.pop_back();
		// Strip optional UTF-8 BOM bytes (\xEF\xBB\xBF) if present.
		if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
			static_cast<unsigned char>(s[1]) == 0xBB &&
			static_cast<unsigned char>(s[2]) == 0xBF)
		{
			s.erase(0, 3);
		}
	}

	// Split a comma-separated string, trimming each piece. Empty pieces
	// (caused by trailing or duplicate commas) are dropped.
	std::vector<std::string> SplitCSV(const std::string& s)
	{
		std::vector<std::string> out;
		std::string cur;
		for (char c : s)
		{
			if (c == ',')
			{
				TrimInPlace(cur);
				if (!cur.empty())
					out.push_back(std::move(cur));
				cur.clear();
			}
			else
			{
				cur.push_back(c);
			}
		}
		TrimInPlace(cur);
		if (!cur.empty())
			out.push_back(std::move(cur));
		return out;
	}

	// Read a whole text file into a string. Used over FileSystem::ReadFileToString
	// directly so that the parser stays trivially testable on non-PCSX2 hosts.
	bool ReadWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f.is_open())
			return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}
} // namespace

namespace ReShade
{
	void Preset::Clear()
	{
		chain.clear();
		effect_params.clear();
		has_techniques_line = false;
	}

	bool Preset::HasParametersFor(const std::string& effect_filename) const
	{
		auto it = effect_params.find(effect_filename);
		return it != effect_params.end() && !it->second.empty();
	}

	bool ParsePresetINI(const std::string& ini_path, Preset& out, std::string* error_out)
	{
		out.Clear();

		std::string contents;
		if (!ReadWholeFile(ini_path, contents))
		{
			if (error_out)
				*error_out = "Failed to open preset file: " + ini_path;
			return false;
		}

		// Some PC-saved INIs use CRLF line endings; std::getline copes with
		// LF, so we just normalize CRLF -> LF up-front.
		std::string normalized;
		normalized.reserve(contents.size());
		for (size_t i = 0; i < contents.size(); ++i)
		{
			if (contents[i] == '\r')
				continue;
			normalized.push_back(contents[i]);
		}

		std::istringstream stream(normalized);
		std::string line;
		std::string current_section; // empty == global
		size_t line_no = 0;

		while (std::getline(stream, line))
		{
			++line_no;
			TrimInPlace(line);
			if (line.empty())
				continue;
			if (line.front() == ';' || line.front() == '#')
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				current_section = line.substr(1, line.size() - 2);
				TrimInPlace(current_section);
				continue;
			}

			const auto eq = line.find('=');
			if (eq == std::string::npos)
			{
				Console.Warning("ReShade preset %s:%zu skipping malformed line: %s",
					ini_path.c_str(), line_no, line.c_str());
				continue;
			}

			std::string key = line.substr(0, eq);
			std::string value = line.substr(eq + 1);
			TrimInPlace(key);
			TrimInPlace(value);
			if (key.empty())
				continue;

			if (current_section.empty())
			{
				if (key == "Techniques")
				{
					out.has_techniques_line = true;
					for (const std::string& tok : SplitCSV(value))
					{
						TechniqueRef ref;
						const auto at = tok.find('@');
						if (at == std::string::npos)
						{
							ref.technique_name.clear();
							ref.effect_filename = tok;
						}
						else
						{
							ref.technique_name = tok.substr(0, at);
							ref.effect_filename = tok.substr(at + 1);
							TrimInPlace(ref.technique_name);
							TrimInPlace(ref.effect_filename);
						}
						if (!ref.effect_filename.empty())
							out.chain.push_back(std::move(ref));
					}
				}
				// Other top-level keys (TechniqueSorting / EffectSearchPaths /
				// TextureSearchPaths / PreprocessorDefinitions) are not used
				// by the MVP and silently ignored. They can be wired in once
				// the runtime supports per-effect search-path resolution.
				continue;
			}

			out.effect_params[current_section][key] = value;
		}

		return true;
	}

	bool ToBool(const std::string& s, bool default_value)
	{
		if (s.empty())
			return default_value;
		if (s == "1" || s == "true"  || s == "TRUE"  || s == "True")  return true;
		if (s == "0" || s == "false" || s == "FALSE" || s == "False") return false;
		return default_value;
	}

	// PCSX2 is built with `-fno-exceptions`, so we cannot rely on the throwing
	// std::stoi / std::stof variants. Use strtol / strtof and inspect errno +
	// the end pointer to mimic "default_value on parse failure" semantics.
	int ToInt(const std::string& s, int default_value)
	{
		if (s.empty())
			return default_value;
		errno = 0;
		char* end = nullptr;
		const long v = std::strtol(s.c_str(), &end, 10);
		if (errno != 0 || end == s.c_str())
			return default_value;
		if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
			return default_value;
		return static_cast<int>(v);
	}

	float ToFloat(const std::string& s, float default_value)
	{
		if (s.empty())
			return default_value;
		errno = 0;
		char* end = nullptr;
		const float v = std::strtof(s.c_str(), &end);
		if (errno != 0 || end == s.c_str())
			return default_value;
		return v;
	}

	std::vector<float> ToFloatList(const std::string& s)
	{
		std::vector<float> out;
		for (const std::string& tok : SplitCSV(s))
		{
			errno = 0;
			char* end = nullptr;
			const float v = std::strtof(tok.c_str(), &end);
			if (errno != 0 || end == tok.c_str())
				break;
			out.push_back(v);
		}
		return out;
	}
} // namespace ReShade
