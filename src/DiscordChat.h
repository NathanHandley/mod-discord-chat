#ifndef DISCORDCHAT_H
#define DISCORDCHAT_H

#include "Common.h"
#include "ObjectGuid.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

using namespace std;

class Player;
class Channel;

class DiscordChatOutboundMessage
{
public:
    string AuthorName;
    string Message;
};

class DiscordChatInboundMessage
{
public:
    string AuthorName;
    string Message;
};

class DiscordChatMod
{
private:
    DiscordChatMod();

public:
    bool IsEnabled;

    // Configs (from server file)
    string ConfigServerName;
    string ConfigInGameChannelName;
    string ConfigInGameChannelPassword;
    bool   ConfigAutoJoinChannelOnLogin;

    string ConfigDiscordApiBaseUrl;
    string ConfigDiscordWebhookUrl;
    string ConfigDiscordBotToken;
    string ConfigDiscordChannelId;
    string ConfigDiscordWebhookUsername;
    bool   ConfigDiscordIncomingEnabled;
    bool   ConfigDiscordOutgoingEnabled;
    uint32 ConfigDiscordPollIntervalInMS;
    uint32 ConfigDiscordHttpTimeoutInMS;

    // Runtime state
    unordered_set<ObjectGuid> JoinedPlayerGUIDs;

    static DiscordChatMod* instance()
    {
        static DiscordChatMod instance;
        return &instance;
    }
    ~DiscordChatMod();

    void LoadConfigurationFile();

    // Worker lifecycle
    void StartWorker();
    void StopWorker();

    // Outbound: in-game chat -> Discord
    void QueueOutboundMessage(string const& authorName, string const& message);

    // Inbound: Discord -> in-game (called from world thread via OnUpdate)
    void BroadcastPendingInboundMessages();

    // Channel join helpers
    void AutoJoinPlayerToChannel(Player* player);
    void LeavePlayerFromChannel(Player* player);
    Channel* GetServerChannel(Player* contextPlayer);

private:
    // Worker thread function
    void WorkerLoop();

    // HTTP helpers (synchronous, called from worker thread)
    bool HttpPostJson(string const& url, string const& jsonBody, string& responseOut);
    bool HttpGetJson(string const& url, string const& bearerToken, string& responseOut);

    // Discord API operations (worker thread)
    void SendOutboundToDiscord(DiscordChatOutboundMessage const& outbound);
    void PollDiscordForMessages();

    // JSON helpers (just enough for Discord message payloads)
    static string JsonEscape(string const& value);
    static bool ExtractDiscordMessages(string const& json, vector<DiscordChatInboundMessage>& outMessages, string& outNewestId);

    // Build a packet broadcast for a single inbound message to all members
    void BroadcastInboundMessageToChannel(DiscordChatInboundMessage const& inbound);

    // Threading
    thread WorkerThread;
    atomic<bool> WorkerRunning;
    mutex QueueMutex;
    condition_variable QueueCondVar;
    deque<DiscordChatOutboundMessage> OutboundQueue;
    deque<DiscordChatInboundMessage> InboundQueue;

    // Tracks the most recently seen Discord message id for incremental polling
    string LastSeenDiscordMessageId;
};

#define DiscordChat DiscordChatMod::instance()

#endif //DISCORDCHAT_H
