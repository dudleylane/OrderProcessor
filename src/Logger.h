/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include "Singleton.h"

namespace aux{ 

	struct LoggerImpl;

	class LogMessage{
	public:
		LogMessage(): binary_(false){}
		virtual ~LogMessage(){}

		virtual void prepareMessage() = 0;
		
		const char *getMessage(size_t *size){
			assert(nullptr != size);
			*size = msg.length();
			return msg.c_str();
		}
		std::string &getMessage(){return msg;}

		bool isBinary()const{return binary_;}
	protected:
		bool binary_;
		std::string msg;
	};

	class Logger
	{
	public:
		Logger(void);
		~Logger(void);

		void debug(const std::string &msg);
		void debug(const char *msg);
		void debug(const char *msg, size_t size);
		void debug(int val);
		void debug(LogMessage &msg);
		
		void note(const std::string &msg);
		void note(const char *msg);
		void note(const char *msg, size_t size);
		void note(int val);
		void note(LogMessage &msg);

		void warn(const std::string &msg);
		void warn(const char *msg);
		void warn(const char *msg, size_t size);
		void warn(int val);
		void warn(LogMessage &msg);

		void error(const std::string &msg);
		void error(const char *msg);
		void error(const char *msg, size_t size);
		void error(int val);
		void error(LogMessage &msg);

		void fatal(const std::string &msg);
		void fatal(const char *msg);
		void fatal(const char *msg, size_t size);
		void fatal(int val);
		void fatal(LogMessage &msg);

		void setDebugOn(bool val);
		void setNoteOn(bool val);
		void setWarnOn(bool val);
		void setErrorOn(bool val);
		void setFatalOn(bool val);

		bool isDebugOn()const;
		bool isNoteOn()const;
		bool isWarnOn()const;
		bool isErrorOn()const;
		bool isFatalOn()const;

	private:
		LoggerImpl *impl_;
	};

	typedef Singleton<Logger> ExchLogger;
}