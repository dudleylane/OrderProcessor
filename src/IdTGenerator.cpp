/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <ctime>
#include "IdTGenerator.h"

using namespace COP;

IdTValueGenerator::IdTValueGenerator(void)
{
	counter_.store(1);
}

IdTValueGenerator::~IdTValueGenerator(void)
{
}

IdT IdTValueGenerator::getId(){
	std::time_t ltime = std::time(nullptr);
	return IdT(counter_.fetch_add(1), static_cast<u32>(ltime));
}
