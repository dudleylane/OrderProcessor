/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <string>
#include "TypesDef.h"
#include "FileStorage.h"

using namespace std;
using namespace tbb;
using namespace COP;
using namespace COP::Store;

namespace{
	char VERSION_RECORD[] = "<Sxxxx::Version 0.0.0.1E>";
	const int VERSION_SIZE = 25;
	const int VERSION_BUFFER_SIZE = 64;

	const int HEADER_SIZE = 32;
	const int BODY_PREFIX_SIZE = 16 + sizeof(IdT);
	const char HEADER_TEMPLATE[] = "<Sxxxx.Y.xxxxxxxxxxxxxxxx.xxxx::";
	const int HEADER_SIZE_POS = 2;
	const int HEADER_ACTIVE_POS = 7;
	const int HEADER_ID_POS = 9;
	const int HEADER_VERSION_POS = 26;
	const char TAIL_BODY[] = "E>";
	const int TAIL_SIZE = 2;

	typedef std::vector<char> RecordBody;

	void addVersionRecord(FILE *f){
		assert(nullptr != f);

		/// save record
		fseek(f, 0, SEEK_SET);
		fwrite(VERSION_RECORD, 1, VERSION_SIZE, f);
		fflush(f);
	}

	void checkVersionRecord(FILE *f){
		assert(nullptr != f);

		fseek(f, 0, SEEK_SET);
		
		char buf[VERSION_SIZE + 1];
		size_t readBytes = fread(buf, 1, VERSION_SIZE, f);
		if(VERSION_SIZE != readBytes)
			throw std::runtime_error("Unable to read version record from file!");
		if(0 != memcmp(buf, VERSION_RECORD, VERSION_SIZE))
			throw std::runtime_error("Content of the version record incorrect!");
	}

	void readRecord(FILE *f, FileRecord &record, RecordBody *body){
		assert(nullptr != f);
		assert(nullptr != body);

		record.offset_ = static_cast<size_t>(ftell(f));

		char buf[BODY_PREFIX_SIZE];
		size_t readBytes = fread(buf, 1, BODY_PREFIX_SIZE, f);
		if(0 == readBytes){
			// if nothing could be read - just exit
			record.enabled_ = false;
			return;
		}
		try{
			if(BODY_PREFIX_SIZE != readBytes)
				throw std::runtime_error("Invalid record, unable to read record header!");
			size_t pos = 0;
			if(('<' != buf[pos++])||('S' != buf[pos++]))
				throw std::runtime_error("Invalid record, record header does not begins from '<S'!");

			memcpy(&(record.size_), &(buf[pos]), sizeof(record.size_));
			pos += sizeof(record.size_);
			if(BODY_PREFIX_SIZE > record.size_)
				throw std::runtime_error("Invalid record, record size is less than header length!");
			if('.' != buf[pos++])
				throw std::runtime_error("Invalid record, delimiter '.' missed after record size in record header!");

			if(('Y' != buf[pos])&&('N' != buf[pos]))
				throw std::runtime_error("Invalid record, valid flag should be 'Y'/'N' in header!");
			record.enabled_ = 'Y' == buf[pos++];
			if('.' != buf[pos++])
				throw std::runtime_error("Invalid record, delimiter '.' missed after valid flag in record header!");

			memcpy(&(record.id_), &(buf[pos]), sizeof(record.id_));
			pos += sizeof(record.id_);
			if('.' != buf[pos++])
				throw std::runtime_error("Invalid record, delimiter '.' missed after record id in record header!");

			memcpy(&(record.version_), &(buf[pos]), sizeof(record.version_));
			pos += sizeof(record.version_);
			if((':' != buf[pos++])||(':' != buf[pos++]))
				throw std::runtime_error("Invalid record, '::' is missed after header!");

			body->resize(record.size_ - BODY_PREFIX_SIZE);
			readBytes = fread(&(body->at(0)), 1, record.size_ - BODY_PREFIX_SIZE, f);
			if(record.size_ - BODY_PREFIX_SIZE != readBytes)
				throw std::runtime_error("Unable to read body of the record!");
			if(('E' != body->at(readBytes - 2))||('>' != body->at(readBytes - 1)))
				throw std::runtime_error("Invalid record's tail, 'E' or '>' is missed!");
			body->resize(readBytes - 2);
		}catch(...){
			// returns to the previous position to locate new record (skip one symbol to 
			//    avoid read this record again)
			fseek(f, static_cast<long>(record.offset_) + 1, SEEK_SET);
			throw;
		}
	}

	void saveRecord(FILE *f, const char *recBody, size_t size, FileRecord &record){
		assert(nullptr != f);
		assert(nullptr != recBody);

		fseek(f, 0, SEEK_END);
		record.offset_ = static_cast<size_t>(ftell(f));

		u32 recSize = static_cast<u32>(size) + TAIL_SIZE + HEADER_SIZE;
		auto_ptr<char> recBuffer(new char[recSize]);

		// prepare template
		memcpy(recBuffer.get(), HEADER_TEMPLATE, HEADER_SIZE);

		//fill record size (from '<S' until 'E>')
		memcpy(recBuffer.get() + HEADER_SIZE_POS, &recSize, sizeof(recSize));

		// fill record id
		memcpy(recBuffer.get() + HEADER_ID_POS, &(record.id_), sizeof(record.id_));

		// fill record version
		memcpy(recBuffer.get() + HEADER_VERSION_POS, &(record.version_), sizeof(record.version_));

		// copy record body
		memcpy(recBuffer.get() + HEADER_SIZE, recBody, size);

		// copy record tail
		memcpy(recBuffer.get() + recSize - TAIL_SIZE, TAIL_BODY, TAIL_SIZE);

		// save record into file
		fwrite(recBuffer.get(), 1, recSize, f);
		fflush(f);
	}

	// locates next record in file and skip fault block
	void findNextRecord(FILE *f){
		assert(nullptr != f);

		const int bufSize = 64*1024;
		char buf[bufSize + 1];
		long surrPos = ftell(f);
		while(!feof(f)){
			size_t readBytes = fread(buf, 1, bufSize, f);
			if(0 == readBytes){
				return;
			}
			char *ptr = static_cast<char *>(memchr(buf, '<', bufSize));
			if(nullptr != ptr){
				fseek(f, surrPos + (ptr - buf), SEEK_SET);
				return;
			}
			surrPos += static_cast<long>(readBytes);
		}
	}

	void disableRecord(FILE *f, FileRecord &record){
		assert(nullptr != f);
		
		int res = fseek(f, static_cast<long>(record.offset_) + HEADER_ACTIVE_POS, SEEK_SET);
		if(0 != res)
			throw std::runtime_error("Unable to locate record in file to disable it!");

		// update record in file
		fwrite("N", 1, 1, f);
		fflush(f);
	}

}

void FileStorage::init()
{
	memcpy(&(VERSION_RECORD[2]), &VERSION_SIZE, sizeof(VERSION_SIZE));
}

FileStorage::FileStorage(): 
	file_(nullptr), generator_(), records_()
{
}

FileStorage::FileStorage(const std::string &fileName, FileStorageObserver *observer): 
	file_(nullptr), generator_(), records_()
{
	load(fileName, observer);
}

FileStorage::~FileStorage(void)
{
	clear();
}

void FileStorage::load(const std::string &fileName, FileStorageObserver *observer)
{
	assert(nullptr == file_);
	assert(nullptr != observer);

	file_ = fopen(fileName.c_str(), "r+");
	bool newFile = nullptr == file_;
	if(newFile)
		file_ = fopen(fileName.c_str(), "w+");
	if(nullptr == file_){
		string s = string("FileStorage: Unable to open file '") + fileName + "'.";
		throw std::runtime_error(s.c_str());
	}

	observer->startLoad();
	if(!newFile){
		checkVersionRecord(file_);

		while(!feof(file_)){
			pocessRecord(file_, observer);
		}
	}else{
		addVersionRecord(file_);
	}
	observer->finishLoad();
}

void FileStorage::pocessRecord(FILE *file, FileStorageObserver *observer)
{
	assert(nullptr != observer);
	try{
		FileRecord rec;
		RecordBody body;
		readRecord(file, rec, &body);
		if(!rec.enabled_)
			return;
		RecordsT::iterator it = records_.find(rec.id_);
		if(records_.end() == it){
			auto_ptr<RecordChainT> cont(new RecordChainT());
			cont->push_back(rec);
			records_.insert(RecordsT::value_type(rec.id_, cont.get()));
			cont.release();
		}else{
			assert(nullptr != it->second);
			it->second->push_back(rec);
		}
		observer->onRecordLoaded(rec.id_, rec.version_, &(body[0]), body.size());
	}catch(...){
		findNextRecord(file);
	}
}

void FileStorage::clear()
{
	RecordsT tmp;
	{
		mutex::scoped_lock lock(lock_);
		std::swap(records_, tmp);
		if(nullptr != file_){
			fflush(file_);
			fclose(file_);
			file_ = nullptr;		}
	}
	for(RecordsT::iterator it = tmp.begin(); it != tmp.end(); ++it){
		auto_ptr<RecordChainT> chain(it->second);
	}
}

IdT FileStorage::save(const char *buf, size_t size)
{
	IdT id = generator_.getId();
	save(id, buf, size);
	return id;
}

void FileStorage::save(const IdT &id, const char *buf, size_t size)
{
	assert(nullptr != file_);
	assert(nullptr != buf);
	assert(0 < size);

	FileRecord rec(id, static_cast<u32>(size));
	{
		mutex::scoped_lock lock(lock_);
		RecordsT::const_iterator it = records_.find(id);
		if(records_.end() != it)
			throw std::runtime_error("FileStorage::save: Unable to save record, record with this Id already exists!");
		saveRecord(file_, buf, size, rec);
		auto_ptr<RecordChainT> chain(new RecordChainT);
		chain->push_back(rec);
		records_.insert(RecordsT::value_type(id, chain.get()));
		chain.release();
	}
}

u32 FileStorage::update(const IdT& id, const char *buf, size_t size){
	assert(nullptr != file_);
	assert(nullptr != buf);
	assert(0 < size);

	FileRecord rec(id, static_cast<u32>(size));
	{
		mutex::scoped_lock lock(lock_);
		RecordsT::const_iterator it = records_.find(id);
		if(records_.end() == it){
			auto_ptr<RecordChainT> chain(new RecordChainT);
			records_.insert(RecordsT::value_type(id, chain.get()));
			chain.release();
		}else{
			assert(nullptr != it->second);
			assert(!it->second->empty());
			rec.version_ = it->second->back().version_ + 1;
		}
		saveRecord(file_, buf, size, rec);
		records_[id]->push_back(rec);
	}
	return rec.version_;
}

u32 FileStorage::replace(const IdT& id, u32 version, const char *buf, size_t size){
	assert(nullptr != file_);
	assert(nullptr != buf);
	assert(0 < size);

	FileRecord rec(id, static_cast<u32>(size));
	{
		mutex::scoped_lock lock(lock_);
		RecordsT::const_iterator it = records_.find(id);
		if(records_.end() == it){
			throw std::runtime_error("FileStorage::replace: Unable to replace record, record with this Id not exists!");
		}
		assert(nullptr != it->second);
		assert(!it->second->empty());
		for(RecordChainT::iterator rit = it->second->begin(); rit != it->second->end(); ++rit){
			if(rit->version_ == version){
				rec.version_ = it->second->back().version_ + 1;
				disableRecord(file_, *rit);
				it->second->erase(rit);

				saveRecord(file_, buf, size, rec);
				it->second->push_back(rec);

				return rec.version_;
			}
		}			
	}
	throw std::runtime_error("FileStorage::replace: Unable to replace record, record with this version not exists!");
}

void FileStorage::erase(const IdT& id, u32 version){
	assert(nullptr != file_);
	mutex::scoped_lock lock(lock_);
	RecordsT::iterator it = records_.find(id);
	if(records_.end() == it)
		return;
	assert(nullptr != it->second);
	for(RecordChainT::iterator rit = it->second->begin(); rit != it->second->end(); ++rit){
		if(rit->version_ == version){
			disableRecord(file_, *rit);
			it->second->erase(rit);
			break;
		}
	}
}

void FileStorage::erase(const IdT& id){
	mutex::scoped_lock lock(lock_);
	RecordsT::iterator it = records_.find(id);
	if(records_.end() == it)
		return;
	auto_ptr<RecordChainT> chain(it->second);
	for(RecordChainT::iterator rit = it->second->begin(); rit != it->second->end(); ++rit){
		disableRecord(file_, *rit);
	}
	records_.erase(it);
}
