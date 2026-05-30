#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "DiscordChat.h"

using namespace Acore::ChatCommands;
using namespace std;

class DiscordChat_CommandScript : public CommandScript
{
public:
    DiscordChat_CommandScript() : CommandScript("DiscordChat_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable discordCommandTable =
        {
            { "discord", HandleDiscordCommand, SEC_PLAYER, Console::No },
            { "d",       HandleDiscordCommand, SEC_PLAYER, Console::No },
        };
        return discordCommandTable;
    }

    static bool HandleDiscordCommand(ChatHandler* handler, Tail message)
    {
        if (DiscordChat->IsEnabled == false)
        {
            handler->PSendSysMessage("Discord chat is disabled on this server.");
            return true;
        }
        if (DiscordChat->ConfigDiscordOutgoingEnabled == false)
        {
            handler->PSendSysMessage("Discord outbound is disabled on this server.");
            return true;
        }
        if (message.empty() == true)
        {
            handler->PSendSysMessage("Usage: .discord <message>");
            return true;
        }

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        string authorName = player ? player->GetName() : "console";
        DiscordChat->QueueOutboundMessage(authorName, string(message));

        handler->PSendSysMessage("Sent to Discord: {}", string(message));
        return true;
    }
};

void AddDiscordChatCommandScripts()
{
    new DiscordChat_CommandScript();
}
