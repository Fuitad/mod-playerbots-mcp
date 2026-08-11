/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTVERIFICATIONOPERATION_H
#define PLAYERBOTS_PLAYERBOTVERIFICATIONOPERATION_H

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "Bot/MCP/PlayerbotVerificationProtocol.h"
#include "Bot/Telemetry/PlayerbotInspector.h"
#include "Script/WorldThr/PlayerbotOperation.h"

inline constexpr std::chrono::milliseconds PLAYERBOT_VERIFICATION_DEADLINE{5000};

// Extra wait granted when the deadline expires while the world thread is already executing the
// operation. Reporting the real result beats reporting a timeout for work the bot actually did.
inline constexpr std::chrono::milliseconds PLAYERBOT_VERIFICATION_COMPLETION_GRACE{250};

class Player;
using PlayerbotRecoveryPersistence = std::function<void(Player*)>;

/**
 * @brief Response state shared by the socket thread and the world thread.
 *
 * A caller that reaches its deadline abandons the request. An abandoned operation is never
 * executed, so a command can never reach the bot after its caller was told the request timed out.
 * Abandonment and execution claim the same state under one lock, so exactly one of them wins.
 */
class PlayerbotVerificationResult
{
public:
    // World thread. Returns false when the caller already abandoned the request.
    [[nodiscard]] bool TryClaim();
    // Socket thread. Returns false when execution already started, so the caller should wait it out.
    bool Abandon();
    void Complete(PlayerbotVerification::Response completedResponse);
    [[nodiscard]] bool IsCompleted() const;
    [[nodiscard]] std::optional<PlayerbotVerification::Response> Wait(std::chrono::milliseconds timeout);

private:
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    bool abandoned = false;
    bool claimed = false;
    PlayerbotVerification::Response response;
};

/**
 * @brief Runs one verification request on the world thread.
 *
 * The operation stores only immutable request values and GUID low parts. It never stores a live
 * Player, Unit, or PlayerbotAI pointer across threads.
 */
class PlayerbotVerificationOperation : public PlayerbotOperation
{
public:
    PlayerbotVerificationOperation(PlayerbotVerification::Request request,
                                   std::shared_ptr<PlayerbotVerificationResult> result,
                                   PlayerbotRecoveryPersistence recoveryPersistence = {});

    bool Execute() override;

    // Always execute so a queued request completes with a typed error instead of being silently skipped.
    bool IsValid() const override { return true; }

    ObjectGuid GetBotGuid() const override;
    uint32 GetPriority() const override { return 50; }
    std::string GetName() const override { return "Playerbot verification operation"; }

private:
    PlayerbotVerification::Request request;
    std::shared_ptr<PlayerbotVerificationResult> result;
    PlayerbotRecoveryPersistence recoveryPersistence;
};

// Gate opened when the verification server starts and closed during world shutdown.
void SetPlayerbotVerificationAcceptingRequests(bool accepting);
[[nodiscard]] bool IsPlayerbotVerificationAcceptingRequests();

// World thread body. Resolves GUIDs, reads live state, and dispatches normal commands.
PlayerbotVerification::Response ExecuteVerificationOnWorldThread(PlayerbotVerification::Request const& request,
                                                                 PlayerbotRecoveryPersistence recoveryPersistence = {});

// Socket thread entry point. Queues the operation and waits for the bounded world thread result.
PlayerbotVerification::Response DispatchVerificationRequest(PlayerbotVerification::Request const& request,
                                                            std::chrono::milliseconds timeout,
                                                            PlayerbotRecoveryPersistence recoveryPersistence = {});

// Evaluates one typed wait condition against the complete inspection, before display caps are applied.
[[nodiscard]] bool EvaluateVerificationCondition(PlayerbotVerificationInspection const& inspection,
                                                 PlayerbotVerification::Request const& request);

#endif
