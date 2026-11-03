#pragma once
#include <algorithm>
#include <cwctype>
#include <numeric>
#include <set>
#include <string>
#include <vector>

inline bool ends_with(const std::wstring& wstr, const std::wstring& wsub)
{
	if (wstr.size() < wsub.size())
		return false;
	else
		return std::equal(wsub.rbegin(), wsub.rend(), wstr.rbegin());
}

inline bool ireplace_last(std::wstring& input, const std::wstring& search, const std::wstring& sub)
{
	std::size_t pos = input.rfind(search);
	if (pos != std::wstring::npos) {
		input.replace(pos, search.length(), sub);
		return true;
	}
	return false;
}

inline std::string join(const std::set<std::string>& list, const std::string& delim)
{
	return std::accumulate(list.begin(), list.end(), std::string(), [&delim](std::string& str1, const std::string& str2)
	{
		return str1.empty() ? str2 : str1 + delim + str2;
	});
}

inline std::vector<std::wstring>& split(std::vector<std::wstring>& result, const std::wstring& input, const wchar_t* delim)
{
	result.clear();
	size_t current = 0;
	size_t next = std::wstring::npos;
	do
	{
		current = next + 1;
		next = input.find_first_of(delim, next + 1);
		result.push_back(input.substr(current, next - current));
	} while (next != std::wstring::npos);
	return result;
}

inline bool starts_with(const std::wstring& wstr, const std::wstring& wsub)
{
	if (wstr.size() < wsub.size())
		return false;
	else
		return std::equal(wsub.begin(), wsub.end(), wstr.begin());
}

inline void to_lower(std::wstring& wstr)
{
	// Deliberately no std::setlocale(LC_ALL, "") here: this header is also
	// compiled into WeaselTSF, an in-process COM server, and switching the
	// locale would mutate the *host* process globally (and is not thread-safe
	// under MSVC). The strings folded here are pure ASCII (file paths, exe
	// names), which std::towlower maps identically in the default "C" locale.
	std::transform(wstr.begin(), wstr.end(), wstr.begin(), std::towlower);
}
