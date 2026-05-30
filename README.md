## Description

AzerothCore mod that creates an in-game chat channel and bridges it to a Discord channel in both directions.

On login, players are automatically joined to a configurable channel (default `discord`). Anything they say in that channel is forwarded to Discord via a webhook, and a background worker polls the Discord REST API on a configurable interval so messages sent from Discord show up in that same in-game channel for everyone joined.

## Configuration

Copy `conf/DiscordChat.conf.dist` to `conf/DiscordChat.conf` and edit. Important values:

- `DiscordChat.ServerName` – tag shown before forwarded messages so multiple realms are distinguishable.
- `DiscordChat.InGame.ChannelName` – name of the in-game bridge channel.
- `DiscordChat.InGame.AutoJoinOnLogin` – when true, every player is auto-joined to the channel on login.
- `DiscordChat.Discord.ApiBaseUrl` – Discord REST base URL (default `https://discord.com/api/v10`).
- `DiscordChat.Discord.WebhookUrl` – Discord webhook used to post in-game chat to Discord (outbound).
- `DiscordChat.Discord.BotToken` + `DiscordChat.Discord.ChannelId` – authentication and target channel for inbound polling.
- `DiscordChat.Discord.PollIntervalInMS` – how often the worker thread polls Discord for new messages.

See the comments in `DiscordChat.conf.dist` for the full list and defaults.

## Discord setup

The bridge has two halves: a webhook for in-game → Discord, and a bot for Discord → in-game.

### 1. Outbound (in-game → Discord): create a webhook

You only need a webhook for the "messages typed in-game show up in Discord" direction.

1. In Discord, open the target server → right-click the target channel → **Edit Channel**.
2. Go to **Integrations** → **Webhooks** → **New Webhook**.
3. (Optional) Give it a name/avatar — these are overridden per-message by `DiscordChat.Discord.WebhookUsername` anyway.
4. Click **Copy Webhook URL**. It will look like:
   `https://discord.com/api/webhooks/123456789012345678/aBcDeF...`
5. Paste it into `DiscordChat.Discord.WebhookUrl` in `DiscordChat.conf`.

No bot is needed for this direction. Webhooks are append-only and can't read messages, which is why you also need a bot for the other direction.

### 2. Inbound (Discord → in-game): create a bot

1. Go to https://discord.com/developers/applications → **New Application** → name it (e.g. "AzerothCore Bridge").
2. In the left sidebar, click **Bot** → **Reset Token** → **Copy**. Paste this into `DiscordChat.Discord.BotToken`.
3. Still on the **Bot** page, under **Privileged Gateway Intents**, enable **Message Content Intent**. (Required to read message text via the REST API.)
4. In the sidebar click **OAuth2** → **URL Generator**:
   - **Scopes**: check `bot`.
   - **Bot Permissions**: check `View Channel` and `Read Message History`.
   - Copy the generated URL at the bottom, open it in a browser, choose your Discord server, and **Authorize**.
5. Get the **channel ID**: in Discord, **User Settings → Advanced → Developer Mode = ON**. Then right-click the bridge channel → **Copy Channel ID**. Paste this into `DiscordChat.Discord.ChannelId`.

### 3. Wire it together

Edit `conf/DiscordChat.conf` (copy from `DiscordChat.conf.dist` if you haven't already):

```
DiscordChat.Discord.WebhookUrl = "https://discord.com/api/webhooks/..."
DiscordChat.Discord.BotToken   = "MTE..."
DiscordChat.Discord.ChannelId  = "123456789012345678"
```

The webhook URL and the bot's channel ID should point at the **same** Discord channel so the bridge is symmetric.

### 4. Quick sanity check

After restarting worldserver:

- Log in a character → you should be auto-joined to the `discord` channel (`/chat list` or open the chat channels pane).
- Type something there → it appears in Discord as `**[AzerothCore] YourName**: hello`.
- Type something in the Discord channel from a real user account → within `PollIntervalInMS` (default 5 s) it appears in-game as `[AzerothCore <- Discord] DiscordUser: hello`.

The bot's own webhook posts are filtered out by the `"bot": true` check in `ExtractDiscordMessages`, so you won't get an echo loop.

### Gotchas

- **Rate limits**: Discord allows ~50 requests/second per bot. The default 5000 ms polling is well under that even on busy servers; keep it ≥ 2000 ms.
- **First poll is silent**: the worker establishes a baseline `LastSeenDiscordMessageId` on the first poll and only forwards messages that arrive *after* the server started — so you won't get a flood of channel history on boot.
- **Webhook vs bot identity**: messages posted by the webhook appear from your webhook's name/avatar (not the bot's), which is why the bot doesn't need `Send Messages` permission — only read permissions.
- **TLS verification**: the included HTTPS client currently uses `ssl::verify_none` to dodge CA-path quirks on first build. Once you confirm it works, flip it to `ssl::verify_peer` in `DiscordChat.cpp` for production.

## How it works

- A `WorldScript` loads config at startup and spins up a background worker thread.
- A `PlayerScript` auto-joins each player to the bridge channel on login and captures their messages via the `OnPlayerCanUseChat` hook to push them onto an outbound queue.
- The worker thread POSTs queued outbound messages to the Discord webhook over HTTPS (Boost.Beast + OpenSSL).
- The same worker thread polls `GET /channels/{id}/messages` on the configured interval, filters out the bot's own posts, and pushes inbound messages onto an inbound queue.
- The world tick drains the inbound queue and broadcasts each message to every joined player as a `CHAT_MSG_CHANNEL` packet.

This was mostly written by Claude Opus 4.7