/***
    This file is part of snapcast
    Copyright (C) 2014-2019  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "client_connection.hpp"
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/str_compat.hpp"
#include "message/hello.hpp"
#include <iostream>
#include <mutex>


using namespace std;


ClientConnection::ClientConnection(MessageReceiver* receiver, const std::string& host, size_t port)
    : socket_(io_context_), active_(false), messageReceiver_(receiver), reqId_(1), host_(host), port_(port), readerThread_(nullptr),
      sumTimeout_(chronos::msec(0))
{
}


ClientConnection::~ClientConnection()
{
    stop();
}



void ClientConnection::socketRead(void* _to, size_t _bytes)
{
    size_t toRead = _bytes;
    size_t len = 0;
    do
    {
        len += socket_.read_some(boost::asio::buffer((char*)_to + len, toRead));
        // cout << "len: " << len << ", error: " << error << endl;
        toRead = _bytes - len;
    } while (toRead > 0);
}


std::string ClientConnection::getMacAddress()
{
    std::string mac = ::getMacAddress(socket_.native_handle());
    if (mac.empty())
        mac = "00:00:00:00:00:00";
    LOG(INFO) << "My MAC: \"" << mac << "\", socket: " << socket_.native_handle() << "\n";
    return mac;
}


void ClientConnection::start()
{
    tcp::resolver resolver(io_context_);
    tcp::resolver::query query(host_, cpt::to_string(port_), boost::asio::ip::resolver_query_base::numeric_service);
    auto iterator = resolver.resolve(query);
    LOG(DEBUG) << "Connecting\n";
    //	struct timeval tv;
    //	tv.tv_sec  = 5;
    //	tv.tv_usec = 0;
    //	cout << "socket: " << socket->native_handle() << "\n";
    //	setsockopt(socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    //	setsockopt(socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    socket_.connect(*iterator);
    SLOG(NOTICE) << "Connected to " << socket_.remote_endpoint().address().to_string() << endl;
    active_ = true;
    sumTimeout_ = chronos::msec(0);
    readerThread_ = new thread(&ClientConnection::reader, this);
}


void ClientConnection::stop()
{
    active_ = false;
    try
    {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        if (ec)
            LOG(ERROR) << "Error in socket shutdown: " << ec.message() << endl;
        socket_.close(ec);
        if (ec)
            LOG(ERROR) << "Error in socket close: " << ec.message() << endl;
        if (readerThread_)
        {
            LOG(DEBUG) << "joining readerThread\n";
            readerThread_->join();
            delete readerThread_;
        }
    }
    catch (...)
    {
    }
    readerThread_ = nullptr;
    LOG(DEBUG) << "readerThread terminated\n";
}


bool ClientConnection::send(const msg::BaseMessage* message)
{
    //	std::unique_lock<std::mutex> mlock(mutex_);
    // LOG(DEBUG) << "send: " << message->type << ", size: " << message->getSize() << "\n";
    std::lock_guard<std::mutex> socketLock(socketMutex_);
    if (!socket_.is_open())
        return false;
    // LOG(DEBUG) << "send: " << message->type << ", size: " << message->getSize() << "\n";
    boost::asio::streambuf streambuf;
    std::ostream stream(&streambuf);
    tv t;
    message->sent = t;
    message->serialize(stream);
    boost::asio::write(socket_, streambuf);
    return true;
}


shared_ptr<msg::SerializedMessage> ClientConnection::sendRequest(const msg::BaseMessage* message, const chronos::msec& timeout)
{
    shared_ptr<msg::SerializedMessage> response(nullptr);
    if (++reqId_ >= 10000)
        reqId_ = 1;
    message->id = reqId_;
    //	LOG(INFO) << "Req: " << message->id << "\n";
    shared_ptr<PendingRequest> pendingRequest(new PendingRequest(reqId_));

    std::unique_lock<std::mutex> lock(pendingRequestsMutex_);
    pendingRequests_.insert(pendingRequest);
    send(message);
    if (pendingRequest->cv.wait_for(lock, std::chrono::milliseconds(timeout)) == std::cv_status::no_timeout)
    {
        response = pendingRequest->response;
        sumTimeout_ = chronos::msec(0);
        //		LOG(INFO) << "Resp: " << pendingRequest->id << "\n";
    }
    else
    {
        sumTimeout_ += timeout;
        LOG(WARNING) << "timeout while waiting for response to: " << reqId_ << ", timeout " << sumTimeout_.count() << "\n";
        if (sumTimeout_ > chronos::sec(10))
            throw SnapException("sum timeout exceeded 10s");
    }
    pendingRequests_.erase(pendingRequest);
    return response;
}


void ClientConnection::getNextMessage()
{
    msg::BaseMessage baseMessage;
    size_t baseMsgSize = baseMessage.getSize();
    vector<char> buffer(baseMsgSize);
    socketRead(&buffer[0], baseMsgSize);
    baseMessage.deserialize(&buffer[0]);
    //	LOG(DEBUG) << "getNextMessage: " << baseMessage.type << ", size: " << baseMessage.size << ", id: " << baseMessage.id << ", refers: " <<
    // baseMessage.refersTo << "\n";
    if (baseMessage.size > buffer.size())
        buffer.resize(baseMessage.size);
    //	{
    //		std::lock_guard<std::mutex> socketLock(socketMutex_);
    socketRead(&buffer[0], baseMessage.size);
    //	}
    tv t;
    baseMessage.received = t;

    {
        std::unique_lock<std::mutex> lock(pendingRequestsMutex_);
        //		LOG(DEBUG) << "got lock - getNextMessage: " << baseMessage.type << ", size: " << baseMessage.size << ", id: " << baseMessage.id << ",
        // refers: " << baseMessage.refersTo << "\n";
        {
            for (auto req : pendingRequests_)
            {
                if (req->id == baseMessage.refersTo)
                {
                    req->response.reset(new msg::SerializedMessage());
                    req->response->message = baseMessage;
                    req->response->buffer = (char*)malloc(baseMessage.size);
                    memcpy(req->response->buffer, &buffer[0], baseMessage.size);
                    lock.unlock();
                    req->cv.notify_one();
                    return;
                }
            }
        }
    }

    if (messageReceiver_ != nullptr)
        messageReceiver_->onMessageReceived(this, baseMessage, &buffer[0]);
}



void ClientConnection::reader()
{
    try
    {
        while (active_)
        {
            getNextMessage();
        }
    }
    catch (const std::exception& e)
    {
        if (messageReceiver_ != nullptr)
            messageReceiver_->onException(this, make_shared<SnapException>(e.what()));
    }
    catch (...)
    {
    }
    active_ = false;
}
