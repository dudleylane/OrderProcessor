/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TransactionDef.h"

namespace COP{

	namespace Tasks{

		class ExecTask{
		public:
			virtual ~ExecTask(){}

			virtual void execute() = 0;
		};

		class ExecTaskManager{
		public:
			virtual ~ExecTaskManager(){}

			virtual void addTask(const ACID::TransactionId &id) = 0;		
		};
	}
}