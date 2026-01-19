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

#include "DataModelDef.h"
#include "NLinkedTree.h"


using namespace std;
using namespace COP;
using namespace COP::ACID;
using namespace aux;
using namespace tbb;
using namespace test;

namespace{
	const size_t MAX_TRANSACTIONS = 1000;//00
	const size_t MAX_ITERATIONS = 100;
	const size_t USED_OBJECTS = 3;
	const size_t OBJECTS_AMOUNT = 50;

	bool testNLinkTreeBenchmark()
	{
		std::vector<DependObjs> deps(MAX_TRANSACTIONS);
		int ready = 0;
		
		for(size_t i = 0; i < MAX_TRANSACTIONS; ++i)
		{
			typedef std::set<IdT> IdsT;
			IdsT ids;
			while(ids.size() < USED_OBJECTS){
				ids.insert(IdT(1 + rand()%OBJECTS_AMOUNT, 1));
			}
			size_t j = 0;
			for(IdsT::const_iterator it = ids.begin(); it != ids.end(); ++it, ++j)
			{
				deps[i].list_[j] = ObjectInTransaction(order_ObjectType, *it);
			}
			deps[i].size_ = USED_OBJECTS;
		}

		NLinkTree tree;
		{
			//Transaction *ptr = nullptr;
			cout<< "\tCreate tree starts..."<< endl;
			tick_count bs = tick_count::now();
			for(size_t i = 0; i < MAX_TRANSACTIONS; ++i)
			{
				tree.add(TransactionId(i, 1), nullptr, deps[i], &ready);
			}
			tick_count es = tick_count::now();
			double diff = (es - bs).seconds();
			cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)MAX_TRANSACTIONS/diff<< " transactions/sec"<< endl;
			cout<< "\t"<< MAX_TRANSACTIONS<< " transactions created."<< endl;
		}
		{
			cout<< "\tRemove tree starts..."<< endl;
			tick_count bs = tick_count::now();
			for(size_t i = 0; i < MAX_TRANSACTIONS; ++i)
			{
				TransactionId k;
				Transaction *v = nullptr;
				check(tree.next(TransactionId(), &k, &v));
				check(tree.remove(k, &ready));
			}
			tick_count es = tick_count::now();
			double diff = (es - bs).seconds();
			cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)MAX_TRANSACTIONS/diff<< " transactions/sec"<< endl;
		}
		{
			TransactionId k;
			Transaction *v = nullptr;
			size_t initialSize = MAX_TRANSACTIONS/20;

			cout<< "\tCreateRemove tree starts..."<< endl;
			tick_count bs = tick_count::now();
			for(size_t j = 0; j < MAX_ITERATIONS; ++j)
			{
				size_t pos = 0;
				for(; pos < initialSize; ++pos)
				{
					tree.add(TransactionId(pos, 1), nullptr, deps[pos], &ready);
				}
				//size_t removed = 0;
				bool hasElements = true;
				do{
					int p = rand();
					bool addNew = (0 == (p&1))&&(pos < MAX_TRANSACTIONS);
					if(addNew){
						tree.add(TransactionId(pos, 1), nullptr, deps[pos], &ready);
						++pos;
					}else{
						hasElements	= tree.next(TransactionId(), &k, &v);
						if(hasElements)
							tree.remove(k, &ready);
						else if(pos < MAX_TRANSACTIONS){
							tree.add(TransactionId(pos, 1), nullptr, deps[pos], &ready);
							++pos;
							hasElements = true;
						}
					}
				}while(hasElements);
			}
			tick_count es = tick_count::now();
			double diff = (es - bs).seconds();
			cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)MAX_TRANSACTIONS*MAX_ITERATIONS/diff<< " transactions/sec"<< endl;
			cout<< "\t"<< MAX_ITERATIONS<< " iterations executed."<< endl;
		}

		return true;
	}
}

bool testNLinkTree()
{
	int ready = 0;
	{
		NLinkTree tree;

		DependObjs dep;
		check(tree.add(TransactionId(1, 1), nullptr, dep, &ready));
		//check(!tree.add(TransactionId(1, 1), nullptr, dep, &ready));
		check(tree.remove(TransactionId(1, 1), &ready));
		check(!tree.remove(TransactionId(1, 1), &ready));
	}
	cout<< "\t1..."<< endl;
	{
		NLinkTree tree;

		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(1, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(2, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(3, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(4, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.list_[2] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.list_[3] = ObjectInTransaction(order_ObjectType, TransactionId(5, 1));
			dep.size_ = 4;
			check(tree.add(TransactionId(5, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.size_ = 2;
			check(tree.add(TransactionId(6, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(7, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(5, 1));
			dep.size_ = 2;
			check(tree.add(TransactionId(8, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(9, 1), nullptr, dep, &ready));
		}
		TransactionId k;
		Transaction *v = nullptr;
		check(tree.next(TransactionId(), &k, &v));
		check(TransactionId(1, 1) == k);check(nullptr == v);
		check(tree.next(k, &k, &v));
		check(TransactionId(2, 1) == k);check(nullptr == v);
		check(tree.next(k, &k, &v));
		check(TransactionId(3, 1) == k);check(nullptr == v);
		check(tree.next(k, &k, &v));
		check(TransactionId(4, 1) == k);check(nullptr == v);
		check(!tree.next(k, &k, &v));

		check(tree.remove(TransactionId(1, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(2, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(3, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(2, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(3, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(3, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(4, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(5, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(5, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(6, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(6, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(7, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(8, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(7, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(9, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(7, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		check(tree.remove(TransactionId(7, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(!tree.next(TransactionId(), &k, &v));
		}


	}
	cout<< "\t2..."<< endl;
	{
		NLinkTree tree;

		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(1, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(2, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(3, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(4, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.list_[2] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.list_[3] = ObjectInTransaction(order_ObjectType, TransactionId(5, 1));
			dep.size_ = 4;
			check(tree.add(TransactionId(5, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(4, 1));
			dep.size_ = 2;
			check(tree.add(TransactionId(6, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(2, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(7, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(1, 1));
			dep.list_[1] = ObjectInTransaction(order_ObjectType, TransactionId(5, 1));
			dep.size_ = 2;
			check(tree.add(TransactionId(8, 1), nullptr, dep, &ready));
		}
		{
			DependObjs dep;
			dep.list_[0] = ObjectInTransaction(order_ObjectType, TransactionId(3, 1));
			dep.size_ = 1;
			check(tree.add(TransactionId(9, 1), nullptr, dep, &ready));
		}

		cout<< "\t2create_finish..."<< endl;
		check(tree.remove(TransactionId(5, 1), &ready));
		cout<< "\t2 5 checking..."<< endl;
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(1, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(2, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(3, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 5 removed..."<< endl;
		check(tree.remove(TransactionId(1, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(2, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(3, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 1 removed..."<< endl;
		check(tree.remove(TransactionId(2, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(3, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 2 removed..."<< endl;
		check(tree.remove(TransactionId(3, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(4, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 3 removed..."<< endl;
		check(tree.remove(TransactionId(4, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(k, &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(6, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 4 removed..."<< endl;
		check(tree.remove(TransactionId(6, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(7, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 6 removed..."<< endl;
		check(tree.remove(TransactionId(7, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(8, 1) == k);check(nullptr == v);
			check(tree.next(k, &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 7 removed..."<< endl;
		check(tree.remove(TransactionId(8, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(tree.next(TransactionId(), &k, &v));
			check(TransactionId(9, 1) == k);check(nullptr == v);
			check(!tree.next(k, &k, &v));
		}
		cout<< "\t2 8 removed..."<< endl;
		check(tree.remove(TransactionId(9, 1), &ready));
		{
			TransactionId k;
			Transaction *v = nullptr;
			check(!tree.next(TransactionId(), &k, &v));
		}
	}
	cout<< "\tbench..."<< endl;

	testNLinkTreeBenchmark();

	return true;
}

