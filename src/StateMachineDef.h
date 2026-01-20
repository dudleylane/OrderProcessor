/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

namespace COP{
	struct OrderEntry;

namespace OrdState{

	struct OrderStatePersistence{
		OrderEntry *orderData_;
		int stateZone1Id_;	
		int stateZone2Id_;

		OrderStatePersistence(): orderData_(nullptr), stateZone1Id_(-1), stateZone2Id_(-1){}

		void serialize(std::string &msg)const;
		char *serialize(char *msg)const;
		const char *restore(const char *buf, size_t size);
		bool compare(const OrderStatePersistence &val)const;
	};

}
}