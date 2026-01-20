/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once
#include <stdexcept>

#include <memory>
#include <deque>
#include <atomic>

#include "Logger.h"
#include "ExchUtils.h"
#include "TypesDef.h"

namespace aux{ 

	template<typename T>
	class InterLockCache{
	public:
		static InterLockCache<T> *instance_;

		static void create(const std::string &name, size_t cacheSize = 100)
		{
			assert(nullptr == instance_);
			if(nullptr != instance_)
				throw std::runtime_error(std::string("Cache '") + name + "' initialised twice!");
			instance_ = new InterLockCache<T>(name, cacheSize);
		}
		static void destroy(){
			delete instance_;
			instance_ = nullptr;
		}
		static InterLockCache<T> *instance(){
			assert(nullptr != instance_);
			if(nullptr == instance_)
				throw std::runtime_error("InterLockCache was not initialised!");
			return instance_;
		}

		explicit InterLockCache(const std::string &name, size_t cacheSize = 100): name_(name){
			cache_ = new Node(new T);
			Node *prev = cache_;
			for(size_t i = 0; i < cacheSize; ++i){
				std::unique_ptr<T> val(new T);
				std::unique_ptr<Node> n(new Node(val.get()));
				prev->next_ = n.get();
				prev = n.release();
				val.release();
			}
			/// add nullptr node and create circle
			std::unique_ptr<Node> n(new Node());
			n->next_ = cache_;
			prev->next_ = n.get();

			nextNull_ = n.release();
			nextFree_ = cache_;
			cacheMiss_ = 0;
		}
		~InterLockCache(){
			Node *next = cache_;
			std::deque<Node *> nodes;
			do{
				std::unique_ptr<T> v(next->val_);
				nodes.push_back(next);
				next = next->next_;
			}while(cache_ != next->next_);
			for(size_t i = 0; i < nodes.size(); ++i){
				delete nodes.at(i);
			}
			if(aux::ExchLogger::instance()->isDebugOn()){
				char buf[64];
				aux::toStr(buf, cacheMiss_);
				aux::ExchLogger::instance()->debug(std::string("InterLockCache '") + name_ + "' misses:" + buf);
			}
		}


		void clear();

		T *popFront(){
			do{
				Node *cur = nextFree_.load(std::memory_order_acquire);
				Node *null = nextNull_.load(std::memory_order_acquire);
				if (cur != null){
					T *v = cur->val_.exchange(nullptr, std::memory_order_acq_rel);
					Node *expected = cur;
					nextFree_.compare_exchange_strong(expected, cur->next_, std::memory_order_release, std::memory_order_relaxed);
					if(nullptr != v)
						return v;
				}else{
					cacheMiss_.fetch_add(1, std::memory_order_relaxed);
					return new T;
				}
			}while(true);
		}

		void pushBack(T *val){
			Node *cur = nextNull_.load(std::memory_order_acquire);
			Node *free = nextFree_.load(std::memory_order_acquire);
			while(cur->next_ != free){
				T *expected = nullptr;
				// Try to store val in current null node
				if(cur->val_.compare_exchange_strong(expected, val, std::memory_order_release, std::memory_order_relaxed)){
					// Successfully stored, now advance nextNull_
					nextNull_.compare_exchange_strong(cur, cur->next_, std::memory_order_release, std::memory_order_relaxed);
					return;
				}
				// Failed to store, reload and retry
				cur = nextNull_.load(std::memory_order_acquire);
				free = nextFree_.load(std::memory_order_acquire);
			}
			// Cache is full, delete the value
			delete val;
		}

	private:
		struct Node{
			Node(): next_(nullptr){val_.store(nullptr);}
			Node(T *val): next_(nullptr){val_.store(val);}
			Node(Node *next): next_(next){val_.store(nullptr);}
			Node(Node *next, T *val): next_(next){val_.store(val);}

			std::atomic<T *> val_;
			Node *next_;
		};

		std::string name_;
		Node *cache_;
		char align1_[256];
		std::atomic<Node *> nextFree_;
		char align2_[256];
		std::atomic<Node *> nextNull_;
		char align3_[256];
		std::atomic<int> cacheMiss_;
		
	protected:
		InterLockCache(const InterLockCache &);
		const InterLockCache &operator=(const InterLockCache &);
	};

	template<typename T>
	InterLockCache<T> *InterLockCache<T>::instance_ = nullptr;

	template<typename V>
	class CacheAutoPtr{
	public:
		CacheAutoPtr(): val_(nullptr){}
		CacheAutoPtr(V *val, InterLockCache<V> *alloc): val_(val), alloc_(alloc){}
		explicit CacheAutoPtr(InterLockCache<V> *alloc): val_(nullptr), alloc_(alloc){
			assert(nullptr != alloc_);
			val_ = alloc_->popFront();
		}

		CacheAutoPtr(CacheAutoPtr<V>& val): val_(val.release()), alloc_(val.alloc_){}

		CacheAutoPtr<V>& operator=(CacheAutoPtr<V>& val){
			reset(val.release());
			alloc_ = val.alloc_;
			return (*this);
		}

		~CacheAutoPtr(){
			if(nullptr != val_){
				assert(nullptr != alloc_);
				alloc_->pushBack(val_);
			}
		}

		V& operator*() const{
			assert(nullptr != val_);
			return *val_;
		}

		V *operator->() const{		
			return (&**this);
		}

		V *get() const{ return val_;}

		V *release(){
			V *t = val_;
			val_ = nullptr;
			alloc_ = nullptr;
			return t;
		}

		void reset(){
			if(nullptr != val_){
				assert(nullptr != alloc_);
				alloc_->pushBack(val_);
			}
			release();
		}

		void reset(V *val, InterLockCache<V> *alloc){
			if(nullptr != val_){
				assert(nullptr != alloc_);
				alloc_->pushBack(val_);
			}
			val_ = val;
			alloc_ = alloc;
		}

	private:
		V *val_;
		InterLockCache<V> *alloc_;
	};

}