#include "Channel.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "DiscordChat.h"

using namespace std;

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
        DiscordChat->AutoJoinPlayerToChannel(player);
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
