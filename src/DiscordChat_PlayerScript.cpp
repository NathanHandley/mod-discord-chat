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
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "DiscordChat.h"

#include <exception>

using namespace std;

class DiscordChat_PlayerScript : public PlayerScript
{
public:
    DiscordChat_PlayerScript() : PlayerScript("DiscordChat_PlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (DiscordChat->IsEnabled == false)
            return;
        // AT_LOGIN_FIRST is still set here; the core clears it right after this
        // hook (and before OnPlayerFirstLogin), so this is where we can tell
        // whether the character has ever logged in before. Mode 1 uses it to
        // remind new characters only.
        bool isFirstLogin = player != nullptr && player->HasAtLoginFlag(AT_LOGIN_FIRST);
        // Queue a deferred reminder; whether it actually fires depends on the
        // player not being on the bridge channel once the delay elapses.
        DiscordChat->QueuePendingJoinReminder(player, isFirstLogin);
    }

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
        // Channel names are case-insensitive in WoW, and Channel::GetName()
        // returns the name with whatever casing the first player to /join it
        // used after the last restart (see ChannelMgr::GetJoinChannel). A plain
        // != comparison against the configured name therefore silently drops
        // outbound messages whenever that casing differs, which is why the
        // game->Discord direction worked on some restarts but not others. Match
        // case-insensitively, the same way the join/leave tracking does.
        if (DiscordChat->IsBridgeChannelName(channel->GetName()) == false)
            return true;

        DiscordChat->TrackPlayerInChannel(player);
        DiscordChat->QueueOutboundMessage(player->GetName(), msg);
        return true;
    }
};

// AzerothCore has no channel join/leave script hook and Channel::IsOn is
// private, so we cannot ask the core whether a player is on the bridge channel.
// Instead we watch the inbound channel opcodes here and maintain the mod's own
// membership set. This is what lets the deferred login reminder tell whether a
// player has (re)joined the channel without touching core code.
class DiscordChat_ServerScript : public ServerScript
{
public:
    DiscordChat_ServerScript() : ServerScript("DiscordChat_ServerScript") {}

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (DiscordChat->IsEnabled == false)
            return true;
        if (session == nullptr)
            return true;

        uint32 opcode = packet.GetOpcode();
        if (opcode != CMSG_JOIN_CHANNEL && opcode != CMSG_LEAVE_CHANNEL)
            return true;

        Player* player = session->GetPlayer();
        if (player == nullptr)
            return true;

        // Copy before reading: the original is const and must stay intact for
        // the core's own handler, which parses it after us.
        WorldPacket reader(packet);
        string channelName;
        try
        {
            if (opcode == CMSG_JOIN_CHANNEL)
            {
                uint32 channelId;
                uint8 unk1, unk2;
                string password;
                reader >> channelId >> unk1 >> unk2 >> channelName >> password;
            }
            else
            {
                uint32 unk;
                reader >> unk >> channelName;
            }
        }
        catch (std::exception const&)
        {
            // Malformed/short packet -- let the core handler deal with it.
            return true;
        }

        if (DiscordChat->IsBridgeChannelName(channelName) == true)
        {
            if (opcode == CMSG_JOIN_CHANNEL)
                DiscordChat->TrackPlayerInChannel(player);
            else
                DiscordChat->UntrackChannelMembership(player);
        }
        return true;
    }
};

void AddDiscordChatPlayerScripts()
{
    new DiscordChat_PlayerScript();
    new DiscordChat_ServerScript();
}
