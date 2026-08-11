/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTVERIFICATIONSERVER_H
#define PLAYERBOTS_PLAYERBOTVERIFICATIONSERVER_H

#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <memory>

#include "PlayerbotVerificationProtocol.h"

boost::asio::ip::tcp::endpoint MakePlayerbotVerificationEndpoint(uint16 port);

class PlayerbotVerificationServer
{
public:
    using RequestHandler = std::function<PlayerbotVerification::Response(PlayerbotVerification::Request const&)>;
    using AuditSink = std::function<void(std::string const&)>;

    PlayerbotVerificationServer();
    explicit PlayerbotVerificationServer(RequestHandler handler);
    PlayerbotVerificationServer(RequestHandler handler, AuditSink auditSink);
    ~PlayerbotVerificationServer();

    PlayerbotVerificationServer(PlayerbotVerificationServer const&) = delete;
    PlayerbotVerificationServer& operator=(PlayerbotVerificationServer const&) = delete;

    bool Start(uint32 port);
    void Stop();
    [[nodiscard]] bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
