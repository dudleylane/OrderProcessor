/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <iostream>
#include <string>
#include "TaskManager.h"
#include "Logger.h"

using namespace std;
using namespace COP::Tasks;

typedef bool(*testFunc)();

struct TestInfo{
	std::string name_;
	testFunc func_;
};

bool testEventBenchmark();
bool testStateMachine();
bool testStates();
bool testOrderBook();
bool testIncomingQueues();
bool testProcessor();
bool testNLinkTree();
bool testTaskManager();
bool testFileStorage();
bool testStorageRecordDispatcher();
bool testCodecs();
bool testIntegral();
bool testInterlockCache();

TestInfo tests[] = {
	//{"testEventBenchmark", &testEventBenchmark},
	{"testFileStorage", &testFileStorage},
	{"testCodecs", &testCodecs},
	{"testStorageRecordDispatcher", &testStorageRecordDispatcher},
	{"testNLinkTree", &testNLinkTree},
	{"testInterlockCache", &testInterlockCache},
	{"testStateMachine", &testStateMachine},
	{"testOrderBook", &testOrderBook},
	{"testIncomingQueues", &testIncomingQueues},
	{"testStates", &testStates},
	{"testProcessor", &testProcessor},
	{"testTaskManager", &testTaskManager},
	{"testIntegral", &testIntegral},
	{"", nullptr},
};

int main(int, char**)
{
	unsigned int succeed = 0, failed = 0;

	aux::ExchLogger::create();
	aux::ExchLogger::instance()->setDebugOn(false);
	aux::ExchLogger::instance()->setNoteOn(false);

	// Start up scheduler
	TaskManager::init(4);

	int i = 0;
	while(nullptr != tests[i].func_){
		cout<< "Execute test '"<< tests[i].name_<< "':"<< endl;
		bool rez;
		try{ 
			rez = tests[i].func_();
		}catch(const std::exception &ex){
			cout<< "Unhandled exception during test executing:"<< endl<< ex.what()<< endl;
			rez = false;
		}catch(...){
			cout<< "Unexpected unhandled exception during test executing!"<< endl;
			rez = false;
		}
		if(rez){
			cout<< "\tSucceed."<< endl;
			++succeed;
		}else{
			cout<< "\tFailed."<< endl;
			++failed;
		}
		++i;
	}

	cout<< endl<< "Test succeed: "<< succeed<< ", failed: "<< failed<< endl;

	TaskManager::destroy();

	aux::ExchLogger::destroy();
	return 0;
}

