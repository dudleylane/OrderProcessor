/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <memory>
#include <vector>
#include <tbb/scalable_allocator.h> 

namespace aux{ 

	template<typename T>
	class AllocateCache{
	public:
		explicit AllocateCache(const size_t cacheSize = 100): cacheSize_(cacheSize){
			cache_.reserve(cacheSize_);
			for(size_t i = 0; i < cacheSize_; ++i){
				T *ptr = allocator_.allocate(1);
				allocator_.construct(ptr, T());
				cache_.push_back(ptr);
			}
			lastAvailable_ = static_cast<int>(cacheSize_) - 1;
		}
		~AllocateCache(){
			clear();
		}

		T *create(){
			T *ptr = allocate();
			allocator_.construct(ptr, T());
			return ptr;
		}
		T *create(const T &val){
			T *ptr = allocate();
			allocator_.construct(ptr, val);
			return ptr;
		}
		void destroy(T *val){
			allocator_.destroy(val);
			if(lastAvailable_ + 1 < static_cast<int>(cacheSize_)){
				++lastAvailable_;
				cache_[lastAvailable_] = val;
			}else{
				allocator_.deallocate(val, 1);
			}
		}

		void clear(){
			CacheT tmp;
			{
				std::swap(tmp, cache_);
				lastAvailable_ = -1;
			}
			for(size_t i = 0; i < tmp.size(); ++i){
				allocator_.deallocate(tmp[i], 1);
			}
		}
	private:
		T *allocate(){
			T *ptr = NULL;
			if(0 <= lastAvailable_){
				ptr = cache_[lastAvailable_];
				cache_[lastAvailable_] = NULL;
				--lastAvailable_;
			}else{
				ptr = allocator_.allocate(1);
			}
			return ptr;
		}

	private:
		tbb::scalable_allocator<T> allocator_;
		typedef std::vector<T *> CacheT;
		int lastAvailable_;
		CacheT cache_;
		size_t cacheSize_;

	protected:
		AllocateCache(const AllocateCache &);
		const AllocateCache &operator=(const AllocateCache &);
	};

	template<typename V>
	class AllocAutoPtr{
	public:
		AllocAutoPtr(): val_(NULL), alloc_(NULL){}
		AllocAutoPtr(V *val, AllocateCache<V> *alloc): val_(val), alloc_(alloc){}

		AllocAutoPtr(AllocAutoPtr<V>& val): val_(val.release()), alloc_(val.alloc_){}

		AllocAutoPtr<V>& operator=(AllocAutoPtr<V>& val){
			reset(val.release());
			alloc_ = val.alloc_;
			return (*this);
		}

		~AllocAutoPtr(){
			if(NULL != val_){
				assert(NULL != alloc_);
				alloc_->destroy(val_);
			}
		}

		V& operator*() const{
			assert(NULL != val_);
			return *val_;
		}

		V *operator->() const{		
			return (&**this);
		}

		V *get() const{ return val_;}

		V *release(){
			V *t = val_;
			val_ = NULL;
			alloc_ = NULL;
			return t;
		}

		void reset(){
			if(NULL != val_){
				assert(NULL != alloc_);
				alloc_->destroy(val_);
			}
			release();
		}

		void reset(V *val, AllocateCache<V> *alloc){
			if(NULL != val_){
				assert(NULL != alloc_);
				alloc_->destroy(val_);
			}
			val_ = val;
			alloc_ = alloc;
		}

	private:
		V *val_;
		AllocateCache<V> *alloc_;
	};

}