/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include <iostream>
#include <vector>
#include <tbb/tick_count.h>

#include "FileStorage.h"

using namespace std;
using namespace COP;
using namespace COP::Store;
using namespace aux;
using namespace tbb;
using namespace test;

namespace{
	class TestFileStorageObserver: public FileStorageObserver{
	public:
		TestFileStorageObserver(){
			reset();
		}
		virtual void startLoad()
		{
			finished_ = false;
		}
		virtual void onRecordLoaded(const IdT& /*id*/, u32 /*version*/, const char *ptr, size_t s)
		{
			assert(nullptr != ptr);
			assert(0 < s);
			records_.push_back(string(ptr, s));
		}
		virtual void finishLoad()
		{
			finished_ = true;
		}
		void reset(){
			finished_ = false;
		}
	public:
		bool finished_;
		std::vector<string> records_;
	};

	char VERSION_RECORD[] = "<Sxxxx::Version 0.0.0.1E>";
	const int VERSION_SIZE = 25;
	const int VERSION_BUFFER_SIZE = 64;

	void prepareTestFile(const std::string &fileName)
	{
		memcpy(&(VERSION_RECORD[2]), &VERSION_SIZE, sizeof(VERSION_SIZE));

		FILE *f = fopen(fileName.c_str(), "w+");
		fwrite(VERSION_RECORD, 1, VERSION_SIZE, f);

		char buf[256];
		{
			int s = 0;
			memcpy(buf, "<S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::valid vesion 0000E>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		fclose(f);
	}
	void prepareBrokenTestFile(const std::string &fileName)
	{
		memcpy(&(VERSION_RECORD[2]), &VERSION_SIZE, sizeof(VERSION_SIZE));

		FILE *f = fopen(fileName.c_str(), "w+");
		fwrite(VERSION_RECORD, 1, VERSION_SIZE, f);

		char buf[256];
		{
			int s = 0;
			memcpy(buf, "<S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::valid vesion 0000E>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		{// invalid beginning of record
			int s = 0;
			memcpy(buf, ".S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::invalid record 01E>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		{// invalid beginning of record
			int s = 0;
			memcpy(buf, "<......Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::invalid record 02E>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		{// invalid record size
			int s = 0;
			memcpy(buf, "<S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::invalid record 03E>", 21); s += 40;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		{// invalid end of record
			int s = 0;
			memcpy(buf, "<S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::invalid record 01.>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}
		{// Huge invalid record
			char hugeBuf[3*64*1024];
			memset(hugeBuf, '.', 3*64*1024);
			fwrite(hugeBuf, 1, 3*63*1024 + 113, f);
		}
		{
			int s = 0;
			memcpy(buf, "<S.....Y.", 10);s += 9;
			IdT id(1, 2);
			memcpy(&buf[s], &id, sizeof(id));s += sizeof(id);
			buf[s] = '.'; s++;
			int ver = 0;
			memcpy(&buf[s], &ver, sizeof(ver));s += sizeof(ver);
			memcpy(&buf[s], "::last record 00000E>", 21); s += 21;
			memcpy(&buf[2], &s, 4);
			fwrite(buf, 1, s, f);
		}

		fclose(f);
	}

}

bool testFileStorage()
{
	FileStorage::init();
	{
		FileStorage tst;
	}

	{
		system("del testStorage.dat");
		TestFileStorageObserver obs;
		check(!obs.finished_);
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
	}
	{
		prepareTestFile("testStorage.dat");
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(1 == obs.records_.size());
		check(string("valid vesion 0000") == obs.records_.at(0));
	}
	{
		system("del testBrokenStorage.dat");
		prepareBrokenTestFile("testBrokenStorage.dat");
		TestFileStorageObserver obs;
		FileStorage tst("testBrokenStorage.dat", &obs);
		check(obs.finished_);
		check(2 == obs.records_.size());
		check(string("valid vesion 0000") == obs.records_.at(0));
		check(string("last record 00000") == obs.records_.at(1));
	}

	{/// test save records
		system("del testStorage.dat");
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(0 == obs.records_.size());
		tst.save(IdT(1, 1), "aaaa", 4);
		try{
			tst.save(IdT(1, 1), "dubRec", 6);
			check(false);
		}catch(const std::exception &){}
		IdT id = tst.save("bbbb", 4);
		try{
			tst.save(id, "dubRec2", 7);
			check(false);
		}catch(const std::exception &){}
	}
	{/// test erase records
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(2 == obs.records_.size());
		check(string("aaaa") == obs.records_.at(0));
		check(string("bbbb") == obs.records_.at(1));

		tst.erase(IdT(1, 1), 1);
		tst.erase(IdT(1, 1), 0);
		tst.erase(IdT(1, 1));
	}

	{/// test erase records
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(1 == obs.records_.size());
		check(string("bbbb") == obs.records_.at(0));

	}
	{/// test update records
		system("del testStorage.dat");
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(0 == obs.records_.size());
		tst.save(IdT(1, 1), "aaaa", 4);
		u32 r = tst.update(IdT(1, 1), "aaaaa", 5);
		check(1 == r);
	}
	{/// test update records
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(2 == obs.records_.size());
		check(string("aaaa") == obs.records_.at(0));
		check(string("aaaaa") == obs.records_.at(1));
	}
	{/// test replace records
		system("del testStorage.dat");
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(0 == obs.records_.size());
		try{
			tst.replace(IdT(2, 1), 1, "aaaaa", 5);
			check(false);
		}catch(...){}
		tst.save(IdT(1, 1), "aaaa", 4);
		try{
			tst.replace(IdT(1, 1), 1, "aaaaa", 5);
			check(false);
		}catch(...){}
		u32 r = tst.replace(IdT(1, 1), 0, "aaaaa", 5);
		check(1 == r);
	}
	{/// test replace records
		TestFileStorageObserver obs;
		FileStorage tst("testStorage.dat", &obs);
		check(obs.finished_);
		check(1 == obs.records_.size());
		check(string("aaaaa") == obs.records_.at(0));
	}

	return true;
}

