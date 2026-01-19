/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <memory>
#include <deque>
#include <tbb/atomic.h>

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
			assert(NULL == instance_);
			if(NULL != instance_)
				throw std::exception(string("Cache '") + name + "' initialised twice!");
			instance_ = new T(name, cacheSize);
		}
		static void destroy(){
			delete instance_;
			instance_ = NULL;
		}
		static T *instance(){
			assert(NULL != instance_);
			if(NULL == instance_)
				throw std::exception("InterLockCache was not initialised!");
			return instance_;
		}

		explicit InterLockCache(const std::string &name, size_t cacheSize = 100): name_(name){
			cache_ = new Node(new T);
			Node *prev = cache_;
			for(size_t i = 0; i < cacheSize; ++i){
				std::auto_ptr<T> val(new T);
				std::auto_ptr<Node> n(new Node(val.get()));
				prev->next_ = n.get();
				prev = n.release();
				val.release();
			}
			/// add NULL node and create circle
			std::auto_ptr<Node> n(new Node());
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
				std::auto_ptr<T> v(next->val_);
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
				Node *cur = nextFree_;
				if (cur != nextNull_){
					T *v = nextFree_->val_.fetch_and_store(NULL);
					nextFree_.compare_and_swap(cur->next_, cur);
					if(NULL != v)
						return v;
				}else{
					cacheMiss_.fetch_and_increment();
					return new T;
				}
			}while(true);
		}

		void pushBack(T *val){
			Node *cur = nextNull_;
			while(cur->next_ != nextFree_){
				bool v = (NULL != nextNull_->val_.compare_and_swap(val, NULL));
				nextNull_.compare_and_swap(nextNull_->next_, cur);
				if(!v)
					return;
				cur = nextNull_;
			}
			delete val;
		}

	private:
		struct Node{
			Node(): next_(NULL){val_.fetch_and_store(NULL);}
			Node(T *val): next_(NULL){val_.fetch_and_store(val);}
			Node(Node *next): next_(next){val_.fetch_and_store(NULL);}
			Node(Node *next, T *val): next_(next){val_.fetch_and_store(val);}

			tbb::atomic<T *> val_;
			Node *next_;
		};

		std::string name_;
		Node *cache_;
		char align1_[256];
		tbb::atomic<Node *> nextFree_;
		char align2_[256];
		tbb::atomic<Node *> nextNull_;
		char align3_[256];
		tbb::atomic<int> cacheMiss_;
		
	protected:
		InterLockCache(const InterLockCache &);
		const InterLockCache &operator=(const InterLockCache &);
	};

	template<typename T>
	T *InterLockCache<T>::instance_ = NULL;

	template<typename V>
	class CacheAutoPtr{
	public:
		CacheAutoPtr(): val_(NULL){}
		CacheAutoPtr(V *val, InterLockCache<V> *alloc): val_(val), alloc_(alloc){}
		explicit CacheAutoPtr(InterLockCache<V> *alloc): val_(NULL), alloc_(alloc){
			assert(NULL != alloc_);
			val_ = alloc_->popFront();
		}

		CacheAutoPtr(CacheAutoPtr<V>& val): val_(val.release()), alloc_(val.alloc_){}

		CacheAutoPtr<V>& operator=(CacheAutoPtr<V>& val){
			reset(val.release());
			alloc_ = val.alloc_;
			return (*this);
		}

		~CacheAutoPtr(){
			if(NULL != val_){
				assert(NULL != alloc_);
				alloc_->pushBack(val_);
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
				alloc_->pushBack(val_);
			}
			release();
		}

		void reset(V *val, InterLockCache<V> *alloc){
			if(NULL != val_){
				assert(NULL != alloc_);
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