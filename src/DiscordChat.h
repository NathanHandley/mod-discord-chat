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
#include <unordered_map>
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
    bool ConfigDoAppendServerName;
    string ConfigServerName;
    string ConfigInGameChannelName;
    bool ConfigNotifyReminderToJoinChannel;
    uint32 ConfigSpeakerCharacterGUID;
    ObjectGuid ConfigSpeakerCharacterObjectGuid;

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

    // Channel membership tracking. AzerothCore has no OnJoinChannel /
    // OnLeaveChannel script hook in 3.3.5a, so we maintain our own set of
    // bridge-channel members. A player is tracked the first time they chat in
    // the bridge channel (via OnPlayerCanUseChat) and untracked on logout.
    void TrackPlayerInChannel(Player* player);
    void UntrackPlayer(Player* player);
    // Remove only the bridge-channel membership flag (used when a player leaves
    // the channel but stays online); does not touch pending reminders.
    void UntrackChannelMembership(Player* player);
    // Case-insensitive match of a channel name against the configured bridge
    // channel (DiscordChat.InGame.ChannelName).
    bool IsBridgeChannelName(string const& name) const;
    Channel* GetServerChannel(Player* contextPlayer);

    // Reminder to /join the bridge channel. The login hook fires before the
    // client has re-joined its saved channels, so we can't tell at login
    // whether the player is already a member. Instead we queue a short delay
    // (see ProcessPendingJoinReminders, ticked from the world update) and only
    // remind players who are still not on the channel once it elapses.
    void QueuePendingJoinReminder(Player* player);
    void ProcessPendingJoinReminders(uint32 diff);

private:
    // Worker thread function
    void WorkerLoop();

    // HTTP helpers (synchronous, called from worker thread)
    bool HttpPostJson(string const& url, string const& jsonBody, string& responseOut);
    bool HttpGetJson(string const& url, string const& bearerToken, string& responseOut);

    // Discord API operations (worker thread)
    void SendOutboundToDiscord(DiscordChatOutboundMessage const& outbound);
    void PollDiscordForMessages();

    static string JsonEscape(string const& value);
    static bool IsWowSafeDisplayName(string const& value);
    static bool ExtractDiscordMessages(string const& json, vector<DiscordChatInboundMessage>& outMessages, string& outNewestId);
    // Builds a short, ASCII-safe placeholder (e.g. "[image: cat.png]") for any
    // non-text content (file attachments, stickers) carried by a single Discord
    // message object. Returns "" when the message has no such media.
    static string SummarizeMessageMedia(string const& messageObjectJson);
    void BroadcastInboundMessageToChannel(DiscordChatInboundMessage const& inbound);

    // Send the "please /join the bridge channel" system message to one player
    void SendJoinReminder(Player* player);

    // Threading
    thread WorkerThread;
    atomic<bool> WorkerRunning;
    mutex QueueMutex;
    condition_variable QueueCondVar;
    deque<DiscordChatOutboundMessage> OutboundQueue;
    deque<DiscordChatInboundMessage> InboundQueue;

    // Tracks the most recently seen Discord message id for incremental polling
    string LastSeenDiscordMessageId;

    // GUIDs of players who logged in and are pending a deferred join-channel
    // reminder, mapped to the milliseconds remaining before the check fires.
    unordered_map<ObjectGuid, uint32> PendingJoinReminders;
};

#define DiscordChat DiscordChatMod::instance()

#endif //DISCORDCHAT_H
