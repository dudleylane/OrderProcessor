#ifdef BUILD_FIX

#include "FixStores.h"

#include "FixGateway.h"
#include "Logger.h"

#include <quickfix/FieldConvertors.h>
#include <quickfix/FieldNumbers.h>
#include <quickfix/FileStore.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>

#include <charconv>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

using namespace COP::App;

namespace
{
constexpr char SOH = '\001';

/// FileStore's name for a session's files: BeginString-SenderCompID-TargetCompID, and the qualifier when there is one
std::string storeName(const FIX::SessionID &id)
{
    std::string name = id.getBeginString().getString() + "-" + id.getSenderCompID().getString() + "-" +
                       id.getTargetCompID().getString();
    if (!id.getSessionQualifier().empty())
    {
        name += "-" + id.getSessionQualifier();
    }
    return name;
}

/// A creation time as FileStore writes it in its session file, to the second, which is how it reads it back
std::string storedTime(const FIX::UtcTimeStamp &time)
{
    return FIX::UtcTimeStampConvertor::convert(time);
}
} // namespace

FixCarryingStore::FixCarryingStore(FIX::MessageStore *inner, std::optional<FIX::TimeRange> sessionTime,
                                   std::filesystem::path markFile, std::string name)
    : inner_(inner), sessionTime_(std::move(sessionTime)), markFile_(std::move(markFile)), name_(std::move(name))
{
    unsentFrom_ = readMark();
}

void FixCarryingStore::attach(FIX::Session *session)
{
    session_.store(session);
}

bool FixCarryingStore::set(FIX::SEQNUM seq, const std::string &message)
{
    const bool stored = inner_->set(seq, message);
    // QuickFIX stores each message it sends before it decides, under the same lock, whether to send it: only to a
    // client that is logged on
    if ((0 == unsentFrom_) && !loggedOn() && isApplication(message))
    {
        unsentFrom_ = seq;
        keepMark();
    }
    return stored;
}

void FixCarryingStore::get(FIX::SEQNUM begin, FIX::SEQNUM end, std::vector<std::string> &messages) const
{
    inner_->get(begin, end, messages);
    // QuickFIX reads its store only to answer a ResendRequest, with what it sends the client now. A client that asks
    // from begin has what came before.
    if ((0 != unsentFrom_) && (unsentFrom_ <= end))
    {
        unsentFrom_ = (end + 1 < inner_->getNextSenderMsgSeqNum()) ? end + 1 : 0;
        keepMark();
    }
}

FIX::SEQNUM FixCarryingStore::getNextSenderMsgSeqNum() const
{
    return inner_->getNextSenderMsgSeqNum();
}

FIX::SEQNUM FixCarryingStore::getNextTargetMsgSeqNum() const
{
    return inner_->getNextTargetMsgSeqNum();
}

void FixCarryingStore::setNextSenderMsgSeqNum(FIX::SEQNUM seq)
{
    inner_->setNextSenderMsgSeqNum(seq);
}

void FixCarryingStore::setNextTargetMsgSeqNum(FIX::SEQNUM seq)
{
    inner_->setNextTargetMsgSeqNum(seq);
}

void FixCarryingStore::incrNextSenderMsgSeqNum()
{
    inner_->incrNextSenderMsgSeqNum();
}

void FixCarryingStore::incrNextTargetMsgSeqNum()
{
    inner_->incrNextTargetMsgSeqNum();
}

FIX::UtcTimeStamp FixCarryingStore::getCreationTime() const
{
    return inner_->getCreationTime();
}

void FixCarryingStore::reset(const FIX::UtcTimeStamp &now)
{
    std::vector<std::string> carried;
    // QuickFIX resets a store for a new session day when the store's day isn't now's, which is this test
    // (Session::checkSessionTime()), and for nothing else unless a Logon asks for it, within the day
    if ((0 != unsentFrom_) && sessionTime_ && !sessionTime_->isInSameRange(now, inner_->getCreationTime()))
    {
        std::vector<std::string> stored;
        inner_->get(unsentFrom_, inner_->getNextSenderMsgSeqNum() - 1, stored);
        for (std::string &message : stored)
        {
            // A session message is the old day's alone: a ResendRequest gets a gap fill in its place
            if (isApplication(message))
            {
                carried.push_back(std::move(message));
            }
        }
    }
    inner_->reset(now);
    unsentFrom_ = 0;
    FIX::SEQNUM seq = 1;
    for (const std::string &message : carried)
    {
        if (const std::optional<std::string> renumbered = restamped(message, seq))
        {
            inner_->set(seq++, *renumbered);
        }
        else
        {
            aux::ExchLogger::instance()->error("FIX session " + name_ +
                                               ": a message its client hasn't received isn't whole, and can't be "
                                               "carried into the new session day: " +
                                               message.substr(0, 200));
        }
    }
    if (1 < seq)
    {
        inner_->setNextSenderMsgSeqNum(seq);
        unsentFrom_ = 1;
        aux::ExchLogger::instance()->note("FIX session " + name_ + ": the new session day begins with the " +
                                          std::to_string(seq - 1) +
                                          " messages its client hasn't received, from MsgSeqNum 1 (#513)");
    }
    keepMark();
}

void FixCarryingStore::refresh()
{
    inner_->refresh();
    if (!markFile_.empty())
    {
        unsentFrom_ = readMark();
    }
}

FIX::SEQNUM FixCarryingStore::unsentFrom() const
{
    return unsentFrom_;
}

FIX::MessageStore *FixCarryingStore::inner() const
{
    return inner_;
}

std::optional<std::string> FixCarryingStore::restamped(const std::string &message, FIX::SEQNUM seq)
{
    // 8=<BeginString>|9=<BodyLength>|<the header's other fields><the body's>|10=<CheckSum>|, where BodyLength counts the
    // bytes after its own field up to 10=, and CheckSum is the sum of every byte before 10=, modulo 256
    const std::size_t lengthField = message.find("\0019=");
    const std::size_t checkSumField = message.rfind("\00110=");
    if ((std::string::npos == lengthField) || (std::string::npos == checkSumField) || (checkSumField <= lengthField))
    {
        return std::nullopt;
    }
    const std::size_t lengthEnd = message.find(SOH, lengthField + 3);
    if (checkSumField < lengthEnd)
    {
        return std::nullopt;
    }
    // The header's fields come first: each is copied, but MsgSeqNum (34), which becomes seq, and PossResend (97),
    // which becomes Y, or is added where the header ends. The body is copied as it is.
    std::string fields;
    bool numbered = false;
    bool possResend = false;
    std::size_t field = lengthEnd + 1;
    while (field <= checkSumField)
    {
        const std::size_t equals = message.find('=', field);
        const std::size_t end = message.find(SOH, field);
        if ((std::string::npos == equals) || (end < equals))
        {
            return std::nullopt;
        }
        int tag = 0;
        const auto [parsed, error] = std::from_chars(message.data() + field, message.data() + equals, tag);
        if ((std::errc() != error) || (message.data() + equals != parsed))
        {
            return std::nullopt;
        }
        if (!FIX::Message::isHeaderField(tag))
        {
            break;
        }
        if (FIX::FIELD::MsgSeqNum == tag)
        {
            fields += "34=" + std::to_string(seq) + SOH;
            numbered = true;
        }
        else if (FIX::FIELD::PossResend == tag)
        {
            fields += "97=Y";
            fields += SOH;
            possResend = true;
        }
        else
        {
            fields.append(message, field, end + 1 - field);
        }
        field = end + 1;
    }
    if (!numbered)
    {
        return std::nullopt;
    }
    if (!possResend)
    {
        fields += "97=Y";
        fields += SOH;
    }
    fields.append(message, field, checkSumField + 1 - field);

    std::string renumbered = message.substr(0, lengthField + 3) + std::to_string(fields.size()) + SOH + fields;
    unsigned sum = 0;
    for (const char c : renumbered)
    {
        sum += static_cast<unsigned char>(c);
    }
    char checkSum[4];
    std::snprintf(checkSum, sizeof(checkSum), "%03u", sum % 256);
    renumbered += "10=";
    renumbered += checkSum;
    renumbered += SOH;
    return renumbered;
}

bool FixCarryingStore::isApplication(const std::string &message)
{
    const std::size_t typeField = message.find("\00135=");
    if (std::string::npos == typeField)
    {
        return false;
    }
    const std::size_t typeEnd = message.find(SOH, typeField + 4);
    return (std::string::npos != typeEnd) &&
           !FIX::Message::isAdminMsgType(message.substr(typeField + 4, typeEnd - (typeField + 4)));
}

bool FixCarryingStore::loggedOn() const
{
    FIX::Session *session = session_.load();
    return (nullptr != session) && session->isLoggedOn();
}

FIX::SEQNUM FixCarryingStore::readMark() const
{
    if (markFile_.empty())
    {
        return 0;
    }
    std::ifstream in(markFile_);
    std::string created;
    FIX::SEQNUM mark = 0;
    if (!(in >> created >> mark))
    {
        return 0;
    }
    // A mark is for the day of the store it was kept for, which a crash between the store's reset and the mark's write
    // would leave behind
    if ((storedTime(inner_->getCreationTime()) != created) || (0 == mark) || (inner_->getNextSenderMsgSeqNum() <= mark))
    {
        aux::ExchLogger::instance()->warn("FIX session " + name_ + ": " + markFile_.string() +
                                          " is for another session day than its store's, and is left out");
        return 0;
    }
    return mark;
}

void FixCarryingStore::keepMark() const
{
    if (markFile_.empty())
    {
        return;
    }
    std::error_code error;
    if (0 == unsentFrom_)
    {
        std::filesystem::remove(markFile_, error);
    }
    else
    {
        // Whole or not at all: a new file in its place. As durable as FileStore's own files, which QuickFIX flushes
        // and doesn't sync.
        std::filesystem::path written = markFile_;
        written += ".new";
        std::ofstream out(written, std::ios::trunc);
        out << storedTime(inner_->getCreationTime()) << '\n' << unsentFrom_ << '\n';
        out.close();
        if (!out)
        {
            error = std::make_error_code(std::errc::io_error);
        }
        else
        {
            std::filesystem::rename(written, markFile_, error);
        }
    }
    if (error)
    {
        aux::ExchLogger::instance()->error("FIX session " + name_ +
                                           ": can't keep the first message its client hasn't "
                                           "received, " +
                                           std::to_string(unsentFrom_) + ", in " + markFile_.string() + ": " +
                                           error.message() + ". A restart after the session day ends loses it.");
    }
}

FixCarryingStoreFactory::FixCarryingStoreFactory(const FIX::SessionSettings &settings,
                                                 std::unique_ptr<FIX::MessageStoreFactory> inner, bool keepMarks,
                                                 FixGateway *gateway)
    : settings_(settings), inner_(std::move(inner)), keepMarks_(keepMarks), gateway_(gateway)
{
    if (nullptr != gateway_)
    {
        gateway_->setStores(this);
    }
}

FixCarryingStoreFactory::~FixCarryingStoreFactory()
{
    if (nullptr != gateway_)
    {
        gateway_->setStores(nullptr);
    }
}

FIX::MessageStore *FixCarryingStoreFactory::create(const FIX::UtcTimeStamp &now, const FIX::SessionID &sid)
{
    const FIX::Dictionary &settings = settings_.get(sid);
    std::optional<FIX::TimeRange> period = sessionTime(settings);
    std::filesystem::path markFile;
    if (keepMarks_)
    {
        markFile = std::filesystem::path(settings.getString(FIX::FILE_STORE_PATH)) / (storeName(sid) + ".unsent");
    }
    FIX::MessageStore *inner = inner_->create(now, sid);
    std::unique_ptr<FixCarryingStore> store;
    try
    {
        store = std::make_unique<FixCarryingStore>(inner, std::move(period), std::move(markFile), sid.toString());
    }
    catch (...)
    {
        inner_->destroy(inner);
        throw;
    }
    std::lock_guard<std::mutex> lock(lock_);
    stores_[sid] = store.get();
    return store.release();
}

void FixCarryingStoreFactory::destroy(FIX::MessageStore *store)
{
    auto *carrying = static_cast<FixCarryingStore *>(store);
    {
        std::lock_guard<std::mutex> lock(lock_);
        std::erase_if(stores_,
                      [carrying](const auto &entry)
                      {
                          return carrying == entry.second;
                      });
    }
    FIX::MessageStore *inner = carrying->inner();
    delete carrying;
    inner_->destroy(inner);
}

void FixCarryingStoreFactory::attach(const FIX::SessionID &sid, FIX::Session *session)
{
    std::lock_guard<std::mutex> lock(lock_);
    const auto found = stores_.find(sid);
    if (stores_.end() != found)
    {
        found->second->attach(session);
    }
}

FixCarryingStore *FixCarryingStoreFactory::storeOf(const FIX::SessionID &sid)
{
    std::lock_guard<std::mutex> lock(lock_);
    const auto found = stores_.find(sid);
    return (stores_.end() != found) ? found->second : nullptr;
}

std::optional<FIX::TimeRange> FixCarryingStoreFactory::sessionTime(const FIX::Dictionary &settings)
{
    // As QuickFIX 22.1's SessionFactory::create() reads them
    if (settings.has(FIX::NON_STOP_SESSION) && settings.getBool(FIX::NON_STOP_SESSION))
    {
        return std::nullopt;
    }
    const bool useLocalTime = settings.has(FIX::USE_LOCAL_TIME) && settings.getBool(FIX::USE_LOCAL_TIME);
    int startDay = -1;
    int endDay = -1;
    try
    {
        startDay = settings.getDay(FIX::START_DAY);
        endDay = settings.getDay(FIX::END_DAY);
    }
    catch (const FIX::ConfigError &)
    {
    }
    const FIX::UtcTimeOnly start = FIX::UtcTimeOnlyConvertor::convert(settings.getString(FIX::START_TIME));
    const FIX::UtcTimeOnly end = FIX::UtcTimeOnlyConvertor::convert(settings.getString(FIX::END_TIME));
    if (useLocalTime)
    {
        return FIX::TimeRange(FIX::LocalTimeOnly(start.getHour(), start.getMinute(), start.getSecond()),
                              FIX::LocalTimeOnly(end.getHour(), end.getMinute(), end.getSecond()), startDay, endDay);
    }
    return FIX::TimeRange(start, end, startDay, endDay);
}

std::unique_ptr<FIX::MessageStoreFactory> COP::App::makeFixStores(const FIX::SessionSettings &settings,
                                                                  FixGateway &gateway)
{
    return std::make_unique<FixCarryingStoreFactory>(settings, std::make_unique<FIX::FileStoreFactory>(settings), true,
                                                     &gateway);
}

#endif // BUILD_FIX
