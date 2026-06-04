#include "DiscordChat.h"

#include "Chat.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Configuration/Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using namespace std;

static bool ConfigInGameUseGlobalNameIfAvailable = true;

DiscordChatMod::DiscordChatMod() :
    IsEnabled(true),
    ConfigDoAppendServerName(true),
    ConfigServerName("AzerothCore"),
    ConfigInGameChannelName("discord"),
    ConfigNotifyReminderToJoinChannel(DISCORDCHAT_JOIN_REMINDER_DISABLED),
    ConfigShowSystemMessageWhenNotInChannel(true),
    ConfigSpeakerCharacterGUID(0),
    ConfigSpeakerCharacterObjectGuid(ObjectGuid::Empty),
    ConfigDiscordApiBaseUrl("https://discord.com/api/v10"),
    ConfigDiscordWebhookUrl(""),
    ConfigDiscordBotToken(""),
    ConfigDiscordChannelId(""),
    ConfigDiscordWebhookUsername("AzerothCore"),
    ConfigDiscordIncomingEnabled(true),
    ConfigDiscordOutgoingEnabled(true),
    ConfigDiscordPollIntervalInMS(5000),
    ConfigDiscordHttpTimeoutInMS(10000),
    WorkerRunning(false)
{
}

DiscordChatMod::~DiscordChatMod()
{
    StopWorker();
}

void DiscordChatMod::LoadConfigurationFile()
{
    IsEnabled = sConfigMgr->GetOption<bool>("DiscordChat.Enabled", true);
    if (IsEnabled == false)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::LoadConfigurationFile has DiscordChat.Enabled as false, so the module is deactivated");
        return;
    }

    ConfigDoAppendServerName = sConfigMgr->GetOption<bool>("DiscordChat.DoAppendServerName", true);
    ConfigServerName = sConfigMgr->GetOption<string>("DiscordChat.ServerName", "AzerothCore");
    ConfigInGameChannelName = sConfigMgr->GetOption<string>("DiscordChat.InGame.ChannelName", "discord");
    ConfigNotifyReminderToJoinChannel = sConfigMgr->GetOption<uint32>("DiscordChat.NotifyReminderToJoinChannel", DISCORDCHAT_JOIN_REMINDER_DISABLED);
    if (ConfigNotifyReminderToJoinChannel > DISCORDCHAT_JOIN_REMINDER_EVERY_LOGIN)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::LoadConfigurationFile has an invalid DiscordChat.NotifyReminderToJoinChannel value of {}; valid values are 0, 1, or 2. Defaulting to 0 (disabled).", ConfigNotifyReminderToJoinChannel);
        ConfigNotifyReminderToJoinChannel = DISCORDCHAT_JOIN_REMINDER_DISABLED;
    }
    ConfigShowSystemMessageWhenNotInChannel = sConfigMgr->GetOption<bool>("DiscordChat.ShowSystemMessageWhenNotInChannel", true);

    ConfigSpeakerCharacterGUID = sConfigMgr->GetOption<uint32>("DiscordChat.InGame.SpeakerCharacterGUID", 0);
    ConfigSpeakerCharacterObjectGuid = ObjectGuid::Empty;
    if (ConfigSpeakerCharacterGUID != 0)
    {
        QueryResult queryResult = CharacterDatabase.Query("SELECT `guid` FROM `characters` WHERE `guid` = {}", ConfigSpeakerCharacterGUID);
        if (!queryResult || queryResult->GetRowCount() == 0)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::LoadConfigurationFile could not find character with GUID {} for DiscordChat.InGame.SpeakerCharacterGUID. Inbound messages will only be delivered as system messages.", ConfigSpeakerCharacterGUID);
            ConfigSpeakerCharacterGUID = 0;
        }
        else
        {
            ConfigSpeakerCharacterObjectGuid = ObjectGuid::Create<HighGuid::Player>(ConfigSpeakerCharacterGUID);
        }
    }
    ConfigInGameUseGlobalNameIfAvailable = sConfigMgr->GetOption<bool>("DiscordChat.InGame.UseGlobalNameIfAvailable", true);

    ConfigDiscordApiBaseUrl = sConfigMgr->GetOption<string>("DiscordChat.Discord.ApiBaseUrl", "https://discord.com/api/v10");
    ConfigDiscordWebhookUrl = sConfigMgr->GetOption<string>("DiscordChat.Discord.WebhookUrl", "");
    ConfigDiscordBotToken = sConfigMgr->GetOption<string>("DiscordChat.Discord.BotToken", "");
    ConfigDiscordChannelId = sConfigMgr->GetOption<string>("DiscordChat.Discord.ChannelId", "");
    ConfigDiscordWebhookUsername = sConfigMgr->GetOption<string>("DiscordChat.Discord.WebhookUsername", ConfigServerName);
    // GetOption returns "" if the key is present-but-empty in the conf, which
    // overrides the default. Discord rejects webhook posts whose username is
    // empty / contains "discord" / "clyde" / "everyone" with HTTP 400, so apply
    // a usable fallback here.
    if (ConfigDiscordWebhookUsername.empty() == true)
        ConfigDiscordWebhookUsername = ConfigServerName;
    if (ConfigDiscordWebhookUsername.empty() == true)
        ConfigDiscordWebhookUsername = "AzerothCore";
    ConfigDiscordOutgoingEnabled = sConfigMgr->GetOption<bool>("DiscordChat.Discord.OutgoingEnabled", true);
    ConfigDiscordIncomingEnabled = sConfigMgr->GetOption<bool>("DiscordChat.Discord.IncomingEnabled", true);
    ConfigDiscordPollIntervalInMS = sConfigMgr->GetOption<uint32>("DiscordChat.Discord.PollIntervalInMS", 5000);
    ConfigDiscordHttpTimeoutInMS = sConfigMgr->GetOption<uint32>("DiscordChat.Discord.HttpTimeoutInMS", 10000);

    if (ConfigDiscordOutgoingEnabled == true && ConfigDiscordWebhookUrl.empty() == true)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::LoadConfigurationFile has DiscordChat.Discord.OutgoingEnabled as true but no DiscordChat.Discord.WebhookUrl, so outbound is disabled");
        ConfigDiscordOutgoingEnabled = false;
    }
    if (ConfigDiscordIncomingEnabled == true && (ConfigDiscordBotToken.empty() == true || ConfigDiscordChannelId.empty() == true))
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::LoadConfigurationFile has DiscordChat.Discord.IncomingEnabled as true but DiscordChat.Discord.BotToken or DiscordChat.Discord.ChannelId is empty, so inbound is disabled");
        ConfigDiscordIncomingEnabled = false;
    }
}

void DiscordChatMod::StartWorker()
{
    if (IsEnabled == false)
        return;
    if (WorkerRunning.load() == true)
        return;

    WorkerRunning.store(true);
    WorkerThread = thread([this]() { WorkerLoop(); });
    LOG_INFO("module.DiscordChat", "DiscordChatMod worker thread started");
}

void DiscordChatMod::StopWorker()
{
    if (WorkerRunning.load() == false)
        return;

    WorkerRunning.store(false);
    QueueCondVar.notify_all();
    if (WorkerThread.joinable() == true)
        WorkerThread.join();
    LOG_INFO("module.DiscordChat", "DiscordChatMod worker thread stopped");
}

void DiscordChatMod::QueueOutboundMessage(string const& authorName, string const& message)
{
    if (IsEnabled == false || ConfigDiscordOutgoingEnabled == false)
        return;
    if (message.empty() == true)
        return;

    DiscordChatOutboundMessage outbound;
    outbound.AuthorName = authorName;
    outbound.Message = message;

    {
        lock_guard<mutex> lock(QueueMutex);
        OutboundQueue.push_back(outbound);
    }
    QueueCondVar.notify_all();
}

void DiscordChatMod::BroadcastPendingInboundMessages()
{
    if (IsEnabled == false)
        return;

    deque<DiscordChatInboundMessage> drained;
    {
        lock_guard<mutex> lock(QueueMutex);
        drained.swap(InboundQueue);
    }
    for (DiscordChatInboundMessage const& inbound : drained)
        BroadcastInboundMessageToChannel(inbound);
}

void DiscordChatMod::TrackPlayerInChannel(Player* player)
{
    if (IsEnabled == false)
        return;
    if (player == nullptr)
        return;
    if (ConfigInGameChannelName.empty() == true)
        return;

    JoinedPlayerGUIDs.insert(player->GetGUID());
}

void DiscordChatMod::UntrackPlayer(Player* player)
{
    if (player == nullptr)
        return;
    JoinedPlayerGUIDs.erase(player->GetGUID());
    PendingJoinReminders.erase(player->GetGUID());
}

void DiscordChatMod::UntrackChannelMembership(Player* player)
{
    if (player == nullptr)
        return;
    JoinedPlayerGUIDs.erase(player->GetGUID());
}

bool DiscordChatMod::IsBridgeChannelName(string const& name) const
{
    // Channel names are case-insensitive in WoW, so compare accordingly.
    if (ConfigInGameChannelName.empty() == true)
        return false;
    if (name.size() != ConfigInGameChannelName.size())
        return false;
    for (size_t i = 0; i < name.size(); ++i)
    {
        unsigned char a = static_cast<unsigned char>(name[i]);
        unsigned char b = static_cast<unsigned char>(ConfigInGameChannelName[i]);
        if (tolower(a) != tolower(b))
            return false;
    }
    return true;
}

Channel* DiscordChatMod::GetServerChannel(Player* contextPlayer)
{
    if (contextPlayer == nullptr)
        return nullptr;
    ChannelMgr* channelMgr = ChannelMgr::forTeam(contextPlayer->GetTeamId());
    if (channelMgr == nullptr)
        return nullptr;
    return channelMgr->GetChannel(ConfigInGameChannelName, contextPlayer, false);
}

void DiscordChatMod::QueuePendingJoinReminder(Player* player, bool isFirstLogin)
{
    if (IsEnabled == false)
        return;
    if (ConfigNotifyReminderToJoinChannel == DISCORDCHAT_JOIN_REMINDER_DISABLED)
        return;
    // Mode 1 reminds only the very first time a character logs in; skip every
    // subsequent login. Mode 2 reminds on every login.
    if (ConfigNotifyReminderToJoinChannel == DISCORDCHAT_JOIN_REMINDER_FIRST_LOGIN_ONLY && isFirstLogin == false)
        return;
    if (player == nullptr)
        return;
    if (ConfigInGameChannelName.empty() == true)
        return;

    // Wait a few seconds so the client has time to re-join its saved channels
    // before we decide whether the player still needs the reminder.
    PendingJoinReminders[player->GetGUID()] = 5000;
}

void DiscordChatMod::ProcessPendingJoinReminders(uint32 diff)
{
    if (PendingJoinReminders.empty() == true)
        return;

    for (auto it = PendingJoinReminders.begin(); it != PendingJoinReminders.end(); )
    {
        if (it->second > diff)
        {
            it->second -= diff;
            ++it;
            continue;
        }

        ObjectGuid guid = it->first;
        it = PendingJoinReminders.erase(it);

        // Already a member of the bridge channel -> no reminder needed. The
        // membership set is kept current by the server script's packet hook
        // (CMSG_JOIN_CHANNEL / CMSG_LEAVE_CHANNEL), so by the time this delayed
        // check fires it reflects any channels the client re-joined on login.
        if (JoinedPlayerGUIDs.find(guid) != JoinedPlayerGUIDs.end())
            continue;

        Player* player = ObjectAccessor::FindPlayer(guid);
        if (player == nullptr || player->GetSession() == nullptr)
            continue;

        SendJoinReminder(player);
    }
}

void DiscordChatMod::WorkerLoop()
{
    using clock = chrono::steady_clock;
    auto nextPoll = clock::now() + chrono::milliseconds(ConfigDiscordPollIntervalInMS);

    while (WorkerRunning.load() == true)
    {
        // Poll for inbound
        if (ConfigDiscordIncomingEnabled == true && clock::now() >= nextPoll)
        {
            PollDiscordForMessages();
            nextPoll = clock::now() + chrono::milliseconds(ConfigDiscordPollIntervalInMS);
        }

        // Drain outbound
        deque<DiscordChatOutboundMessage> drained;
        {
            unique_lock<mutex> lock(QueueMutex);
            QueueCondVar.wait_for(lock, chrono::milliseconds(500), [this]()
                {
                    return OutboundQueue.empty() == false || WorkerRunning.load() == false;
                });
            drained.swap(OutboundQueue);
        }
        for (DiscordChatOutboundMessage const& outbound : drained)
        {
            if (WorkerRunning.load() == false)
                break;
            SendOutboundToDiscord(outbound);
        }
    }
}

void DiscordChatMod::SendOutboundToDiscord(DiscordChatOutboundMessage const& outbound)
{
    if (ConfigDiscordWebhookUrl.empty() == true)
        return;

    string body = "";
    if (ConfigDoAppendServerName == true)
    {
        body = "{\"username\":\"" + JsonEscape(ConfigDiscordWebhookUsername) +
            "\",\"content\":\"**[" + JsonEscape(ConfigServerName) + "] " +
            JsonEscape(outbound.AuthorName) + "**: " +
            JsonEscape(outbound.Message) + "\"}";
    }
    else
    {
        body = "{\"username\":\"" + JsonEscape(ConfigDiscordWebhookUsername) +
            "\",\"content\":\"**[" + JsonEscape(outbound.AuthorName) + "]**: " +
            JsonEscape(outbound.Message) + "\"}";
    }

    string response;
    if (HttpPostJson(ConfigDiscordWebhookUrl, body, response) == false)
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::SendOutboundToDiscord failed to post message for author {}", outbound.AuthorName);
}

void DiscordChatMod::PollDiscordForMessages()
{
    if (ConfigDiscordBotToken.empty() == true || ConfigDiscordChannelId.empty() == true)
        return;

    string url = ConfigDiscordApiBaseUrl + "/channels/" + ConfigDiscordChannelId + "/messages?limit=25";
    if (LastSeenDiscordMessageId.empty() == false)
        url += "&after=" + LastSeenDiscordMessageId;

    string response;
    if (HttpGetJson(url, ConfigDiscordBotToken, response) == false)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::PollDiscordForMessages failed to fetch messages");
        return;
    }

    vector<DiscordChatInboundMessage> parsed;
    string newestId;
    if (ExtractDiscordMessages(response, parsed, newestId) == false)
        return;

    if (newestId.empty() == false)
        LastSeenDiscordMessageId = newestId;

    if (parsed.empty() == true)
        return;

    // First poll establishes baseline without rebroadcasting history
    static bool firstPoll = true;
    if (firstPoll == true)
    {
        firstPoll = false;
        return;
    }

    lock_guard<mutex> lock(QueueMutex);
    for (DiscordChatInboundMessage const& msg : parsed)
        InboundQueue.push_back(msg);
}

bool DiscordChatMod::HttpPostJson(string const& url, string const& jsonBody, string& responseOut)
{
    try
    {
        // Parse url: expect https://host[:port]/path
        if (url.rfind("https://", 0) != 0)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpPostJson only supports https URLs (got {})", url);
            return false;
        }
        string rest = url.substr(8);
        string::size_type slashPos = rest.find('/');
        string hostPort = slashPos == string::npos ? rest : rest.substr(0, slashPos);
        string target = slashPos == string::npos ? string("/") : rest.substr(slashPos);
        string host = hostPort;
        string port = "443";
        string::size_type colonPos = hostPort.find(':');
        if (colonPos != string::npos)
        {
            host = hostPort.substr(0, colonPos);
            port = hostPort.substr(colonPos + 1);
        }

        asio::io_context ioc;
        ssl::context sslCtx(ssl::context::tlsv12_client);
        sslCtx.set_default_verify_paths();
        sslCtx.set_verify_mode(ssl::verify_none); // Discord cert verification is provided by base store; relax to avoid distro path quirks

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, sslCtx);

        if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 0)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpPostJson failed to set SNI for host {}", host);
            return false;
        }

        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).expires_after(chrono::milliseconds(ConfigDiscordHttpTimeoutInMS));
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{ http::verb::post, target, 11 };
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "AzerothCore-mod-discord-chat");
        req.set(http::field::content_type, "application/json");
        req.body() = jsonBody;
        req.prepare_payload();

        beast::get_lowest_layer(stream).expires_after(chrono::milliseconds(ConfigDiscordHttpTimeoutInMS));
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        responseOut = res.body();
        unsigned status = res.result_int();

        beast::error_code ec;
        stream.shutdown(ec); // Ignore truncation errors per Beast docs

        if (status < 200 || status >= 300)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpPostJson got status {} from {} -- body: {} -- request: {}", status, url, responseOut, jsonBody);
            return false;
        }
        return true;
    }
    catch (exception const& ex)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpPostJson exception: {}", ex.what());
        return false;
    }
}

bool DiscordChatMod::HttpGetJson(string const& url, string const& bearerToken, string& responseOut)
{
    try
    {
        if (url.rfind("https://", 0) != 0)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpGetJson only supports https URLs (got {})", url);
            return false;
        }
        string rest = url.substr(8);
        string::size_type slashPos = rest.find('/');
        string hostPort = slashPos == string::npos ? rest : rest.substr(0, slashPos);
        string target = slashPos == string::npos ? string("/") : rest.substr(slashPos);
        string host = hostPort;
        string port = "443";
        string::size_type colonPos = hostPort.find(':');
        if (colonPos != string::npos)
        {
            host = hostPort.substr(0, colonPos);
            port = hostPort.substr(colonPos + 1);
        }

        asio::io_context ioc;
        ssl::context sslCtx(ssl::context::tlsv12_client);
        sslCtx.set_default_verify_paths();
        sslCtx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, sslCtx);

        if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 0)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpGetJson failed to set SNI for host {}", host);
            return false;
        }

        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).expires_after(chrono::milliseconds(ConfigDiscordHttpTimeoutInMS));
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{ http::verb::get, target, 11 };
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "AzerothCore-mod-discord-chat");
        req.set(http::field::authorization, string("Bot ") + bearerToken);

        beast::get_lowest_layer(stream).expires_after(chrono::milliseconds(ConfigDiscordHttpTimeoutInMS));
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        responseOut = res.body();
        unsigned status = res.result_int();

        beast::error_code ec;
        stream.shutdown(ec);

        if (status < 200 || status >= 300)
        {
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpGetJson got status {} from {} -- body: {}", status, url, responseOut);
            return false;
        }
        return true;
    }
    catch (exception const& ex)
    {
        LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpGetJson exception: {}", ex.what());
        return false;
    }
}

string DiscordChatMod::JsonEscape(string const& value)
{
    string out;
    out.reserve(value.size() + 4);
    for (char c : value)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
                out += c;
            break;
        }
    }
    return out;
}

bool DiscordChatMod::IsWowSafeDisplayName(string const& value)
{
    if (value.empty() == true)
        return false;

    // Discord returns names as UTF-8. Decode and accept only code points the
    // 3.3.5a client renders reliably: printable ASCII (U+0020-U+007E) and the
    // printable Latin-1 supplement (U+00A0-U+00FF, accented Western European
    // letters). Anything higher (CJK, emoji), any control character, or a
    // malformed byte makes the whole name unusable, so the caller falls back
    // to the always-ASCII username.
    for (size_t i = 0; i < value.size(); )
    {
        unsigned char b = static_cast<unsigned char>(value[i]);
        uint32 cp;
        if (b < 0x80)
        {
            cp = b;
            i += 1;
        }
        else if ((b & 0xE0) == 0xC0)
        {
            // 2-byte sequence -> U+0080..U+07FF; only the low half is Latin-1.
            if (i + 1 >= value.size())
                return false;
            unsigned char c1 = static_cast<unsigned char>(value[i + 1]);
            if ((c1 & 0xC0) != 0x80)
                return false;
            cp = ((b & 0x1F) << 6) | (c1 & 0x3F);
            i += 2;
        }
        else
        {
            // 3- or 4-byte sequence (> U+00FF) or a stray continuation byte.
            return false;
        }

        bool isPrintableAscii = cp >= 0x20 && cp <= 0x7E;
        bool isPrintableLatin1 = cp >= 0xA0 && cp <= 0xFF;
        if (isPrintableAscii == false && isPrintableLatin1 == false)
            return false;
    }
    return true;
}

// Minimal extractor for Discord's GET /channels/{id}/messages response.
// The payload is a JSON array of message objects with at least:
//   { "id": "...", "content": "...", "author": { "username": "...", "bot": true|false } }
// This avoids pulling in a JSON library while staying robust enough for the
// fields we care about. Returns the newest id seen so callers can paginate.
bool DiscordChatMod::ExtractDiscordMessages(string const& json, vector<DiscordChatInboundMessage>& outMessages, string& outNewestId)
{
    outMessages.clear();
    outNewestId.clear();

    auto unescape = [](string const& s) -> string
        {
            string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size())
                {
                    char n = s[i + 1];
                    switch (n)
                    {
                    case '"':  out += '"';  i++; break;
                    case '\\': out += '\\'; i++; break;
                    case '/':  out += '/';  i++; break;
                    case 'b':  out += '\b'; i++; break;
                    case 'f':  out += '\f'; i++; break;
                    case 'n':  out += '\n'; i++; break;
                    case 'r':  out += '\r'; i++; break;
                    case 't':  out += '\t'; i++; break;
                    case 'u':
                        if (i + 5 < s.size())
                        {
                            // Drop unicode escapes to ASCII-printable space marker
                            out += '?';
                            i += 5;
                        }
                        break;
                    default: out += n; i++; break;
                    }
                }
                else
                    out += s[i];
            }
            return out;
        };

    auto findStringField = [&](string const& obj, string const& key, string& outValue) -> bool
        {
            string needle = "\"" + key + "\"";
            size_t pos = obj.find(needle);
            if (pos == string::npos)
                return false;
            pos = obj.find(':', pos + needle.size());
            if (pos == string::npos)
                return false;
            while (pos + 1 < obj.size() && (obj[pos + 1] == ' ' || obj[pos + 1] == '\t'))
                pos++;
            if (pos + 1 >= obj.size() || obj[pos + 1] != '"')
                return false;
            size_t start = pos + 2;
            size_t end = start;
            while (end < obj.size())
            {
                if (obj[end] == '\\' && end + 1 < obj.size())
                {
                    end += 2;
                    continue;
                }
                if (obj[end] == '"')
                    break;
                end++;
            }
            if (end >= obj.size())
                return false;
            outValue = unescape(obj.substr(start, end - start));
            return true;
        };

    auto findBoolField = [&](string const& obj, string const& key, bool& outValue) -> bool
        {
            string needle = "\"" + key + "\"";
            size_t pos = obj.find(needle);
            if (pos == string::npos)
                return false;
            pos = obj.find(':', pos + needle.size());
            if (pos == string::npos)
                return false;
            while (pos + 1 < obj.size() && (obj[pos + 1] == ' ' || obj[pos + 1] == '\t'))
                pos++;
            if (obj.compare(pos + 1, 4, "true") == 0) { outValue = true; return true; }
            if (obj.compare(pos + 1, 5, "false") == 0) { outValue = false; return true; }
            return false;
        };

    // Like findStringField, but only matches a key at the immediate top level of
    // the object (brace/bracket depth 1). A message's own "id"/"content" must not
    // be confused with the same key nested inside "attachments" or "author": an
    // attachment's "id" is a different (and smaller) snowflake than the message
    // id, and grabbing it would stall the poll cursor and refetch the message
    // every interval.
    auto findTopLevelStringField = [&](string const& obj, string const& key, string& outValue) -> bool
        {
            string needle = "\"" + key + "\"";
            int depth = 0;
            bool inString = false;
            for (size_t p = 0; p < obj.size(); ++p)
            {
                char c = obj[p];
                if (inString == true)
                {
                    if (c == '\\' && p + 1 < obj.size()) { p++; continue; }
                    if (c == '"') inString = false;
                    continue;
                }
                if (c == '"')
                {
                    if (depth == 1 && obj.compare(p, needle.size(), needle) == 0)
                    {
                        size_t colon = p + needle.size();
                        while (colon < obj.size() && (obj[colon] == ' ' || obj[colon] == '\t'))
                            colon++;
                        if (colon < obj.size() && obj[colon] == ':')
                            return findStringField(obj.substr(p), key, outValue);
                    }
                    inString = true;
                    continue;
                }
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') depth--;
            }
            return false;
        };

    // Walk top-level array, slicing each balanced { ... } member
    size_t i = json.find('[');
    if (i == string::npos)
        return false;
    i++;
    while (i < json.size())
    {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ','))
            i++;
        if (i >= json.size() || json[i] == ']')
            break;
        if (json[i] != '{')
            break;

        size_t start = i;
        int depth = 0;
        bool inString = false;
        size_t j = i;
        for (; j < json.size(); ++j)
        {
            char c = json[j];
            if (inString == true)
            {
                if (c == '\\' && j + 1 < json.size()) { j++; continue; }
                if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') depth++;
            else if (c == '}')
            {
                depth--;
                if (depth == 0)
                {
                    j++;
                    break;
                }
            }
        }
        if (depth != 0)
            break;

        string obj = json.substr(start, j - start);
        i = j;

        // Extract id, content, author block. These must come from the message's
        // own top-level fields, not from nested objects such as "attachments"
        // (whose entries also carry an "id"), so use the depth-aware finder.
        string id, content;
        if (findTopLevelStringField(obj, "id", id) == false)
            continue;
        findTopLevelStringField(obj, "content", content);

        // Slice author sub-object
        string authorName;
        bool isBot = false;
        size_t authorPos = obj.find("\"author\"");
        if (authorPos != string::npos)
        {
            size_t braceStart = obj.find('{', authorPos);
            if (braceStart != string::npos)
            {
                int aDepth = 0;
                bool aInString = false;
                size_t k = braceStart;
                for (; k < obj.size(); ++k)
                {
                    char c = obj[k];
                    if (aInString == true)
                    {
                        if (c == '\\' && k + 1 < obj.size()) { k++; continue; }
                        if (c == '"') aInString = false;
                        continue;
                    }
                    if (c == '"') { aInString = true; continue; }
                    if (c == '{') aDepth++;
                    else if (c == '}')
                    {
                        aDepth--;
                        if (aDepth == 0)
                        {
                            k++;
                            break;
                        }
                    }
                }
                string authorObj = obj.substr(braceStart, k - braceStart);

                // Fallback when global_name is absent, null, or contains characters the 3.3.5a client cannot render (CJK, emoji, etc.; accented Latin-1 names are kept).
                if (ConfigInGameUseGlobalNameIfAvailable == true)
                {
                    string globalName;
                    if (findStringField(authorObj, "global_name", globalName) == true
                        && IsWowSafeDisplayName(globalName) == true)
                        authorName = globalName;
                    else
                        findStringField(authorObj, "username", authorName);
                }
                else
                    findStringField(authorObj, "username", authorName);
                findBoolField(authorObj, "bot", isBot);
            }
        }

        // Track newest seen id regardless of source so polling advances correctly
        if (id > outNewestId)
            outNewestId = id;

        // Skip bot messages (avoids echoing back our own webhook posts)
        if (isBot == true)
            continue;

        // Surface any media (images, videos, files, stickers) the message
        // carried. Media-only Discord posts have empty `content`, so without
        // this they would be dropped just below and players would never see
        // that anything was posted.
        string mediaMarker = SummarizeMessageMedia(obj);
        if (mediaMarker.empty() == false)
        {
            if (content.empty() == true)
                content = mediaMarker;
            else
                content += " " + mediaMarker;
        }

        if (content.empty() == true)
            continue;

        DiscordChatInboundMessage msg;
        msg.AuthorName = authorName.empty() == true ? "discord" : authorName;
        msg.Message = content;
        outMessages.push_back(msg);
    }

    // Discord returns newest-first; reverse so we replay in chronological order
    reverse(outMessages.begin(), outMessages.end());
    return true;
}

string DiscordChatMod::SummarizeMessageMedia(string const& messageObjectJson)
{
    // Reads non-text content out of a single Discord message object so it can be
    // represented as plain text. The 3.3.5a client cannot display images,
    // videos, files or stickers, so we emit a short placeholder like
    // "[image: cat.png]" instead. Filenames/sticker names that the client can't
    // render are reduced to just the kind (e.g. "[image]").

    // Reads a string field directly nested in `obj` (not recursive). Good enough
    // for the flat attachment/sticker objects we inspect here.
    auto findString = [](string const& obj, string const& key, string& outValue) -> bool
        {
            string needle = "\"" + key + "\"";
            size_t pos = obj.find(needle);
            if (pos == string::npos)
                return false;
            pos = obj.find(':', pos + needle.size());
            if (pos == string::npos)
                return false;
            while (pos + 1 < obj.size() && (obj[pos + 1] == ' ' || obj[pos + 1] == '\t'))
                pos++;
            if (pos + 1 >= obj.size() || obj[pos + 1] != '"')
                return false;
            size_t start = pos + 2;
            size_t end = start;
            while (end < obj.size())
            {
                if (obj[end] == '\\' && end + 1 < obj.size())
                {
                    end += 2;
                    continue;
                }
                if (obj[end] == '"')
                    break;
                end++;
            }
            if (end >= obj.size())
                return false;
            outValue = obj.substr(start, end - start);
            return true;
        };

    // Slices each balanced { ... } object out of the JSON array stored at `key`.
    auto sliceArrayObjects = [](string const& obj, string const& key, vector<string>& out)
        {
            string needle = "\"" + key + "\"";
            size_t pos = obj.find(needle);
            if (pos == string::npos)
                return;
            size_t bracket = obj.find('[', pos);
            if (bracket == string::npos)
                return;
            size_t i = bracket + 1;
            while (i < obj.size())
            {
                while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t' || obj[i] == '\n' || obj[i] == '\r' || obj[i] == ','))
                    i++;
                if (i >= obj.size() || obj[i] == ']')
                    break;
                if (obj[i] != '{')
                    break;
                size_t start = i;
                int depth = 0;
                bool inString = false;
                size_t j = i;
                for (; j < obj.size(); ++j)
                {
                    char c = obj[j];
                    if (inString == true)
                    {
                        if (c == '\\' && j + 1 < obj.size()) { j++; continue; }
                        if (c == '"') inString = false;
                        continue;
                    }
                    if (c == '"') { inString = true; continue; }
                    if (c == '{') depth++;
                    else if (c == '}')
                    {
                        depth--;
                        if (depth == 0)
                        {
                            j++;
                            break;
                        }
                    }
                }
                if (depth != 0)
                    break;
                out.push_back(obj.substr(start, j - start));
                i = j;
            }
        };

    vector<string> markers;

    // File uploads: pasted/dragged images, videos, audio and other files.
    vector<string> attachments;
    sliceArrayObjects(messageObjectJson, "attachments", attachments);
    for (string const& att : attachments)
    {
        string contentType;
        findString(att, "content_type", contentType);

        string kind = "file";
        if (contentType.rfind("image/", 0) == 0)
            kind = "image";
        else if (contentType.rfind("video/", 0) == 0)
            kind = "video";
        else if (contentType.rfind("audio/", 0) == 0)
            kind = "audio";

        string filename;
        findString(att, "filename", filename);
        if (IsWowSafeDisplayName(filename) == true)
            markers.push_back("[" + kind + ": " + filename + "]");
        else
            markers.push_back("[" + kind + "]");
    }

    // Stickers.
    vector<string> stickers;
    sliceArrayObjects(messageObjectJson, "sticker_items", stickers);
    for (string const& st : stickers)
    {
        string name;
        findString(st, "name", name);
        if (IsWowSafeDisplayName(name) == true)
            markers.push_back("[sticker: " + name + "]");
        else
            markers.push_back("[sticker]");
    }

    string out;
    for (string const& m : markers)
    {
        if (out.empty() == false)
            out += " ";
        out += m;
    }
    return out;
}

void DiscordChatMod::BroadcastInboundMessageToChannel(DiscordChatInboundMessage const& inbound)
{
    string display = inbound.AuthorName + ": " + inbound.Message;
    string systemDisplay = "[" + ConfigServerName + " <- Discord] " + inbound.AuthorName + ": " + inbound.Message;

    // Channel-scoped delivery (CHAT_MSG_CHANNEL) requires the speaker to be a
    // real character GUID. If no speaker is configured we fall back to system
    // messages for every player so messages still get through.
    bool channelDeliveryAvailable = ConfigSpeakerCharacterObjectGuid != ObjectGuid::Empty
        && ConfigInGameChannelName.empty() == false;

    ChatHandler(nullptr).DoForAllValidSessions([&](Player* player)
        {
            if (player == nullptr || player->GetSession() == nullptr)
                return;

            bool isInBridgeChannel = JoinedPlayerGUIDs.find(player->GetGUID()) != JoinedPlayerGUIDs.end();

            WorldPacket data;
            if (isInBridgeChannel == true && channelDeliveryAvailable == true)
            {
                // Members of the bridge channel see the message as a normal channel
                // chat line attributed to the configured speaker character. The
                // client resolves the senderGUID via SMSG_NAME_QUERY so the speaker
                // appears with their real character name.
                ChatHandler::BuildChatPacket(
                    data,
                    CHAT_MSG_CHANNEL,
                    LANG_UNIVERSAL,
                    ConfigSpeakerCharacterObjectGuid,
                    ConfigSpeakerCharacterObjectGuid,
                    display,
                    0,
                    "",
                    "",
                    0,
                    false,
                    ConfigInGameChannelName);
            }
            else
            {
                // Everyone else gets a system message so the bridge still reaches
                // players who haven't joined the channel. Players who are not on
                // the bridge channel only see this when the system-message
                // fallback is enabled; players who are on the channel but lack
                // channel-scoped delivery (no speaker configured) always get it.
                if (isInBridgeChannel == false && ConfigShowSystemMessageWhenNotInChannel == false)
                    return;
                ChatHandler::BuildChatPacket(
                    data,
                    CHAT_MSG_SYSTEM,
                    LANG_UNIVERSAL,
                    ObjectGuid::Empty,
                    ObjectGuid::Empty,
                    systemDisplay,
                    0);
            }
            player->GetSession()->SendPacket(&data);
        });
}

void DiscordChatMod::SendJoinReminder(Player* player)
{
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    if (ConfigInGameChannelName.empty() == true)
        return;

    string reminder = "Join the '" + ConfigInGameChannelName +
        "' channel that bridges chat with Discord. Type /join " + ConfigInGameChannelName +
        " to join it.";
    ChatHandler(player->GetSession()).SendSysMessage(reminder);
}
