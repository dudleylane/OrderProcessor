/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "WideDataStorage.h"

namespace COP{

template<typename T>
class WideDataLazyRef
{
public:
	WideDataLazyRef(const SourceIdT &id, bool load = false): id_(id), loaded_(load){
		if(loaded_)
			get();
	}
	~WideDataLazyRef(void){}

	const T &get()const{
		if(!loaded_){
			Store::WideDataStorage::instance()->get(id_, &val_);
			loaded_ = true;
		}
		return val_;
	}

	void load(){
		if(!loaded_){
			Store::WideDataStorage::instance()->get(id_, &val_);
			loaded_ = true;
		}
	}

	const SourceIdT &getId()const{
		return id_;
	};
private:
	mutable T val_;
	SourceIdT id_;
	mutable bool loaded_;
};

}