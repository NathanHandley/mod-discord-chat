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

## How it works

- A `WorldScript` loads config at startup and spins up a background worker thread.
- A `PlayerScript` auto-joins each player to the bridge channel on login and captures their messages via the `OnPlayerCanUseChat` hook to push them onto an outbound queue.
- The worker thread POSTs queued outbound messages to the Discord webhook over HTTPS (Boost.Beast + OpenSSL).
- The same worker thread polls `GET /channels/{id}/messages` on the configured interval, filters out the bot's own posts, and pushes inbound messages onto an inbound queue.
- The world tick drains the inbound queue and broadcasts each message to every joined player as a `CHAT_MSG_CHANNEL` packet.

This was mostly written by Claude Opus 4.7