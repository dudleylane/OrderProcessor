/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "Singleton.h"
#include <tbb/atomic.h>

namespace COP{

class IdTValueGenerator
{
public:
	IdTValueGenerator(void);
	~IdTValueGenerator(void);

	IdT getId();
private:
	tbb::atomic<u64> counter_;
};

typedef aux::Singleton<IdTValueGenerator> IdTGenerator;
}