#include "WorldChat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "StringFormat.h"
#include "WorldSessionMgr.h"

static WC::Config g_wcConfig;

static std::string strToLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

static WC::PlayerState& GetState(Player const* player)
{
    return g_wcConfig.playerStates[player->GetGUID().GetCounter()];
}

static void SavePlayerState(Player const* player)
{
    WC::PlayerState const& state = GetState(player);

    CharacterDatabase.Execute(
        "REPLACE INTO `character_worldchat` (`guid`, `receive_enabled`, `world_mode`) VALUES ({}, {}, {})",
        player->GetGUID().GetCounter(),
        state.receiveEnabled ? 1 : 0,
        state.worldMode ? 1 : 0
    );
}

static void LoadPlayerState(Player* player)
{
    WC::PlayerState& state = GetState(player);

    state.receiveEnabled = g_wcConfig.loginState;
    state.worldMode = false;
    state.muteUntil = 0;
    state.recentMessages.clear();

    if (QueryResult result = CharacterDatabase.Query(
        "SELECT `receive_enabled`, `world_mode` FROM `character_worldchat` WHERE `guid` = {} LIMIT 1",
        player->GetGUID().GetCounter()))
    {
        state.receiveEnabled = (*result)[0].Get<uint8>() != 0;
        state.worldMode = (*result)[1].Get<uint8>() != 0;
    }
}

static bool IsFlooding(Player const& sender)
{
    if (sender.GetSession()->GetSecurity() > SEC_PLAYER)
        return false;

    WC::PlayerState& state = GetState(&sender);
    time_t now = time(nullptr);

    if (state.muteUntil > now)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}You are muted from World Chat for {} more seconds.|r",
            WC::ChatColor::WORLD,
            WC::ChatColor::YELLOW,
            uint32(state.muteUntil - now)
        );
        return true;
    }

    while (!state.recentMessages.empty() && now - state.recentMessages.front() > time_t(g_wcConfig.floodWindowSeconds))
        state.recentMessages.pop_front();

    state.recentMessages.push_back(now);

    if (state.recentMessages.size() == g_wcConfig.floodWarnCount)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}Please slow down. Continued spam will temporarily mute World Chat.|r",
            WC::ChatColor::WORLD,
            WC::ChatColor::YELLOW
        );
    }

    if (state.recentMessages.size() >= g_wcConfig.floodMuteCount)
    {
        state.muteUntil = now + g_wcConfig.floodMuteSeconds;
        state.recentMessages.clear();

        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}You have been temporarily muted from World Chat for {} seconds.|r",
            WC::ChatColor::WORLD,
            WC::ChatColor::RED,
            g_wcConfig.floodMuteSeconds
        );

        return true;
    }

    return false;
}

void WC::WorldChat_Config::OnBeforeConfigLoad(bool /*reload*/)
{
    g_wcConfig.enabled = sConfigMgr->GetOption<bool>("WorldChat.Enable", true);
    g_wcConfig.crossFaction = sConfigMgr->GetOption<bool>("WorldChat.CrossFaction", true);
    g_wcConfig.announce = sConfigMgr->GetOption<bool>("WorldChat.Announce", false);
    g_wcConfig.channelName = strToLower(sConfigMgr->GetOption<std::string>("WorldChat.ChannelName", "world"));
    g_wcConfig.loginState = sConfigMgr->GetOption<bool>("WorldChat.OnLogin.State", true);
    g_wcConfig.tag = sConfigMgr->GetOption<std::string>("WorldChat.Tag", "[World]");
    g_wcConfig.minimumLevel = sConfigMgr->GetOption<uint8>("WorldChat.MinimumLevel", 10);
    g_wcConfig.floodWindowSeconds = sConfigMgr->GetOption<uint32>("WorldChat.Flood.WindowSeconds", 10);
    g_wcConfig.floodWarnCount = sConfigMgr->GetOption<uint32>("WorldChat.Flood.WarnCount", 3);
    g_wcConfig.floodMuteCount = sConfigMgr->GetOption<uint32>("WorldChat.Flood.MuteCount", 5);
    g_wcConfig.floodMuteSeconds = sConfigMgr->GetOption<uint32>("WorldChat.Flood.MuteSeconds", 60);
}

void WC::SendWorldMessage(Player const& sender, std::string const& msg, int team)
{
    if (msg.empty())
        return;

    if (!g_wcConfig.enabled)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}World Chat is disabled.|r",
            ChatColor::WORLD,
            ChatColor::RED
        );
        return;
    }

    if (sender.GetLevel() < g_wcConfig.minimumLevel && sender.GetSession()->GetSecurity() == SEC_PLAYER)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}You must be level {} to use World Chat.|r",
            ChatColor::WORLD,
            ChatColor::YELLOW,
            g_wcConfig.minimumLevel
        );
        return;
    }

    if (!sender.CanSpeak())
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}You cannot use World Chat while muted.|r",
            ChatColor::WORLD,
            ChatColor::RED
        );
        return;
    }

    if (IsFlooding(sender))
        return;

    AccountTypes senderSecurity = sender.GetSession()->GetSecurity();
    std::string senderName = sender.GetName();
    std::string audienceTag;

    if (team == TEAM_HORDE)
        audienceTag = " |cffff2020[Horde Only]|r";
    else if (team == TEAM_ALLIANCE)
        audienceTag = " |cff3399ff[Alliance Only]|r";

    std::string outMessage;

    // The original msg is inserted untouched so WoW hyperlinks remain clickable.
    if (sender.isGMChat() || sender.IsDeveloper())
    {
        outMessage = Acore::StringFormat(
            "{}{}|r{} {}{}|Hplayer:{}|h{}|h|r: {}{}|r",
            ChatColor::WORLD,
            g_wcConfig.tag,
            audienceTag,
            GMIcon,
            ClassColor[sender.getClass()],
            senderName,
            senderName,
            ChatColor::TEXT,
            msg
        );
    }
    else
    {
        outMessage = Acore::StringFormat(
            "{}{}|r{} {}|Hplayer:{}|h{}|h|r: {}{}|r",
            ChatColor::WORLD,
            g_wcConfig.tag,
            audienceTag,
            ClassColor[sender.getClass()],
            senderName,
            senderName,
            ChatColor::TEXT,
            msg
        );
    }

    WorldSessionMgr::SessionMap sessions = sWorldSessionMgr->GetAllSessions();

    // Deliberately avoid std::views::values here. Direct map iteration is
    // compatible with both MSVC and GCC/Clang using older libstdc++ versions.
    for (auto const& sessionEntry : sessions)
    {
        WorldSession* session = sessionEntry.second;

        if (!session)
            continue;

        Player* target = session->GetPlayer();

        if (!target || !target->IsInWorld())
            continue;

        WC::PlayerState& targetState = GetState(target);

        if (!targetState.receiveEnabled)
            continue;

        if (team != -1 && target->GetTeamId() != team)
            continue;

        if (sender.GetTeamId() != target->GetTeamId()
            && !g_wcConfig.crossFaction
            && senderSecurity == SEC_PLAYER
            && target->GetSession()->GetSecurity() == SEC_PLAYER)
        {
            continue;
        }

        ChatHandler(target->GetSession()).PSendSysMessage(outMessage);
    }
}

void WC::SendWorldAnnouncement(std::string const& msg)
{
    if (msg.empty())
        return;

    std::string outMessage = Acore::StringFormat(
        "{}[Announcement]|r {}{}|r",
        ChatColor::ANNOUNCE,
        ChatColor::TEXT,
        msg
    );

    WorldSessionMgr::SessionMap sessions = sWorldSessionMgr->GetAllSessions();

    for (auto const& sessionEntry : sessions)
    {
        WorldSession* session = sessionEntry.second;

        if (!session)
            continue;

        Player* target = session->GetPlayer();

        if (!target || !target->IsInWorld())
            continue;

        ChatHandler(target->GetSession()).PSendSysMessage(outMessage);
    }
}

void WC::WorldChat_Announce::OnPlayerLogin(Player* player)
{
    LoadPlayerState(player);

    if (g_wcConfig.enabled && g_wcConfig.announce)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "{}[World]|r {}Use .world message, .world on, .world off, .world status, or .world to toggle World Chat mode.|r",
            ChatColor::WORLD,
            ChatColor::GREY
        );
    }
}

bool WC::WorldChat_Announce::OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg)
{
    if (!player || msg.empty())
        return true;

    if (lang == LANG_ADDON)
        return true;

    if (type != CHAT_MSG_SAY)
        return true;

    WC::PlayerState& state = GetState(player);

    if (!state.worldMode)
        return true;

    SendWorldMessage(*player, msg, -1);
    return false;
}

bool WC::WorldChat_Announce::OnPlayerCanUseChat(
    Player* player,
    uint32 /*type*/,
    uint32 lang,
    std::string& msg,
    Channel* channel)
{
    if (!player || !channel)
        return true;

    if (g_wcConfig.channelName != ""
        && lang != LANG_ADDON
        && strToLower(channel->GetName()) == g_wcConfig.channelName)
    {
        SendWorldMessage(*player, msg, -1);
        return false;
    }

    return true;
}

bool WC::WorldChat::HandleWorldChatCommand(ChatHandler* handler, const char* msg)
{
    Player* player = handler->GetSession()->GetPlayer();
    std::string message = Acore::String::Trim(std::string(msg ? msg : ""));

    WC::PlayerState& state = GetState(player);

    if (message.empty())
    {
        state.worldMode = !state.worldMode;
        SavePlayerState(player);

        handler->PSendSysMessage(
            "{}[World]|r {}World Chat mode is now {}.|r",
            ChatColor::WORLD,
            state.worldMode ? ChatColor::GREEN : ChatColor::YELLOW,
            state.worldMode ? "ON" : "OFF"
        );

        return true;
    }

    SendWorldMessage(*player, message, -1);
    return true;
}

bool WC::WorldChat::HandleWorldChatOnCommand(ChatHandler* handler)
{
    Player* player = handler->GetSession()->GetPlayer();
    WC::PlayerState& state = GetState(player);

    state.receiveEnabled = true;
    SavePlayerState(player);

    handler->PSendSysMessage(
        "{}[World]|r {}World Chat is now visible.|r",
        ChatColor::WORLD,
        ChatColor::GREEN
    );
    return true;
}

bool WC::WorldChat::HandleWorldChatOffCommand(ChatHandler* handler)
{
    Player* player = handler->GetSession()->GetPlayer();
    WC::PlayerState& state = GetState(player);

    state.receiveEnabled = false;
    state.worldMode = false;
    SavePlayerState(player);

    handler->PSendSysMessage(
        "{}[World]|r {}World Chat is now hidden.|r",
        ChatColor::WORLD,
        ChatColor::YELLOW
    );
    return true;
}

bool WC::WorldChat::HandleWorldChatStatusCommand(ChatHandler* handler)
{
    Player* player = handler->GetSession()->GetPlayer();
    WC::PlayerState& state = GetState(player);

    handler->PSendSysMessage(
        "{}[World]|r {}Visible: {} | Mode: {} | Minimum Level: {}|r",
        ChatColor::WORLD,
        ChatColor::GREY,
        state.receiveEnabled ? "ON" : "OFF",
        state.worldMode ? "ON" : "OFF",
        g_wcConfig.minimumLevel
    );

    return true;
}

bool WC::WorldChat::HandleWorldChatAnnounceCommand(ChatHandler* handler, const char* msg)
{
    std::string message = Acore::String::Trim(std::string(msg ? msg : ""));

    if (message.empty())
    {
        handler->SendSysMessage("Usage: .worldgm message");
        return false;
    }

    SendWorldAnnouncement(message);
    return true;
}

bool WC::WorldChat::HandleWorldChatHordeCommand(ChatHandler* handler, const char* msg)
{
    SendWorldMessage(*handler->GetSession()->GetPlayer(), msg ? msg : "", TEAM_HORDE);
    return true;
}

bool WC::WorldChat::HandleWorldChatAllianceCommand(ChatHandler* handler, const char* msg)
{
    SendWorldMessage(*handler->GetSession()->GetPlayer(), msg ? msg : "", TEAM_ALLIANCE);
    return true;
}

Acore::ChatCommands::ChatCommandTable WC::WorldChat::GetCommands() const
{
    static Acore::ChatCommands::ChatCommandTable worldCommandTable =
    {
        { "on", HandleWorldChatOnCommand, SEC_PLAYER, Acore::ChatCommands::Console::No },
        { "off", HandleWorldChatOffCommand, SEC_PLAYER, Acore::ChatCommands::Console::No },
        { "status", HandleWorldChatStatusCommand, SEC_PLAYER, Acore::ChatCommands::Console::No },
        { "", HandleWorldChatCommand, SEC_PLAYER, Acore::ChatCommands::Console::No },
    };

    static Acore::ChatCommands::ChatCommandTable commandTable =
    {
        { "world", worldCommandTable },
        { "worldgm", HandleWorldChatAnnounceCommand, SEC_MODERATOR, Acore::ChatCommands::Console::No },
        { "worldh", HandleWorldChatHordeCommand, SEC_MODERATOR, Acore::ChatCommands::Console::No },
        { "worlda", HandleWorldChatAllianceCommand, SEC_MODERATOR, Acore::ChatCommands::Console::No },
    };

    return commandTable;
}

void AddSC_WorldChatScripts()
{
    new WC::WorldChat_Config();
    new WC::WorldChat_Announce();
    new WC::WorldChat();
}
