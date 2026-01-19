/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "Logger.h"
#include "ExchUtils.h"
#include <tbb/mutex.h>
#include <tbb/atomic.h>
#include <sys/types.h>
#include <sys/timeb.h>

#define BOOST_LOG_COMPILE_FAST_OFF
#include <boost/logging/logging.hpp>
#include <boost/logging/format.hpp>

using namespace aux;
using namespace boost::logging;

namespace{
	struct no_gather {
		typedef const char* msg_type;

		const char * m_msg;
		no_gather() : m_msg(0) {}
		const char * msg() const { return m_msg; }
		void *out(const char* msg) { m_msg = msg; return this; }
		void *out(const std::string& msg) { m_msg = msg.c_str(); return this; }
	};

	typedef logger< no_gather, destination::cout > out_log_type;
	typedef logger< no_gather, destination::file > file_log_type;

	BOOST_DEFINE_LOG_FILTER(g_log_filter, filter::no_ts ) 

	BOOST_DEFINE_LOG(out_log, out_log_type)
	BOOST_DEFINE_LOG_WITH_ARGS( exch_log, file_log_type, ("exchange.log") )

	#define OUT_LOG_ BOOST_LOG_USE_SIMPLE_LOG_IF_FILTER(out_log(), g_log_filter()->is_enabled() ) 
	#define EXCH_LOG_ BOOST_LOG_USE_SIMPLE_LOG_IF_FILTER(exch_log(), g_log_filter()->is_enabled() ) 

	char *addTimeStamp(char *ptr){
		__int64 ltime = 0;
		_time64( &ltime );
		struct tm *newtime = _gmtime64( &ltime );
		struct _timeb tstruct;
		_ftime( &tstruct ); 

		/// format is YYYYMMDD-HH:MM:SS.sss
		ptr = toStr(ptr, 1900 + newtime->tm_year);
		ptr = toStr(ptr, newtime->tm_mon + 1, 2);
		ptr = toStr(ptr, newtime->tm_mday, 2);
		*ptr = '-'; ++ptr;
		ptr = toStr(ptr, newtime->tm_hour, 2);
		*ptr = ':'; ++ptr;
		ptr = toStr(ptr, newtime->tm_min, 2);
		*ptr = ':'; ++ptr;
		ptr = toStr(ptr, newtime->tm_sec, 2);
		*ptr = '.'; ++ptr;
		ptr = toStr(ptr, tstruct.millitm, 3);
		
		return ptr;
	}

	const std::string DEBUG_PREFIX(": [Debug][");
	const std::string NOTE_PREFIX(": [Note][");
	const std::string WARN_PREFIX(": [Warn][");
	const std::string ERROR_PREFIX(": [Error][");
	const std::string FATAL_PREFIX(": [Fatal][");

	char *prepareRecord(char *buf, const char *prefix, size_t prefixSize){
		buf[0] = 0;
		
		///prepare record prefix: TimeStamp: [<Severity>][<ThreadId>] <message>
		char *ptr = addTimeStamp(buf);
		memcpy(ptr, prefix, prefixSize + 1); 
		ptr += prefixSize;

		ptr = toStr(ptr, static_cast<size_t>(GetCurrentThreadId()));
		memcpy(ptr, "] \0", 3); ptr += 2;
		
		return ptr;
	}
}

namespace aux{
	const size_t BUFFER_SIZE = 256;
	struct LoggerImpl{
		char buf_[BUFFER_SIZE + 1];
		mutable tbb::mutex lock_;

		enum LoggingMask{
			ALL_DISABLED_FLAG = 0,
			DEBUG_ON_FLAG = 1,
			NOTE_ON_FLAG = 2,
			WARN_ON_FLAG = 4,
			ERROR_ON_FLAG = 8,
			FATAL_ON_FLAG = 16,
			ALL_ENABLED_FLAG = 31
		};
		tbb::atomic<char> loggingMask_;

		LoggerImpl(){
			loggingMask_.fetch_and_store(ALL_ENABLED_FLAG);
		}

		void setFlag(bool enabled, char flag){
			char before = LoggerImpl::ALL_DISABLED_FLAG, 
				 current = LoggerImpl::ALL_DISABLED_FLAG;
			do{
				before = loggingMask_;
				char newVal = before;
				if(enabled){
					if(0 != (before&flag)) // flag already assigned
						return;
					newVal |= flag;
				}else{
					if(0 == (before&~flag)) //flag already removed
						return;
					newVal &= ~flag;
				}
				current = loggingMask_.compare_and_swap(newVal, before);
			}while(current != before);
		}

		bool isFlag(char flag){
			char mask = loggingMask_;
			return 0 != (mask&flag);
		}

		void logMessage(const std::string &msg, char flag, const std::string &prefix){
			if(!isFlag(flag))
				return;

			//size_t msgLen = msg.size();
			tbb::mutex::scoped_lock lock(lock_);

			prepareRecord(buf_, prefix.c_str(), prefix.length());

			/// write message to the log
			EXCH_LOG_(buf_);
			EXCH_LOG_(msg.c_str());
			EXCH_LOG_("\n");
		}

		void logMessage(const char *msg, char flag, const std::string &prefix){
			if(!isFlag(flag))
				return;

			size_t msgLen = strlen(msg);
			tbb::mutex::scoped_lock lock(lock_);

			char *ptr = prepareRecord(buf_, prefix.c_str(), prefix.length());

			if(BUFFER_SIZE - (ptr - buf_) - 2 > msgLen){
				strncat(ptr, msg, msgLen);
				ptr[msgLen] = '\n';
				ptr[msgLen + 1] = 0;
				EXCH_LOG_(buf_);
			}else{
				EXCH_LOG_(buf_);
				EXCH_LOG_(msg);
				EXCH_LOG_("\n");
			}
		}

		void logMessage(int val, char flag, const std::string &prefix){
			if(!isFlag(flag))
				return;

			tbb::mutex::scoped_lock lock(lock_);

			char *ptr = prepareRecord(buf_, prefix.c_str(), prefix.length());

			ptr = toStr(ptr, val);
			ptr[0] = '\n';
			ptr[1] = 0;
			EXCH_LOG_(buf_);
		}

		void logMessage(LogMessage &msg, char flag, const std::string &prefix){
			if(!isFlag(flag))
				return;

			msg.prepareMessage();

			tbb::mutex::scoped_lock lock(lock_);

			char *ptr = prepareRecord(buf_, prefix.c_str(), prefix.length());

			/// write message to the log
			size_t msgLen = 0;
			const char *msgBuf = NULL;
			if((!msg.isBinary()) && (NULL != (msgBuf = msg.getMessage(&msgLen))) && 
				(BUFFER_SIZE - (ptr - buf_) - 2 > msgLen)){
				strncat(ptr, msgBuf, msgLen);
				ptr[msgLen] = '\n';
				ptr[msgLen + 1] = 0;
				EXCH_LOG_(buf_);
			}else{
				EXCH_LOG_(buf_);
				EXCH_LOG_(msg.getMessage());
				EXCH_LOG_("\n");
			}
		}

	};
}

Logger::Logger(void): impl_(NULL)
{
    out_log()->mark_as_initialized();
    exch_log()->mark_as_initialized();

	impl_ = new LoggerImpl();

	note("----------------------------------------------------------------------");
	note(" LogSystem initialized.");
}

Logger::~Logger(void)
{
	note(" LogSystem stopped.");
	note("----------------------------------------------------------------------");
	delete impl_;
}

void Logger::setDebugOn(bool val)
{
	assert(NULL != impl_);
	impl_->setFlag(val, LoggerImpl::DEBUG_ON_FLAG);
	if(val)
		warn("Debug logging turned on.");
	else
		warn("Debug logging turned off.");
}

void Logger::setNoteOn(bool val)
{
	assert(NULL != impl_);
	impl_->setFlag(val, LoggerImpl::NOTE_ON_FLAG);
	if(val)
		warn("Notification logging turned on.");
	else
		warn("Notification logging turned off.");
}

void Logger::setWarnOn(bool val)
{
	assert(NULL != impl_);
	impl_->setFlag(val, LoggerImpl::WARN_ON_FLAG);
	if(val)
		warn("Warning logging turned on.");
	else
		warn("Warning logging turned off.");
}

void Logger::setErrorOn(bool val)
{
	assert(NULL != impl_);
	impl_->setFlag(val, LoggerImpl::ERROR_ON_FLAG);
	if(val)
		warn("Error logging turned on.");
	else
		warn("Error logging turned off.");
}

void Logger::setFatalOn(bool val)
{
	assert(NULL != impl_);
	impl_->setFlag(val, LoggerImpl::FATAL_ON_FLAG);
	if(val)
		warn("Fatal logging turned on.");
	else
		warn("Fatal logging turned off.");
}

bool Logger::isDebugOn()const
{
	assert(NULL != impl_);
	return impl_->isFlag(LoggerImpl::DEBUG_ON_FLAG);
}

bool Logger::isNoteOn()const
{
	assert(NULL != impl_);
	return impl_->isFlag(LoggerImpl::NOTE_ON_FLAG);
}

bool Logger::isWarnOn()const
{
	assert(NULL != impl_);
	return impl_->isFlag(LoggerImpl::WARN_ON_FLAG);
}

bool Logger::isErrorOn()const
{
	assert(NULL != impl_);
	return impl_->isFlag(LoggerImpl::ERROR_ON_FLAG);
}

bool Logger::isFatalOn()const
{
	assert(NULL != impl_);
	return impl_->isFlag(LoggerImpl::FATAL_ON_FLAG);
}


void Logger::debug(const std::string &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(const char *msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(int val){
	assert(NULL != impl_);
	impl_->logMessage(val, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(LogMessage &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::note(const std::string &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(const char *msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(int val){
	assert(NULL != impl_);
	impl_->logMessage(val, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(LogMessage &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::warn(const std::string &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(const char *msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(int val){
	assert(NULL != impl_);
	impl_->logMessage(val, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(LogMessage &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::error(const std::string &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(const char *msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(int val){
	assert(NULL != impl_);
	impl_->logMessage(val, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(LogMessage &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::fatal(const std::string &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(const char *msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(int val){
	assert(NULL != impl_);
	impl_->logMessage(val, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(LogMessage &msg){
	assert(NULL != impl_);
	impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}
