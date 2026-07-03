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
        // The worker thread reads the config strings (webhook URL, bot token, status messages) while it runs, so
        // on a live ".reload config" it must be stopped (joined) before LoadConfigurationFile reassigns them.
        // This also lets a reload enable/disable the bridge cleanly. Worst case the join waits out one in-flight
        // HTTP request, which is bounded by DiscordChat.Discord.HttpTimeoutInMS.
        if (reload == true)
            DiscordChat->StopWorker();
        DiscordChat->LoadConfigurationFile();
        if (DiscordChat->IsEnabled == false)
            return;
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

        DiscordChat->ProcessPendingJoinReminders(diff);

        DrainAccumulatorMS += diff;
        if (DrainAccumulatorMS < 250)
            return;
        DrainAccumulatorMS = 0;
        DiscordChat->BroadcastPendingInboundMessages();
        DiscordChat->BroadcastPendingBridgeStatusNotices();
    }

private:
    uint32 DrainAccumulatorMS;
};

void AddDiscordChatWorldScripts()
{
    new DiscordChat_WorldScript();
}
