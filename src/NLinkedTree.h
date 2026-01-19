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
#include <set>
#include <map>
#include <list>

#include "AllocateCache.h"
#include "TypesDef.h"
#include "TransactionDef.h"

namespace COP{ namespace ACID{
	class Transaction;
}}

namespace aux{ 

	typedef COP::ACID::TransactionId K;
	typedef COP::ACID::ObjectInTransaction O;
	typedef COP::ACID::ObjectsInTransaction DependObjs;
	typedef COP::ACID::Transaction *V;

	typedef std::set<O> OSetT;
	typedef std::set<K> KSetT;

	struct NTreeNode;

	struct SortedByKey: public std::binary_function <NTreeNode *, NTreeNode *, bool>{
		bool operator()(const NTreeNode * lft, const NTreeNode * rgt) const;
	};

	typedef std::set<NTreeNode *, SortedByKey> TreeNodesT;
	typedef std::list<NTreeNode *> TreeNodesListT;

	struct NTreeRootNode{
		TreeNodesListT children_;

		NTreeRootNode(){}
	};

	struct NTreeNode{
		TreeNodesT parents_;
		TreeNodesT children_;

		K key_;
		V value_;

		NTreeNode();
		NTreeNode(const K &key, const V &value);
	};

	inline bool SortedByKey::operator()(const NTreeNode * lft, const NTreeNode * rgt) const{
		return lft->key_ < rgt->key_;
	}

	struct KParam{
		DependObjs dependsOn_;

		NTreeNode *node_;

	};
	typedef std::map<K, KParam *> KParamsT;

	struct OParam{
		KSetT usedIn_;
	};
	typedef std::map<O, OParam *> OParamsT;
	

	class NLinkTree{
	public:
		NLinkTree();
		~NLinkTree();

		bool add(const K &key, const V &value, const DependObjs &depend, int *readyToExecuteAdded);
		bool remove(const K &key, int *readyToExecuteAdded);
		bool getParents(const K &key, KSetT *parents)const;
		bool getChildren(const K &key, KSetT *children)const;

		bool next(const K &after, K *key, V *value);
		bool next(K *key, V *value);
		bool current(K *key, V *value)const;
		bool isCurrentValid()const;

		void clear();

		void dumpTree();
	private:
		NTreeRootNode root_;
		///contains object and set of the objects that it depends on
		KParamsT keys_;
		///contains list of objects that depends on key object
		OParamsT depends_;
		AllocateCache<NTreeNode> treeNodeAllocator_;
		AllocateCache<KParam> keyParamAllocator_;
		AllocateCache<OParam> objParamAllocator_;
		NTreeNode *current_;

	protected:
		NLinkTree(const NLinkTree &);
		const NLinkTree &operator=(const NLinkTree &);
	};

}