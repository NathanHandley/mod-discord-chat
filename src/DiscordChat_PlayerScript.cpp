//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU Affero General Public License as published by the
//  Free Software Foundation; either version 3 of the License, or (at your
//  option) any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.See the GNU Affero General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Channel.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "DiscordChat.h"

using namespace std;

class DiscordChat_PlayerScript : public PlayerScript
{
public:
    DiscordChat_PlayerScript() : PlayerScript("DiscordChat_PlayerScript") {}

    void OnPlayerLogout(Player* player) override
    {
        if (DiscordChat->IsEnabled == false)
            return;
        DiscordChat->UntrackPlayer(player);
    }

    // Capture chat the player tries to send into the bridge channel and forward
    // it to Discord. Also use this as our membership signal: a player can only
    // reach this hook with our channel pointer if they're actually a member, so
    // remember the GUID for channel-scoped inbound delivery later. Returning
    // true lets the message also broadcast to in-game channel members normally.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Channel* channel) override
    {
        if (DiscordChat->IsEnabled == false)
            return true;
        if (channel == nullptr || player == nullptr)
            return true;
        if (channel->GetName() != DiscordChat->ConfigInGameChannelName)
            return true;

        DiscordChat->TrackPlayerInChannel(player);
        DiscordChat->QueueOutboundMessage(player->GetName(), msg);
        return true;
    }
};

void AddDiscordChatPlayerScripts()
{
    new DiscordChat_PlayerScript();
}
