/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <climits>
#include "TypesDef.h"

namespace aux{
	char *toStr(char *buf, size_t val);
	char *toStr(char *buf, size_t val, size_t pos);
	char *toStr(char *buf, int val);
	char *toStr(char *buf, int val, size_t pos);

	char *toStr(char *buf, COP::u32 val);
	char *toStr(char *buf, COP::u32 val, size_t pos);
	// u64 and i64 overloads only if different from size_t (Windows)
#if defined(_WIN32) || ULONG_MAX == 0xFFFFFFFF
	char *toStr(char *buf, COP::u64 val);
	char *toStr(char *buf, COP::u64 val, size_t pos);
	char *toStr(char *buf, COP::i64 val);
	char *toStr(char *buf, COP::i64 val, size_t pos);
#endif

	COP::DateTimeT currentDateTime();
	void WaitInterval(int mseconds);
}