/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotVerificationServer.h"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "Log.h"
#include "PlayerbotVerificationOperation.h"

using boost::asio::ip::tcp;
using namespace PlayerbotVerification;

namespace
{
uint64 CurrentTimestampMs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

struct PlayerbotVerificationServer::Impl
{
    Impl(RequestHandler requestHandler, AuditSink recoveryAuditSink)
        : handler(std::move(requestHandler)), auditSink(std::move(recoveryAuditSink))
    {
        if (!handler)
        {
            // The socket thread only queues and waits. Every live object is resolved on the world thread.
            handler = [](Request const& request)
            { return DispatchVerificationRequest(request, PLAYERBOT_VERIFICATION_DEADLINE); };
        }
        if (!auditSink)
            auditSink = [](std::string const& record) { LOG_INFO("playerbots.audit", "{}", record); };
    }

    RequestHandler handler;
    AuditSink auditSink;
    boost::asio::io_context io;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::shared_ptr<tcp::socket> activeSocket;
    std::thread worker;
    mutable std::mutex stateMutex;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    TokenDigest tokenDigest{};

    template <typename StartOperation>
    bool CompleteWithDeadline(tcp::socket& socket, StartOperation&& startOperation, boost::system::error_code& error)
    {
        boost::asio::steady_timer timer(io);
        bool completed = false;
        bool timedOut = false;

        io.restart();
        timer.expires_after(PLAYERBOT_VERIFICATION_DEADLINE);
        startOperation(
            [&](boost::system::error_code const& operationError, std::size_t)
            {
                error = operationError;
                completed = true;
                timer.cancel();
            });
        timer.async_wait(
            [&](boost::system::error_code const& timerError)
            {
                if (timerError || completed)
                    return;

                timedOut = true;
                boost::system::error_code cancelError;
                socket.cancel(cancelError);
            });
        io.run();

        if (timedOut)
            error = boost::asio::error::timed_out;
        return completed && !error;
    }

    template <typename MutableBuffer>
    bool ReadWithDeadline(tcp::socket& socket, MutableBuffer const& buffer, boost::system::error_code& error)
    {
        return CompleteWithDeadline(
            socket, [&](auto completion) { boost::asio::async_read(socket, buffer, std::move(completion)); }, error);
    }

    template <typename ConstBuffer>
    bool WriteWithDeadline(tcp::socket& socket, ConstBuffer const& buffer, boost::system::error_code& error)
    {
        return CompleteWithDeadline(
            socket, [&](auto completion) { boost::asio::async_write(socket, buffer, std::move(completion)); }, error);
    }

    void SendResponse(tcp::socket& socket, uint64 requestId, Response const& response)
    {
        std::string const payload = SerializeResponse(requestId, response);
        std::optional<std::vector<uint8>> const frame = EncodeFrame(payload);
        if (!frame)
            return;

        boost::system::error_code error;
        WriteWithDeadline(socket, boost::asio::buffer(*frame), error);
    }

    void EmitAudit(RecoveryAuditRecord const& record) { auditSink(SerializeRecoveryAuditRecord(record)); }

    void HandleConnection(tcp::socket& socket)
    {
        std::array<uint8, FRAME_HEADER_BYTES> header{};
        boost::system::error_code error;
        if (!ReadWithDeadline(socket, boost::asio::buffer(header), error))
        {
            if (!stopping.load() && error != boost::asio::error::timed_out)
                SendResponse(socket, 0, Response::Failure(ErrorCode::MalformedFrame, {}));
            return;
        }

        uint32 const length = (static_cast<uint32>(header[0]) << 24) | (static_cast<uint32>(header[1]) << 16) |
                              (static_cast<uint32>(header[2]) << 8) | static_cast<uint32>(header[3]);
        if (length > MAX_FRAME_PAYLOAD_BYTES)
        {
            SendResponse(socket, 0, Response::Failure(ErrorCode::FrameTooLarge, {}));
            return;
        }

        std::string payload(length, '\0');
        if (length)
        {
            if (!ReadWithDeadline(socket, boost::asio::buffer(payload.data(), payload.size()), error))
            {
                if (!stopping.load() && error != boost::asio::error::timed_out)
                    SendResponse(socket, 0, Response::Failure(ErrorCode::MalformedFrame, {}));
                return;
            }
        }

        RequestParseResult parsed = ParseRequestPayload(payload, tokenDigest);
        if (!parsed.request)
        {
            if (std::optional<RecoveryAuditRecord> audit =
                    BuildRejectedRecoveryAudit(payload, parsed.error.code, CurrentTimestampMs()))
                EmitAudit(*audit);
            SendResponse(socket, parsed.responseRequestId, Response::Failure(parsed.error.code, {}));
            return;
        }
        if (stopping.load())
        {
            if (std::optional<RecoveryAuditRecord> audit =
                    BuildRejectedRecoveryAudit(payload, ErrorCode::Shutdown, CurrentTimestampMs()))
                EmitAudit(*audit);
            SendResponse(socket, parsed.request->requestId, Response::Failure(ErrorCode::Shutdown, {}));
            return;
        }

        Response response;
        try
        {
            response = handler(*parsed.request);
        }
        catch (...)
        {
            response = Response::Failure(ErrorCode::InternalError, {});
        }
        if (response.recoveryAudit)
            EmitAudit(*response.recoveryAudit);
        else if (parsed.request->operation == Operation::Recover)
        {
            ErrorCode const auditError = response.ok ? ErrorCode::InternalError : response.error.code;
            if (std::optional<RecoveryAuditRecord> audit =
                    BuildRejectedRecoveryAudit(payload, auditError, CurrentTimestampMs()))
                EmitAudit(*audit);
        }
        SendResponse(socket, parsed.request->requestId, response);
    }

    void Run()
    {
        // SHORTCUT: One active request at a time. Add bounded concurrency when concurrent MCP clients require it.
        while (!stopping.load())
        {
            std::shared_ptr<tcp::socket> socket = std::make_shared<tcp::socket>(io);
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                activeSocket = socket;
            }

            boost::system::error_code error;
            acceptor->accept(*socket, error);
            if (error)
                break;

            HandleConnection(*socket);
            socket->shutdown(tcp::socket::shutdown_both, error);
            socket->close(error);
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (activeSocket == socket)
                    activeSocket.reset();
            }
        }
        running.store(false);
    }

    void AbortNetwork()
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        boost::system::error_code error;
        if (acceptor)
        {
            acceptor->cancel(error);
            acceptor->close(error);
        }
        if (activeSocket)
        {
            activeSocket->shutdown(tcp::socket::shutdown_both, error);
            activeSocket->close(error);
        }
    }
};

tcp::endpoint MakePlayerbotVerificationEndpoint(uint16 port) { return {boost::asio::ip::address_v4::loopback(), port}; }

PlayerbotVerificationServer::PlayerbotVerificationServer() : impl(std::make_unique<Impl>(RequestHandler{}, AuditSink{}))
{
}

PlayerbotVerificationServer::PlayerbotVerificationServer(RequestHandler handler)
    : impl(std::make_unique<Impl>(std::move(handler), AuditSink{}))
{
}

PlayerbotVerificationServer::PlayerbotVerificationServer(RequestHandler handler, AuditSink auditSink)
    : impl(std::make_unique<Impl>(std::move(handler), std::move(auditSink)))
{
}

PlayerbotVerificationServer::~PlayerbotVerificationServer() { Stop(); }

bool PlayerbotVerificationServer::Start(uint32 port)
{
    if (!port || port > std::numeric_limits<uint16>::max())
        return false;

    std::optional<TokenDigest> tokenDigest = VerificationTokenDigestFromEnvironment();
    if (!tokenDigest)
        return false;

    std::lock_guard<std::mutex> lock(impl->stateMutex);
    if (impl->running.load() || impl->worker.joinable())
        return false;

    impl->io.restart();
    impl->acceptor = std::make_unique<tcp::acceptor>(impl->io);
    boost::system::error_code error;
    impl->acceptor->open(tcp::v4(), error);
    if (!error)
        impl->acceptor->bind(MakePlayerbotVerificationEndpoint(static_cast<uint16>(port)), error);
    if (!error)
        impl->acceptor->listen(boost::asio::socket_base::max_listen_connections, error);
    if (error)
    {
        impl->acceptor.reset();
        return false;
    }

    impl->tokenDigest = *tokenDigest;
    impl->stopping.store(false);
    impl->running.store(true);
    SetPlayerbotVerificationAcceptingRequests(true);
    impl->worker = std::thread([serverImpl = impl.get()] { serverImpl->Run(); });
    return true;
}

void PlayerbotVerificationServer::Stop()
{
    SetPlayerbotVerificationAcceptingRequests(false);
    impl->stopping.store(true);
    impl->AbortNetwork();
    if (impl->worker.joinable())
        impl->worker.join();

    std::lock_guard<std::mutex> lock(impl->stateMutex);
    impl->activeSocket.reset();
    impl->acceptor.reset();
    impl->tokenDigest.fill(0);
    impl->running.store(false);
}

bool PlayerbotVerificationServer::IsRunning() const { return impl->running.load(); }
