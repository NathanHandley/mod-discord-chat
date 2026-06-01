## Description

AzerothCore mod that bridges in-game chat to a Discord channel in both directions.

There are two ways players can send a message to Discord:

1. Type `.discord <message>` (or the shorter `.d <message>`) as a chat command. This works without joining any channel.
2. Join the configured in-game bridge channel (default `discord`) with `/join discord` and chat there normally. Every message in that channel is forwarded to Discord.

Inbound messages from Discord are delivered two different ways depending on the receiving player:

- **Players who have joined the bridge channel** see the message as a normal channel chat line, attributed in-game to a configured "speaker" character (see `DiscordChat.InGame.SpeakerCharacterGUID`).
- **Players who have not joined the bridge channel** see the message as a system message in their default chat tab, so they still get the content without having to opt in to the channel.

Media posted in Discord (images, videos, files, ...) cannot be displayed by the 3.3.5a client so the mod delivers a short text placeholder instead like `[image: cat.png]`, `[video]`, or `[file: log.txt]`.
		 
## Configuration

Copy `conf/DiscordChat.conf.dist` to `conf/DiscordChat.conf` and edit. Important values:

- `DiscordChat.ServerName` – tag shown before forwarded messages so multiple realms are distinguishable.
- `DiscordChat.InGame.ChannelName` – name of the in-game bridge channel (players join with `/join <name>`).
- `DiscordChat.InGame.SpeakerCharacterGUID` – character GUID (from the `characters` table) used as the in-game speaker for Discord messages delivered into the bridge channel. Required for channel-scoped inbound delivery; set to `0` to disable it and have all inbound Discord traffic delivered as system messages. **Do not use a bot character (such as playerbot bots) or it will likely crash the server.**
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

### 3. Pick a speaker character (optional, but recommended)

For channel-scoped inbound delivery you need a character GUID in your `characters` database. Pick or create a normal player character on your realm whose name should appear as the in-game speaker for Discord messages — something like *"Discord"* or *"Herald"*. Then:

```sql
SELECT guid, name FROM characters WHERE name = 'YourSpeaker';
```

Paste the GUID value into `DiscordChat.InGame.SpeakerCharacterGUID`. The character does not need to be online; the client looks the name up on demand. **Do not use a bot character (such as playerbot bots) or it will likely crash the server.**

If you leave this at `0`, the channel direction still works (you can chat in-game and it forwards to Discord), but inbound Discord messages will be delivered to *everyone* as system messages instead of being routed through the channel.

### 4. Wire it together

Edit `conf/DiscordChat.conf` (copy from `DiscordChat.conf.dist` if you haven't already):

```
DiscordChat.InGame.SpeakerCharacterGUID = 12
DiscordChat.Discord.WebhookUrl          = "https://discord.com/api/webhooks/..."
DiscordChat.Discord.BotToken            = "MTE..."
DiscordChat.Discord.ChannelId           = "123456789012345678"
```

The webhook URL and the bot's channel ID should point at the **same** Discord channel so the bridge is symmetric.

### 5. Quick sanity check

After restarting worldserver:

- Log in a character.
- Send to Discord via dot command: `.discord hello` → appears in Discord as `**[AzerothCore] YourName**: hello`.
- Or join the channel and chat there: `/join discord`, then type in the new channel tab — same forwarding behaviour.
- Type something in the Discord channel from a real user account → within `PollIntervalInMS` (default 5 s):
  - if you have joined the bridge channel, the message shows up in the channel tab as `[Discord] DiscordUser: hello`, attributed to the configured speaker character;
  - if you haven't, it shows up as a system message in your default tab as `[YourServer <- Discord] DiscordUser: hello`.

### Why players have to /join manually

The WoW 3.3.5a client only allocates a chat-tab slot for a channel when *the client itself* sends `CMSG_JOIN_CHANNEL` (i.e., when the user types `/join`). Server-initiated joins via `Channel::JoinChannel` register the player on the channel server-side and deliver the `SMSG_CHANNEL_NOTIFY` packet, but the client's chat UI doesn't add the channel to its switchable tab list — so `/N` shortcuts don't reach it and the channel isn't visible in the chat tab.

### Gotchas

- **Rate limits**: Discord allows ~50 requests/second per bot. The default 5000 ms polling is well under that even on busy servers; keep it ≥ 2000 ms.
- **First poll is silent**: the worker establishes a baseline `LastSeenDiscordMessageId` on the first poll and only forwards messages that arrive *after* the server started — so you won't get a flood of channel history on boot.
- **Webhook vs bot identity**: messages posted by the webhook appear from your webhook's name/avatar (not the bot's), which is why the bot doesn't need `Send Messages` permission — only read permissions.
- **Empty webhook username**: Discord rejects webhook POSTs whose `username` is empty (HTTP 400). The mod falls back to `ServerName` if the conf has `WebhookUsername = ""`.