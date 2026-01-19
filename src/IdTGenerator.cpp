/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#define _USE_32BIT_TIME_T
#include <time.h>
#include <sys/timeb.h>
#include "IdTGenerator.h"

using namespace COP;

IdTValueGenerator::IdTValueGenerator(void)
{
	counter_.fetch_and_store(1);
}

IdTValueGenerator::~IdTValueGenerator(void)
{
}

IdT IdTValueGenerator::getId(){
	__time32_t ltime;
	_time32( &ltime );
	return IdT(counter_.fetch_and_increment(), static_cast<u32>(ltime));
}
