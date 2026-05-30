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
#include <chrono>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using namespace std;

DiscordChatMod::DiscordChatMod() :
    IsEnabled(true),
    ConfigServerName("AzerothCore"),
    ConfigInGameChannelName("discord"),
    ConfigInGameChannelPassword(""),
    ConfigAutoJoinChannelOnLogin(true),
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

    ConfigServerName = sConfigMgr->GetOption<string>("DiscordChat.ServerName", "AzerothCore");
    ConfigInGameChannelName = sConfigMgr->GetOption<string>("DiscordChat.InGame.ChannelName", "discord");
    ConfigInGameChannelPassword = sConfigMgr->GetOption<string>("DiscordChat.InGame.ChannelPassword", "");
    ConfigAutoJoinChannelOnLogin = sConfigMgr->GetOption<bool>("DiscordChat.InGame.AutoJoinOnLogin", true);

    ConfigDiscordApiBaseUrl = sConfigMgr->GetOption<string>("DiscordChat.Discord.ApiBaseUrl", "https://discord.com/api/v10");
    ConfigDiscordWebhookUrl = sConfigMgr->GetOption<string>("DiscordChat.Discord.WebhookUrl", "");
    ConfigDiscordBotToken = sConfigMgr->GetOption<string>("DiscordChat.Discord.BotToken", "");
    ConfigDiscordChannelId = sConfigMgr->GetOption<string>("DiscordChat.Discord.ChannelId", "");
    ConfigDiscordWebhookUsername = sConfigMgr->GetOption<string>("DiscordChat.Discord.WebhookUsername", ConfigServerName);
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

void DiscordChatMod::AutoJoinPlayerToChannel(Player* player)
{
    if (IsEnabled == false)
        return;
    if (player == nullptr)
        return;
    if (ConfigInGameChannelName.empty() == true)
        return;

    ChannelMgr* channelMgr = ChannelMgr::forTeam(player->GetTeamId());
    if (channelMgr == nullptr)
        return;

    Channel* channel = channelMgr->GetJoinChannel(ConfigInGameChannelName, 0);
    if (channel == nullptr)
        return;

    // Force a fresh join. LeaveChannel is a no-op if the player isn't on the
    // channel, so an unconditional leave-then-join refreshes the client's
    // channel-tab registration (Channel::JoinChannel short-circuits when the
    // player is already a member and never re-sends SMSG_CHANNEL_NOTIFY).
    channel->LeaveChannel(player, true);
    channel->JoinChannel(player, ConfigInGameChannelPassword);
    JoinedPlayerGUIDs.insert(player->GetGUID());
}

void DiscordChatMod::LeavePlayerFromChannel(Player* player)
{
    if (player == nullptr)
        return;
    JoinedPlayerGUIDs.erase(player->GetGUID());
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

void DiscordChatMod::WorkerLoop()
{
    using clock = chrono::steady_clock;
    auto nextPoll = clock::now() + chrono::milliseconds(ConfigDiscordPollIntervalInMS);

    while (WorkerRunning.load() == true)
    {
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

        // Poll for inbound
        if (ConfigDiscordIncomingEnabled == true && clock::now() >= nextPoll)
        {
            PollDiscordForMessages();
            nextPoll = clock::now() + chrono::milliseconds(ConfigDiscordPollIntervalInMS);
        }
    }
}

void DiscordChatMod::SendOutboundToDiscord(DiscordChatOutboundMessage const& outbound)
{
    if (ConfigDiscordWebhookUrl.empty() == true)
        return;

    string body = "{\"username\":\"" + JsonEscape(ConfigDiscordWebhookUsername) +
                  "\",\"content\":\"**[" + JsonEscape(ConfigServerName) + "] " +
                  JsonEscape(outbound.AuthorName) + "**: " +
                  JsonEscape(outbound.Message) + "\"}";

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

        http::request<http::string_body> req{http::verb::post, target, 11};
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
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpPostJson got status {} from {}", status, url);
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

        http::request<http::string_body> req{http::verb::get, target, 11};
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
            LOG_ERROR("module.DiscordChat", "DiscordChatMod::HttpGetJson got status {} from {}", status, url);
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

        // Extract id, content, author block
        string id, content;
        if (findStringField(obj, "id", id) == false)
            continue;
        findStringField(obj, "content", content);

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

void DiscordChatMod::BroadcastInboundMessageToChannel(DiscordChatInboundMessage const& inbound)
{
    // Compose a single visible line. We deliberately do NOT route Discord
    // messages through CHAT_MSG_CHANNEL: that packet relies on the client
    // having allocated a channel slot and on a real player senderGUID, neither
    // of which holds for messages originating outside the game. CHAT_MSG_SYSTEM
    // renders unconditionally in every player's default chat tab and is
    // visually styled as a server announcement, which fits the bridge usage.
    string display = "[" + ConfigServerName + " <- Discord] " + inbound.AuthorName + ": " + inbound.Message;

    for (auto it = JoinedPlayerGUIDs.begin(); it != JoinedPlayerGUIDs.end(); )
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(*it);
        if (player == nullptr || player->GetSession() == nullptr)
        {
            it = JoinedPlayerGUIDs.erase(it);
            continue;
        }

        WorldPacket data;
        ChatHandler::BuildChatPacket(
            data,
            CHAT_MSG_SYSTEM,
            LANG_UNIVERSAL,
            ObjectGuid::Empty,
            ObjectGuid::Empty,
            display,
            0);
        player->GetSession()->SendPacket(&data);
        ++it;
    }
}
