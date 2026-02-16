/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once
#include <stdexcept>

#include <cassert>
#include <memory>

namespace aux{

template<typename T>
struct Singleton
{
    static std::unique_ptr<T> instance_;

	static void create()
	{
		assert(!instance_);
		if(instance_) [[unlikely]]
			throw std::runtime_error("Singleton initialised twice!");
		instance_ = std::make_unique<T>();
	}
	static void destroy() noexcept {
		instance_.reset();
	}
	static T *instance() noexcept {
		assert(instance_);
		[[assume(instance_ != nullptr)]];
		return instance_.get();
	}
};

template<typename T>
std::unique_ptr<T> Singleton<T>::instance_ = nullptr;

}
