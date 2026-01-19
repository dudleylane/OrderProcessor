/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "DataModelDef.h"
#include "FilterImpl.h"

namespace COP{ namespace SubscrMgr{

	class InstrumentElementFilter{
	public:
		virtual ~InstrumentElementFilter(){}
		virtual bool match(const InstrumentEntry &params)const = 0;
		virtual bool getInstrumentEntry(InstrumentEntry *val)const = 0;
	};

	class InstrumentIdFilter: public InstrumentElementFilter{
	public:
		InstrumentIdFilter(Impl::IdTFilter *filter);
		virtual bool match(const InstrumentEntry &params)const;
		virtual bool getInstrumentEntry(InstrumentEntry *val)const = 0;
	private:
		Impl::IdTFilter impl_;
	};

	class InstrumentSymbolFilter: public InstrumentElementFilter{
	public:
		InstrumentSymbolFilter(Impl::StringFilter *filter);
		~InstrumentSymbolFilter();
		virtual bool match(const InstrumentEntry &params)const;
		virtual bool getInstrumentEntry(InstrumentEntry *val)const;
	private:
		Impl::StringFilter *filter_;
	};

	class InstrumentSecurityIdFilter: public InstrumentElementFilter{
	public:
		InstrumentSecurityIdFilter(Impl::StringFilter *filter);
		virtual bool match(const InstrumentEntry &params)const;
		virtual bool getInstrumentEntry(InstrumentEntry *val)const;
	private:
		Impl::StringFilter *filter_;
	};

	class InstrumentSecurityIdSourceFilter: public InstrumentElementFilter{
	public:
		InstrumentSecurityIdSourceFilter(Impl::StringFilter *filter);
		virtual bool match(const InstrumentEntry &params)const;
		virtual bool getInstrumentEntry(InstrumentEntry *val)const;
	private:
		Impl::StringFilter *filter_;
	};

	class InstrumentFilter{
	public:
		InstrumentFilter(){}
		~InstrumentFilter(){}
		void addFilter(InstrumentElementFilter *fltr);

		bool match(const OrderParams &params)const;

		void release();

		bool getInstrumentEntry(InstrumentEntry *val)const;
	private:
		typedef std::deque<InstrumentElementFilter *> FiltersT;
		FiltersT filters_;
		//IdT id_;
		//StringT symbol_;
		//StringT securityId_;
		//StringT securityIdSource_;	
	};

	class AccountElementFilter{
	public:
		virtual ~AccountElementFilter(){}
		virtual bool match(const AccountEntry &params)const = 0;
	};

	class AccountIdFilter: public AccountElementFilter{
	public:
		AccountIdFilter(Impl::IdTFilter *filter);
		virtual bool match(const AccountEntry &params)const;
	private:
		Impl::IdTFilter impl_;
	};

	class AccountAccountFilter: public AccountElementFilter{
	public:
		AccountAccountFilter(Impl::StringFilter *filter);
		virtual bool match(const AccountEntry &params)const;
	private:
		Impl::StringFilter *filter_;
	};

	class AccountFirmFilter: public AccountElementFilter{
	public:
		AccountFirmFilter(Impl::StringFilter *filter);
		virtual bool match(const AccountEntry &params)const;
	private:
		Impl::StringFilter * filter_;
	};

	class AccountTypeEqualFilter: public AccountElementFilter{
	public:
		AccountTypeEqualFilter(AccountType value);
		virtual bool match(const AccountEntry &params)const;
	private:
		Impl::EnumEqualFilter<AccountType> impl_;
	};

	class AccountTypeInFilter: private Impl::EnumInSetFilter<AccountType>, public AccountElementFilter{
	public:
		AccountTypeInFilter(const ValuesT &values);
		virtual bool match(const AccountEntry &params)const;
	};

	class AccountFilter{
	public:
		void addFilter(AccountElementFilter *fltr);
		bool match(const OrderParams &params)const;

		void release();
	private:
		typedef std::deque<AccountElementFilter *> FiltersT;
		FiltersT filters_;
		//IdT id_;
		//StringT account_;
		//StringT firm_;
		//AccountType type_;	
	};

	class ClearingElementFilter{
	public:
		virtual ~ClearingElementFilter(){}
		virtual bool match(const ClearingEntry &params)const = 0;
	};

	class ClearingIdFilter: public ClearingElementFilter{
	public:
		ClearingIdFilter(Impl::IdTFilter *filter);
		virtual bool match(const ClearingEntry &params)const;
	private:
		Impl::IdTFilter impl_;
	};

	class ClearingFirmFilter: public ClearingElementFilter{
	public:
		ClearingFirmFilter(Impl::StringFilter *filter);
		virtual bool match(const ClearingEntry &params)const;
	private:
		Impl::StringFilter *filter_;
	};

	class ClearingFilter{
	public:
		void addFilter(ClearingElementFilter *fltr);
		bool match(const OrderParams &params)const;

		void release();
	private:
		typedef std::deque<ClearingElementFilter *> FiltersT;
		FiltersT filters_;
		//IdT id_;
		//StringT firm_;
	};

}}