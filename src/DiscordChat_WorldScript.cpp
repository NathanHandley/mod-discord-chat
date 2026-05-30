#include "Configuration/Config.h"
#include "ScriptMgr.h"

#include "DiscordChat.h"

using namespace std;

class DiscordChat_WorldScript : public WorldScript
{
public:
    DiscordChat_WorldScript() : WorldScript("DiscordChat_WorldScript"), DrainAccumulatorMS(0) {}

    void OnAfterConfigLoad(bool reload) override
    {
        DiscordChat->LoadConfigurationFile();
        if (DiscordChat->IsEnabled == false)
            return;
        if (reload == false)
            DiscordChat->StartWorker();
    }

    void OnShutdown() override
    {
        DiscordChat->StopWorker();
    }

    // The worker thread populates an inbound queue; this hook drains it on the
    // world tick so packets are sent from the main game thread.
    void OnUpdate(uint32 diff) override
    {
        if (DiscordChat->IsEnabled == false)
            return;

        DrainAccumulatorMS += diff;
        if (DrainAccumulatorMS < 250)
            return;
        DrainAccumulatorMS = 0;
        DiscordChat->BroadcastPendingInboundMessages();
    }

private:
    uint32 DrainAccumulatorMS;
};

void AddDiscordChatWorldScripts()
{
    new DiscordChat_WorldScript();
}
