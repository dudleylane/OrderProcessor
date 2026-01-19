/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once
#include <stdexcept>

#include <cassert>
#include <exception>

namespace aux{ 

template<typename T>
struct Singleton
{
    static T *instance_;

	static void create()
	{
		assert(NULL == instance_);
		if(NULL != instance_)
			throw std::runtime_error("Singleton initialised twice!");
		instance_ = new T;
	}
	static void destroy(){
		delete instance_;
		instance_ = NULL;
	}
	static T *instance(){
		assert(NULL != instance_);
		if(NULL == instance_)
			throw std::runtime_error("Singleton was not initialised!");
		return instance_;
	}
};

template<typename T>
T *Singleton<T>::instance_ = NULL;

} 
