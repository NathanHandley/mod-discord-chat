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

// Modes for DiscordChat.NotifyReminderToJoinChannel.
enum DiscordChatJoinReminderMode
{
    DISCORDCHAT_JOIN_REMINDER_DISABLED         = 0, // never remind
    DISCORDCHAT_JOIN_REMINDER_FIRST_LOGIN_ONLY = 1, // remind only on a character's first login
    DISCORDCHAT_JOIN_REMINDER_EVERY_LOGIN      = 2  // remind on every login
};

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
    uint32 ConfigNotifyReminderToJoinChannel;
    bool ConfigShowSystemMessageWhenNotInChannel;
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
    bool   ConfigAnnounceBridgeStatusToPlayers;
    uint32 ConfigBridgeDownThreshold;
    string ConfigBridgeDownMessage;
    string ConfigBridgeUpMessage;

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

    // Bridge status notices (Discord reachable / unreachable). The worker thread
    // queues a one-shot notice on each up<->down transition; this drains and
    // broadcasts them from the world thread, same as inbound messages.
    void BroadcastPendingBridgeStatusNotices();

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
    void QueuePendingJoinReminder(Player* player, bool isFirstLogin);
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

    // Bridge health tracking, driven off the poll heartbeat (worker thread).
    // Records a poll outcome and, on a down<->up transition, queues a one-shot
    // player notice. Only the worker thread touches the counters/flag.
    void RecordPollOutcome(bool success);
    // Queue a status notice for delivery to all online players (worker thread).
    void QueueBridgeStatusNotice(string const& message);
    // Broadcast one status notice to every online player as a system message
    // (world thread).
    void BroadcastBridgeStatusNoticeToPlayers(string const& message);

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
    // Status notices ("bridge down" / "bridge back up") awaiting delivery to
    // players on the world thread. Guarded by QueueMutex.
    deque<string> BridgeStatusNoticeQueue;

    // Bridge health, owned exclusively by the worker thread. ReportedDown gates
    // the notice so the "down" message is sent once per outage, not every poll.
    uint32 ConsecutivePollFailures = 0;
    bool BridgeReportedDown = false;

    // Tracks the most recently seen Discord message id for incremental polling
    string LastSeenDiscordMessageId;

    // GUIDs of players who logged in and are pending a deferred join-channel
    // reminder, mapped to the milliseconds remaining before the check fires.
    unordered_map<ObjectGuid, uint32> PendingJoinReminders;
};

#define DiscordChat DiscordChatMod::instance()

#endif //DISCORDCHAT_H
