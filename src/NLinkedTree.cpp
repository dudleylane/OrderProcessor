/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <cassert>
#include "NLinkedTree.h"
#include "Logger.h"
#include "ExchUtils.h"

#include <iostream>

using namespace std;
using namespace aux;
using namespace COP;
using namespace COP::ACID;

namespace{
	struct ChildElement{
		K childId_;
		K parentId_;

		ChildElement(): childId_(), parentId_(){}
		ChildElement(const K &chld, const K &prnt): childId_(chld), parentId_(prnt){}
	};
	inline bool operator < (const ChildElement &lft, const ChildElement &rght){
		if(lft.childId_ == rght.childId_)
			return lft.parentId_ < rght.parentId_;
		return lft.childId_ < rght.childId_;
	}

	typedef std::set<ChildElement> ChildrenT;
}

namespace {
	enum LogMessages{
		INVALID_MSG = 0,
		ADDNODE_FINAL_MSG,
		REMOVENODE_START_MSG,
		REMOVENODE_FINAL_MSG,
		NEXT_FINAL_MSG
	};

	class NLTLogMessage: public aux::LogMessage{
	public:
		NLTLogMessage(LogMessages type, const IdT &id, size_t rootElems, int newChild): 
			type_(type), id_(id), rootElems_(rootElems), newChild_(newChild)
		{}

		virtual void prepareMessage(){
			switch(type_){
			case REMOVENODE_START_MSG:
				msg.reserve(256);
				msg = "NLinkTree::remove() - starting with node '";
				id_.toString(msg);
				msg += "' to remove, root contains '";
				buf[0] = 0;
				toStr(buf, rootElems_);
				msg += buf;
				msg += "'.";
				break;
			case NEXT_FINAL_MSG:
				msg.reserve(256);
				msg = "NLinkTree::next() - node '";
				id_.toString(msg);
				msg += "'.";
				break;
			case ADDNODE_FINAL_MSG:
				msg.reserve(256);
				msg = "NLinkTree::add() - node '";
				id_.toString(msg);
				msg += "' added, root contains '";
				buf[0] = 0;
				toStr(buf, rootElems_);
				msg += buf;
				msg += "', were added '";
				buf[0] = 0;
				toStr(buf, newChild_);
				msg += buf;
				msg += "'.";
				break;
			case REMOVENODE_FINAL_MSG:
				msg.reserve(256);
				msg = "NLinkTree::remove() - node '";
				id_.toString(msg);
				msg += "' removed, root contains '";
				buf[0] = 0;
				toStr(buf, rootElems_);
				msg += buf;
				msg += "', were added '";
				buf[0] = 0;
				toStr(buf, newChild_);
				msg += buf;
				msg += "'.";
				break;
			default:
				assert(false);
				throw std::runtime_error("NLTLogMessage used with invalid type!");
			};	
		}
	private:
		NLTLogMessage(const NLTLogMessage &);
		NLTLogMessage& operator=(const NLTLogMessage &);

		char buf[64];
		LogMessages type_;
		const IdT &id_;
		size_t rootElems_;
		int newChild_;
	};

}


NTreeNode::NTreeNode():key_(), value_()
{}

NTreeNode::NTreeNode(const K &key, const V &value):key_(key), value_(value)
{}

NLinkTree::NLinkTree(): root_(), keys_(), depends_(), 
	treeNodeAllocator_(2000), keyParamAllocator_(2000), objParamAllocator_(), current_(nullptr)
{

}

NLinkTree::~NLinkTree()
{
	clear();
}

void NLinkTree::clear()
{
	KParamsT tmpKeys;
	OParamsT tmpDepends;
	{
		NTreeRootNode tmp;		
		std::swap(tmp, root_);
		std::swap(tmpKeys, keys_);
		std::swap(tmpDepends, depends_);
	}
	{
		for(OParamsT::iterator it = tmpDepends.begin(); it != tmpDepends.end(); ++it){
			AllocAutoPtr<OParam> el(it->second, &objParamAllocator_);//auto_ptr<OParam> el(it->second);
		}
	}
	{
		for(KParamsT::iterator it = tmpKeys.begin(); it != tmpKeys.end(); ++it){
			AllocAutoPtr<KParam> el(it->second, &keyParamAllocator_);//auto_ptr<KParam> el(it->second);
			if(nullptr != it->second->node_)
				AllocAutoPtr<NTreeNode> nd(it->second->node_, &treeNodeAllocator_);//auto_ptr<NTreeNode> nd(it->second->node_);
		}
	}
}

bool NLinkTree::add(const K &key, const V &value, const DependObjs &depend, int *readyToExecuteAdded)
{
	assert((keys_.empty())||(keys_.rbegin()->first < key));
	assert(nullptr != readyToExecuteAdded);
	if(aux::ExchLogger::instance()->isDebugOn())
		aux::ExchLogger::instance()->debug("NLinkTree::add() start add");

	if(keys_.end() != keys_.find(key)){
		aux::ExchLogger::instance()->debug("NLinkTree::add() - key already exists");
		return false;
	}

	*readyToExecuteAdded = 0;
	KSetT parents;
	// update list of the based objects and get list of the parent keys
	for(unsigned char o = 0; o < depend.size_; ++o){
		const O &obj = depend.list_[o];
		assert(obj.id_.isValid());
		OParamsT::iterator depIt = depends_.find(obj);
		if(depends_.end() == depIt){
			// if this object not used in transactions now - need to add it
			AllocAutoPtr<OParam> el(objParamAllocator_.create(OParam()), &objParamAllocator_);//auto_ptr<OParam> el(new OParam());
			el->usedIn_.insert(key);
			depends_.insert(OParamsT::value_type(obj, el.get()));
			el.release();
		}else{
			assert(nullptr != depIt->second);
			// if this object used in some transaction - last transaction will be parent of this one
			assert(!depIt->second->usedIn_.empty());
			assert(*(depIt->second->usedIn_.rbegin()) < key);
			parents.insert(*(depIt->second->usedIn_.rbegin()));
			// add this transaction as a last that use this object
			depIt->second->usedIn_.insert(key);
		}
	}

	AllocAutoPtr<NTreeNode> node(treeNodeAllocator_.create(NTreeNode(key, value)), &treeNodeAllocator_);
	// update tree - add new transaction as a child to all parents or root
	if(!parents.empty()){
		for(KSetT::const_iterator pIt = parents.begin(); pIt != parents.end(); ++pIt){
			KParamsT::iterator kIt = keys_.find(*pIt);
			if(keys_.end() == kIt){
				///Todo: error about broken structure!
			}else{
				assert(nullptr != kIt->second);
				assert(nullptr != kIt->second->node_);
				// remember parent
				node->parents_.insert(kIt->second->node_);
				// add this element into the parent's childrens
				kIt->second->node_->children_.insert(node.get());
			}
		}
	}else{
		// if no parents - than root parent 
		root_.children_.push_back(node.get());
		++(*readyToExecuteAdded);
	}

	// add into the list of the keys
	AllocAutoPtr<KParam> keyParams(keyParamAllocator_.create(KParam()), &keyParamAllocator_);//auto_ptr<KParam> keyParams(new KParam());
	keyParams->node_ = node.get();
	keyParams->dependsOn_ = depend;
	keys_.insert(KParamsT::value_type(key, keyParams.get()));
	keyParams.release();

	node.release();

	if(aux::ExchLogger::instance()->isDebugOn()){
		NLTLogMessage l(ADDNODE_FINAL_MSG, key, root_.children_.size(), *readyToExecuteAdded);
		aux::ExchLogger::instance()->debug(l);
	}
	return true;
}

bool NLinkTree::remove(const K &key, int *readyToExecuteAdded)
{
	assert(nullptr != readyToExecuteAdded);
	if(aux::ExchLogger::instance()->isDebugOn()){
		NLTLogMessage l(REMOVENODE_START_MSG, key, root_.children_.size(), 0);
		aux::ExchLogger::instance()->debug(l);
	}

	*readyToExecuteAdded = 0;
	AllocAutoPtr<KParam> keyParams;
	AllocAutoPtr<NTreeNode> nd;
	{/// locate parameters of this transaction and remove it from keys_
		KParamsT::iterator kIt = keys_.find(key);
		if(keys_.end() == kIt){
			aux::ExchLogger::instance()->debug("NLinkTree::remove() - remove failed, unable to locate object");
			return false;
		}
		assert(nullptr != kIt->second);
		assert(nullptr != kIt->second->node_);
		keyParams.reset(kIt->second, &keyParamAllocator_);
		nd.reset(keyParams->node_, &treeNodeAllocator_);
		keys_.erase(kIt);
	}

	{/// search for this transaction in root_ and remove it
		/// this transaction will be in root element, if it does not contains any parents
		if(keyParams->node_->parents_.empty()){
			if(root_.children_.empty()){
				aux::ExchLogger::instance()->debug("NLinkTree::remove() - remove failed, hasn't parents but root is empty.");
				return false;
			}
			if(keyParams->node_ == current_){
				TreeNodesListT::iterator erIt = root_.children_.begin();
				if(*erIt == current_){
					current_ = nullptr;
				}else{
					for(; erIt != root_.children_.end(); ++erIt){
						if(*erIt == current_){
							TreeNodesListT::const_iterator i = erIt;
							--i;
							current_ = *i;
							break;
						}
					}					
				}
				if(root_.children_.end() != erIt)
					root_.children_.erase(erIt);
			}else{
				for(TreeNodesListT::iterator erIt = root_.children_.begin(); erIt != root_.children_.end(); ++erIt){
					if(*erIt == keyParams->node_){
						root_.children_.erase(erIt);
						break;
					}
				}
				//root_.children_.erase(keyParams->node_);
			}
		}
	}

	ChildrenT childn;
	{// for all objects - remove this transaction and bind next element in usedIn_ (that child of this transaction) 
	 // with previous usedIn_ element (that parent of this transaction).
	 // remove this transaction from all objects that used in it
		for(unsigned char o = 0; o < keyParams->dependsOn_.size_; ++o){
			const O&obj = keyParams->dependsOn_.list_[o];
			OParamsT::iterator depIt = depends_.find(obj);
			if(depends_.end() != depIt){
				assert(nullptr != depIt->second);
				if(depIt->second->usedIn_.empty())
					continue;///ToDo: notify about error
				K child = K(), prnt = K();
				if(key == *(depIt->second->usedIn_.begin())){
					/// it this transaction first - than child should become next
					KSetT::const_iterator c = ++(depIt->second->usedIn_.begin());
					if(depIt->second->usedIn_.end() != c)
						child = *c;
					depIt->second->usedIn_.erase(depIt->second->usedIn_.begin());
				}else{
					KSetT::iterator kIt = depIt->second->usedIn_.find(key);
					if(depIt->second->usedIn_.end() != kIt){
						KSetT::const_iterator i = kIt;
						++i;
						if(depIt->second->usedIn_.end() != i){
							child = *i;		
						}
						i = kIt;
						--i;
						prnt = *i;
						depIt->second->usedIn_.erase(kIt);
					}
				}
				if((K() != child)||(K() != prnt))
					childn.insert(ChildElement(child, prnt));
				if(depIt->second->usedIn_.empty()){
					AllocAutoPtr<OParam> el(depIt->second, &objParamAllocator_);//auto_ptr<OParam> el(depIt->second);
					depends_.erase(depIt);
				}
			}else{
				/// todo: notify about error
			}	
		}
	}

	{// for the all children - update parent
		for(ChildrenT::const_iterator cIt = childn.begin(); cIt != childn.end(); ++cIt){
			KParamsT::iterator prntIt = keys_.end();
			if(K() != cIt->parentId_){
				prntIt = keys_.find(cIt->parentId_);
				if(keys_.end() != prntIt){
					assert(nullptr != prntIt->second);
					assert(nullptr != prntIt->second->node_);
					prntIt->second->node_->children_.erase(keyParams->node_);
				}else
					true; /// todo: note about broken structure
			}

			if(K() != cIt->childId_){
				KParamsT::iterator chldIt = keys_.find(cIt->childId_);
				if(keys_.end() == chldIt){
					/// todo: note about broken structure
					continue;
				}
				assert(nullptr != chldIt->second);
				assert(nullptr != chldIt->second->node_);
				chldIt->second->node_->parents_.erase(keyParams->node_);

				if(keys_.end() != prntIt){
					chldIt->second->node_->parents_.insert(prntIt->second->node_);
				}else{
					// check that child has not another parents and add it into the Root
					if(chldIt->second->node_->parents_.empty()){
						ChildrenT::const_iterator nxtIt = cIt;
						++nxtIt;
						if((childn.end() == nxtIt)||(cIt->childId_ != nxtIt->childId_)){
							root_.children_.push_back(chldIt->second->node_);
							++(*readyToExecuteAdded);
						}
					}
				}
			}
		}
	}

	if(aux::ExchLogger::instance()->isDebugOn()){
		NLTLogMessage l(REMOVENODE_FINAL_MSG, key, root_.children_.size(), *readyToExecuteAdded);
		aux::ExchLogger::instance()->debug(l);
	}
	return true;
}

bool NLinkTree::getParents(const K &key, KSetT *parents)const
{
	assert(nullptr != parents);
	{
		KParamsT::const_iterator kIt = keys_.find(key);
		if(keys_.end() == kIt)
			return false;
		assert(nullptr != kIt->second);
		assert(nullptr != kIt->second->node_);
		for(TreeNodesT::const_iterator it = kIt->second->node_->parents_.begin(); 
			it != kIt->second->node_->parents_.begin(); ++it){
			parents->insert((*it)->key_);
		}
	}
	return true;
}

bool NLinkTree::getChildren(const K &key, KSetT *children)const
{
	assert(nullptr != children);
	{
		KParamsT::const_iterator kIt = keys_.find(key);
		if(keys_.end() == kIt)
			return false;
		assert(nullptr != kIt->second);
		assert(nullptr != kIt->second->node_);
		for(TreeNodesT::const_iterator it = kIt->second->node_->children_.begin(); 
			it != kIt->second->node_->children_.begin(); ++it){
			children->insert((*it)->key_);
		}
	}
	return true;
}

bool NLinkTree::next(const K &after, K *key, V *value)
{
	/*for(TreeNodesT::const_iterator it = root_.children_.begin(); 
		it != root_.children_.end(); ++it){
		cout<< (*it)->key_<< ", "<< (*it)->value_<< endl;
	}*/
	{
		NTreeNode *node = nullptr;
		if(K() == after){
			if(root_.children_.empty())
				return false;
			node = *(root_.children_.begin());
		}else{
			bool found = false;
			for(TreeNodesListT::const_iterator it = root_.children_.begin(); 
				it != root_.children_.end(); ++it){
				assert(nullptr != *it);
				found = (after == (*it)->key_);
				if(found){
					++it;
					if(root_.children_.end() == it)
						return false;
					node = *(it);
					break;
				}
			}
			if(!found)
				return false;
		}
		assert(nullptr != node);
		*key = node->key_;
		*value = node->value_;
	}
	return true;
}

bool NLinkTree::next(K *key, V *value)
{
	{
		NTreeNode *node = nullptr;
		if(nullptr == current_){
			if(root_.children_.empty()){
				aux::ExchLogger::instance()->debug("NLinkTree::next() - root has no childrent.");
				return false;
			}
			node = *(root_.children_.begin());
		}else{
			bool found = false;
			for(TreeNodesListT::const_iterator it = root_.children_.begin(); 
				it != root_.children_.end(); ++it){
				assert(nullptr != *it);
				found = (*it == current_);
				if(found){
					++it;
					if(root_.children_.end() == it){
						aux::ExchLogger::instance()->debug("NLinkTree::next() - last child.");
						return false;
					}
					node = *(it);
					break;
				}
			}
			if(!found){
				aux::ExchLogger::instance()->debug("NLinkTree::next() - not found.");
				return false;
			}
		}
		assert(nullptr != node);
		*key = node->key_;
		*value = node->value_;
		current_ = node;
	}
	if(aux::ExchLogger::instance()->isDebugOn()){
		NLTLogMessage l(NEXT_FINAL_MSG, *key, 0, 0);
		aux::ExchLogger::instance()->debug(l);
	}
	return true;
}

bool NLinkTree::current(K *key, V *value)const
{
	{
		NTreeNode *node = nullptr;
		if(nullptr == current_){
			return false;
		}else{
			bool found = false;
			for(TreeNodesListT::const_iterator it = root_.children_.begin(); 
				it != root_.children_.end(); ++it){
				assert(nullptr != *it);
				found = (*it == current_);
				if(found){
					node = *(it);
					break;
				}
			}
			if(!found)
				return false;
		}
		assert(nullptr != node);
		*key = node->key_;
		*value = node->value_;
	}
	return true;
}

bool NLinkTree::isCurrentValid()const
{
	if(nullptr == current_)
		return false;
	for(TreeNodesListT::const_iterator it = root_.children_.begin(); 
		it != root_.children_.end(); ++it){
		assert(nullptr != *it);
		if(*it == current_)
			return true;
	}
	return false;
}

void NLinkTree::dumpTree()
{
	char buf[64];
	buf[0] = 0;
	aux::toStr(buf, keys_.size());
	aux::ExchLogger::instance()->fatal(string("Start duming tree: tree contains elements ") + buf);
	string keyVal;
	for(KParamsT::const_iterator it = keys_.begin(); it != keys_.end(); ++it){
		if(nullptr == it->second)
			break;
		keyVal.clear();
		it->first.toString(keyVal);
		string text = "El '" + keyVal + "' depends on: ";
		for(int i = 0; i < it->second->dependsOn_.size_; ++i){
			keyVal.clear();
			it->second->dependsOn_.list_[i].id_.toString(keyVal);
			text += "[" + keyVal + ", ";
			buf[0] = 0;
			aux::toStr(buf, static_cast<int>(it->second->dependsOn_.list_[i].type_));
			text += buf;
			text += "]";
		}

		assert(nullptr != it->second->node_);
		text += ". Parents: ";
		for(TreeNodesT::const_iterator iit = it->second->node_->parents_.begin(); 
			iit != it->second->node_->parents_.end(); ++iit){
			keyVal.clear();
			(*iit)->key_.toString(keyVal);
			text += "[" + keyVal + "] ";
		}
		text += ". Children: ";
		for(TreeNodesT::const_iterator iit = it->second->node_->children_.begin(); 
			iit!= it->second->node_->children_.end(); ++iit){
			keyVal.clear();
			(*iit)->key_.toString(keyVal);
			text += "[" + keyVal + "] ";
		}

		aux::ExchLogger::instance()->fatal(text);
	}
	aux::ExchLogger::instance()->fatal(string("Finished duming tree"));
}