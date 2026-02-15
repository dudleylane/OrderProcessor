/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <mutex>
#include "TypesDef.h"
#include "WideDataStorage.h"

namespace COP{

template<typename T>
class WideDataLazyRef
{
public:
	WideDataLazyRef(const SourceIdT &id, bool load = false): id_(id), loaded_(false){
		if(load)
			this->load();
	}

	WideDataLazyRef(const WideDataLazyRef &other)
		: val_(other.val_), id_(other.id_), loaded_(other.loaded_.load(std::memory_order_acquire)){}

	WideDataLazyRef &operator=(const WideDataLazyRef &other){
		if(this != &other){
			val_ = other.val_;
			id_ = other.id_;
			loaded_.store(other.loaded_.load(std::memory_order_acquire), std::memory_order_release);
		}
		return *this;
	}

	~WideDataLazyRef(void){}

	const T &get()const{
		if(!loaded_.load(std::memory_order_acquire)){
			std::lock_guard<std::mutex> guard(mtx_);
			if(!loaded_.load(std::memory_order_relaxed)){
				Store::WideDataStorage::instance()->get(id_, &val_);
				loaded_.store(true, std::memory_order_release);
			}
		}
		return val_;
	}

	void load(){
		if(!loaded_.load(std::memory_order_acquire)){
			std::lock_guard<std::mutex> guard(mtx_);
			if(!loaded_.load(std::memory_order_relaxed)){
				Store::WideDataStorage::instance()->get(id_, &val_);
				loaded_.store(true, std::memory_order_release);
			}
		}
	}

	const SourceIdT &getId()const{
		return id_;
	};
private:
	mutable T val_;
	SourceIdT id_;
	mutable std::atomic<bool> loaded_;
	mutable std::mutex mtx_;
};

}