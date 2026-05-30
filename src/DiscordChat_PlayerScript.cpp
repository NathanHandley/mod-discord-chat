#include "Channel.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "DiscordChat.h"

#include <chrono>

using namespace std;
using namespace std::chrono_literals;

class DiscordChat_PlayerScript : public PlayerScript
{
public:
    DiscordChat_PlayerScript() : PlayerScript("DiscordChat_PlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (DiscordChat->IsEnabled == false)
            return;
        if (DiscordChat->ConfigAutoJoinChannelOnLogin == false)
            return;
        if (player == nullptr)
            return;

        // Defer the join until the client has had a chance to set up its chat
        // UI. If we send SMSG_CHANNEL_NOTIFY too early in login, the client
        // never allocates a channel slot, /N switching is broken, and any
        // CHAT_MSG_CHANNEL traffic addressed to that channel is dropped.
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            Player* delayedPlayer = ObjectAccessor::FindConnectedPlayer(guid);
            if (delayedPlayer == nullptr)
                return;
            DiscordChat->AutoJoinPlayerToChannel(delayedPlayer);
        }, 3s);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (DiscordChat->IsEnabled == false)
            return;
        DiscordChat->LeavePlayerFromChannel(player);
    }

    // Capture chat the player tries to send into the bridge channel and forward
    // it to Discord. Returning true lets the message also broadcast to in-game
    // channel members normally.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Channel* channel) override
    {
        if (DiscordChat->IsEnabled == false)
            return true;
        if (channel == nullptr || player == nullptr)
            return true;
        if (channel->GetName() != DiscordChat->ConfigInGameChannelName)
            return true;

        DiscordChat->QueueOutboundMessage(player->GetName(), msg);
        return true;
    }
};

void AddDiscordChatPlayerScripts()
{
    new DiscordChat_PlayerScript();
}
