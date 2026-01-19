/****************************************************************************************
*
*                             Exchange v.2
*                   Copyright(c) 2009 Sergey Mikhailik
*
****************************************************************************************/

#include "TestAux.h"
#include <iostream>
#include <vector>
#include <tbb/tick_count.h>
#include <tbb/compat/thread>

#include "DataModelDef.h"
#include "InterLockCache.h"
#include "Logger.h"


using namespace std;
using namespace COP;
using namespace COP::ACID;
using namespace aux;
using namespace tbb;
using namespace test;

namespace {
	const int MAX_ITERATION = 1000000;
	const int MAX_CONCURR_TIMES = 2;
	InterLockCache<int> *gCache = nullptr;

	void cacheBenchmark(){
		assert(nullptr != gCache);
		std::vector<int> step(MAX_ITERATION, 0);
		std::deque<int *> queue;

		for(size_t i = 0; i < MAX_ITERATION; ++i)
			step[i] = rand()%2;

		int steps = 0;
		cout<< "\tStart singleThread benchmark..."<< endl;
		tick_count bs = tick_count::now();
		for(size_t j = 0; j < MAX_CONCURR_TIMES; ++j){
			for(size_t i = 0; i < MAX_ITERATION; ++i)
			{
				if((0 == step[i])||queue.empty()){
					int *val = gCache->popFront();
					assert(nullptr != val);
					queue.push_back(val);			
					++steps;
				}else{
					int *val = queue.front();
					gCache->pushBack(val);
					queue.pop_front();
				}
			}
			while(!queue.empty()){
				int *val = queue.front();
				gCache->pushBack(val);
				queue.pop_front();
			}
			queue.clear();
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)MAX_CONCURR_TIMES*2*steps/diff<< " cacheCalls/sec"<< endl;
		cout<< "\t"<< steps<< " cache calls executed."<< endl;
	}
}

bool testInterlockCache()
{
	aux::ExchLogger::instance()->setDebugOn(true);
	{
		InterLockCache<int> cache("base");
	}

	{
		std::deque<int *> queue;
		InterLockCache<int> cache("base");
		for(size_t i = 0; i < 10; ++i){
			int *val = cache.popFront();
			assert(nullptr != val);
			queue.push_back(val);
		}
		for(size_t i = 0; i < queue.size(); ++i){
			cache.pushBack(queue.at(i));
		}
		queue.clear();
		for(size_t i = 0; i < 10; ++i){
			int *val = cache.popFront();
			assert(nullptr != val);
			queue.push_back(val);
		}
		for(size_t i = 0; i < queue.size(); ++i){
			cache.pushBack(queue.at(i));
		}
	}
	{
		std::vector<int> step(MAX_ITERATION, 0);
		std::deque<int *> queue;
		InterLockCache<int> cache("singleThread benchmark");

		for(size_t i = 0; i < MAX_ITERATION; ++i)
			step[i] = rand()%2;

		int steps = 0;
		cout<< "\tStart singleThread benchmark..."<< endl;
		tick_count bs = tick_count::now();
		for(size_t i = 0; i < MAX_ITERATION; ++i)
		{
			if((0 == step[i])||queue.empty()){
				int *val = cache.popFront();
				assert(nullptr != val);
				queue.push_back(val);			
				++steps;
			}else{
				int *val = queue.front();
				cache.pushBack(val);
				queue.pop_front();
			}
		}
		while(!queue.empty())
		{
			int *val = queue.front();
			cache.pushBack(val);
			queue.pop_front();
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)2*steps/diff<< " cacheCalls/sec"<< endl;
		cout<< "\t"<< steps<< " cache calls executed."<< endl;
	}

	{
		InterLockCache<int> cache("multiThread benchmark");
		gCache = &cache;
		cout<< "\tStart multiThread benchmark..."<< endl;
		std::thread th1(cacheBenchmark);
		std::thread th2(cacheBenchmark);
		std::thread th3(cacheBenchmark);
		std::thread th4(cacheBenchmark);
		std::thread th5(cacheBenchmark);

		th5.join();
		th4.join();
		th3.join();
		th2.join();
		th1.join();
	}

	return true;
}

