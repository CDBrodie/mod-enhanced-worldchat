#include <algorithm>
#include <cctype>
#include <sstream>

#include "WorldChat.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "SocialMgr.h"
#include "StringFormat.h"
#include "WorldSessionMgr.h"

static WC::Config g_wcConfig;

static std::string strToLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });

    return str;
}

static std::string TrimString(std::string str)
{
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front())))
        str.erase(str.begin());

    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))
        str.pop_back();

    return str;
}

static std::vector<std::string> SplitCsv(std::string const& input)
{
    std::vector<std::string> result;
    std::stringstream ss(input);
    std::string token;

    while (std::getline(ss, token, ','))
    {
        token = strToLower(TrimString(token));
        if (!token.empty())
            result.push_back(token);
    }

    return result;
}

static std::string NormalizeMessage(std::string msg)
{
    msg = strToLower(msg);

    std::string normalized;
    normalized.reserve(msg.size());

    bool lastWasSpace = false;

    for (char c : msg)
    {
        unsigned char uc = static_cast<unsigned char>(c);

        if (std::isalnum(uc))
        {
            normalized += static_cast<char>(uc);
            lastWasSpace = false;
        }
        else if (std::isspace(uc))
        {
            if (!lastWasSpace)
            {
                normalized += ' ';
                lastWasSpace = true;
            }
        }
    }

    return TrimString(normalized);
}

static bool ContainsAny(std::string const& lowerMsg, std::initializer_list<char const*> terms)
{
    for (char const* term : terms)
        if (lowerMsg.find(term) != std::string::npos)
            return true;

    return false;
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

    state.lastNormalizedMessage.clear();
    state.lastDuplicateTime = 0;
    state.duplicateCount = 0;

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

static bool IsDuplicateMessage(Player const& sender, std::string const& msg)
{
    if (!g_wcConfig.duplicateDetection)
        return false;

    if (sender.GetSession()->GetSecurity() > SEC_PLAYER)
        return false;

    std::string normalized = NormalizeMessage(msg);

    if (normalized.empty())
        return false;

    WC::PlayerState& state = GetState(&sender);
    time_t now = time(nullptr);

    if (state.lastNormalizedMessage == normalized &&
        now - state.lastDuplicateTime <= time_t(g_wcConfig.duplicateWindowSeconds))
    {
        ++state.duplicateCount;
    }
    else
    {
        state.lastNormalizedMessage = normalized;
        state.lastDuplicateTime = now;
        state.duplicateCount = 1;
    }

    if (state.duplicateCount > g_wcConfig.duplicateMaxRepeats)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage(
            "{}[World]|r {}Please avoid repeating the same message.|r",
            WC::ChatColor::WORLD,
            WC::ChatColor::YELLOW
        );
        return true;
    }

    return false;
}

static bool ContainsBlockedUrl(Player const& sender, std::string const& msg)
{
    if (!g_wcConfig.urlFilter)
        return false;

    if (sender.GetSession()->GetSecurity() > SEC_PLAYER)
        return false;

    std::string lowerMsg = strToLower(msg);

    for (std::string const& term : g_wcConfig.blockedUrlTerms)
    {
        if (!term.empty() && lowerMsg.find(term) != std::string::npos)
        {
            ChatHandler(sender.GetSession()).PSendSysMessage(
                "{}[World]|r {}Links and advertising are not allowed in World Chat.|r",
                WC::ChatColor::WORLD,
                WC::ChatColor::YELLOW
            );
            return true;
        }
    }

    return false;
}

static std::string GetDetectedTag(std::string const& msg)
{
    std::string lowerMsg = strToLower(msg);

    if (g_wcConfig.tradeDetection)
    {
        if (ContainsAny(lowerMsg,
            {
                "wts", "wtb", "wtt", "lfw",
                "selling", "buying", "for sale",
                "enchanting", "jewelcrafting", "blacksmithing",
                "alchemy", "tailoring", "leatherworking", "engineering"
            }))
        {
            return g_wcConfig.tradeTag;
        }
    }

    if (g_wcConfig.groupDetection)
    {
        if (ContainsAny(lowerMsg,
            {
                "lfg", "lfm", "looking for group", "looking for more",
                "need tank", "need healer", "need heals", "need dps",
                "tank needed", "healer needed", "dps needed",
                "rdf", "heroic", "naxx", "ulduar", "toc", "icc", "voa", "os", "eoe"
            }))
        {
            return g_wcConfig.groupTag;
        }
    }

    return g_wcConfig.tag;
}

static std::string GetTagColor(std::string const& tag)
{
    if (tag == g_wcConfig.tradeTag)
        return std::string(WC::ChatColor::TRADE);

    if (tag == g_wcConfig.groupTag)
        return std::string(WC::ChatColor::GROUP);

    return std::string(WC::ChatColor::WORLD);
}

static bool IsIgnoredByTarget(Player* target, Player const& sender)
{
    if (!g_wcConfig.ignoreSupport)
        return false;

    if (!target || target == &sender)
        return false;

    PlayerSocial* social = target->GetSocial();
    if (!social)
        return false;

    return social->HasIgnore(sender.GetGUID());
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
    g_wcConfig.ignoreSupport = sConfigMgr->GetOption<bool>("WorldChat.IgnoreSupport", true);

    g_wcConfig.floodWindowSeconds = sConfigMgr->GetOption<uint32>("WorldChat.Flood.WindowSeconds", 10);
    g_wcConfig.floodWarnCount = sConfigMgr->GetOption<uint32>("WorldChat.Flood.WarnCount", 3);
    g_wcConfig.floodMuteCount = sConfigMgr->GetOption<uint32>("WorldChat.Flood.MuteCount", 5);
    g_wcConfig.floodMuteSeconds = sConfigMgr->GetOption<uint32>("WorldChat.Flood.MuteSeconds", 60);

    g_wcConfig.duplicateDetection = sConfigMgr->GetOption<bool>("WorldChat.DuplicateDetection", true);
    g_wcConfig.duplicateWindowSeconds = sConfigMgr->GetOption<uint32>("WorldChat.Duplicate.WindowSeconds", 30);
    g_wcConfig.duplicateMaxRepeats = sConfigMgr->GetOption<uint32>("WorldChat.Duplicate.MaxRepeats", 2);

    g_wcConfig.urlFilter = sConfigMgr->GetOption<bool>("WorldChat.UrlFilter", true);
    g_wcConfig.blockedUrlTerms = SplitCsv(sConfigMgr->GetOption<std::string>(
        "WorldChat.UrlFilter.BlockedTerms",
        "http://,https://,www.,.com,.net,.org,discord.gg,bit.ly,tinyurl"
    ));

    g_wcConfig.tradeDetection = sConfigMgr->GetOption<bool>("WorldChat.TradeDetection", true);
    g_wcConfig.groupDetection = sConfigMgr->GetOption<bool>("WorldChat.GroupDetection", true);

    g_wcConfig.tradeTag = sConfigMgr->GetOption<std::string>("WorldChat.TradeTag", "[Trade]");
    g_wcConfig.groupTag = sConfigMgr->GetOption<std::string>("WorldChat.GroupTag", "[Group]");
}

void WC::SendWorldMessage(Player const& sender, std::string const& msg, int team)
{
    if (msg.empty())
        return;

    if (!g_wcConfig.enabled)
    {
        ChatHandler(sender.GetSession()).PSendSysMessage("{}[World]|r {}World Chat is disabled.|r", ChatColor::WORLD, ChatColor::RED);
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
        ChatHandler(sender.GetSession()).PSendSysMessage("{}[World]|r {}You cannot use World Chat while muted.|r", ChatColor::WORLD, ChatColor::RED);
        return;
    }

    if (ContainsBlockedUrl(sender, msg))
        return;

    if (IsDuplicateMessage(sender, msg))
        return;

    if (IsFlooding(sender))
        return;

    AccountTypes senderSecurity = sender.GetSession()->GetSecurity();
    std::string senderName = sender.GetName();

    std::string displayTag = GetDetectedTag(msg);
    std::string tagColor = GetTagColor(displayTag);

    std::string audienceTag;

    if (team == TEAM_HORDE)
        audienceTag = " |cffff2020[Horde Only]|r";
    else if (team == TEAM_ALLIANCE)
        audienceTag = " |cff3399ff[Alliance Only]|r";

    std::string outMessage;

    // The original msg is inserted untouched to preserve WoW hyperlinks.
    if (sender.isGMChat() || sender.IsDeveloper())
    {
        outMessage = Acore::StringFormat(
            "{}{}|r{} {}{}|Hplayer:{}|h{}|h|r: {}{}|r",
            tagColor,
            displayTag,
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
            tagColor,
            displayTag,
            audienceTag,
            ClassColor[sender.getClass()],
            senderName,
            senderName,
            ChatColor::TEXT,
            msg
        );
    }

    WorldSessionMgr::SessionMap sessions = sWorldSessionMgr->GetAllSessions();

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

        // Staff always receive faction-restricted World Chat messages so they can monitor both factions.
        if (team != -1 &&
            target->GetTeamId() != team &&
            target->GetSession()->GetSecurity() == SEC_PLAYER)
        {
            continue;
        }

        if (sender.GetTeamId() != target->GetTeamId()
            && !g_wcConfig.crossFaction
            && senderSecurity == SEC_PLAYER
            && target->GetSession()->GetSecurity() == SEC_PLAYER)
            continue;

        if (IsIgnoredByTarget(target, sender))
            continue;

        ChatHandler(target->GetSession()).PSendSysMessage(outMessage);
    }
}

static void SendSystemTaggedMessage(std::string const& tag, std::string const& tagColor, std::string const& msg, int team, bool gmOnly)
{
    if (msg.empty())
        return;

    std::string outMessage = Acore::StringFormat(
        "{}{}|r {}{}|r",
        tagColor,
        tag,
        WC::ChatColor::TEXT,
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

        if (gmOnly && session->GetSecurity() == SEC_PLAYER)
            continue;

        // Staff always receive faction-restricted API messages too.
        if (team != -1 &&
            target->GetTeamId() != team &&
            session->GetSecurity() == SEC_PLAYER)
        {
            continue;
        }

        ChatHandler(target->GetSession()).PSendSysMessage(outMessage);
    }
}

static void GetApiTagAndColor(WC::MessageChannel channel, std::string& tag, std::string& color, int& team, bool& gmOnly)
{
    tag = g_wcConfig.tag;
    color = std::string(WC::ChatColor::WORLD);
    team = -1;
    gmOnly = false;

    switch (channel)
    {
        case WC::MessageChannel::Announcement:
            tag = "[Announcement]";
            color = std::string(WC::ChatColor::ANNOUNCE);
            break;

        case WC::MessageChannel::Trade:
            tag = g_wcConfig.tradeTag;
            color = std::string(WC::ChatColor::TRADE);
            break;

        case WC::MessageChannel::Group:
            tag = g_wcConfig.groupTag;
            color = std::string(WC::ChatColor::GROUP);
            break;

        case WC::MessageChannel::Horde:
            tag = g_wcConfig.tag + " |cffff2020[Horde Only]|r";
            team = TEAM_HORDE;
            break;

        case WC::MessageChannel::Alliance:
            tag = g_wcConfig.tag + " |cff3399ff[Alliance Only]|r";
            team = TEAM_ALLIANCE;
            break;

        case WC::MessageChannel::GM:
            tag = "[GM]";
            color = std::string(WC::ChatColor::ANNOUNCE);
            gmOnly = true;
            break;

        case WC::MessageChannel::World:
        default:
            break;
    }
}

void WC::SendWorldAnnouncement(std::string const& msg)
{
    SendSystemTaggedMessage("[Announcement]", std::string(ChatColor::ANNOUNCE), msg, -1, false);
}

void WC::Broadcast(std::string const& msg)
{
    SendSystemTaggedMessage(g_wcConfig.tag, std::string(ChatColor::WORLD), msg, -1, false);
}

void WC::BroadcastFaction(std::string const& msg, int team)
{
    SendSystemTaggedMessage(g_wcConfig.tag, std::string(ChatColor::WORLD), msg, team, false);
}

void WC::BroadcastTrade(std::string const& msg)
{
    SendSystemTaggedMessage(g_wcConfig.tradeTag, std::string(ChatColor::TRADE), msg, -1, false);
}

void WC::BroadcastGroup(std::string const& msg)
{
    SendSystemTaggedMessage(g_wcConfig.groupTag, std::string(ChatColor::GROUP), msg, -1, false);
}

void WC::Broadcast(MessageChannel channel, std::string const& msg)
{
    std::string tag;
    std::string color;
    int team;
    bool gmOnly;

    GetApiTagAndColor(channel, tag, color, team, gmOnly);
    SendSystemTaggedMessage(tag, color, msg, team, gmOnly);
}

void WC::SendToPlayer(Player* player, MessageChannel channel, std::string const& msg)
{
    if (!player || !player->IsInWorld() || msg.empty())
        return;

    std::string tag;
    std::string color;
    int team;
    bool gmOnly;

    GetApiTagAndColor(channel, tag, color, team, gmOnly);

    if (gmOnly && player->GetSession()->GetSecurity() == SEC_PLAYER)
        return;

    ChatHandler(player->GetSession()).PSendSysMessage(
        "{}{}|r {}{}|r",
        color,
        tag,
        WC::ChatColor::TEXT,
        msg
    );
}

void WC::SendToFaction(uint32 teamId, MessageChannel channel, std::string const& msg)
{
    std::string tag;
    std::string color;
    int ignoredTeam;
    bool gmOnly;

    GetApiTagAndColor(channel, tag, color, ignoredTeam, gmOnly);
    SendSystemTaggedMessage(tag, color, msg, teamId, gmOnly);
}

void WC::SendToGMs(MessageChannel channel, std::string const& msg)
{
    std::string tag;
    std::string color;
    int team;
    bool gmOnly;

    GetApiTagAndColor(channel, tag, color, team, gmOnly);
    SendSystemTaggedMessage(tag, color, msg, -1, true);
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

bool WC::WorldChat_Announce::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Channel* channel)
{
    if (!player || !channel)
        return true;

    if (g_wcConfig.channelName != "" && lang != LANG_ADDON && strToLower(channel->GetName()) == g_wcConfig.channelName)
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

    handler->PSendSysMessage("{}[World]|r {}World Chat is now visible.|r", ChatColor::WORLD, ChatColor::GREEN);
    return true;
}

bool WC::WorldChat::HandleWorldChatOffCommand(ChatHandler* handler)
{
    Player* player = handler->GetSession()->GetPlayer();
    WC::PlayerState& state = GetState(player);

    state.receiveEnabled = false;
    state.worldMode = false;
    SavePlayerState(player);

    handler->PSendSysMessage("{}[World]|r {}World Chat is now hidden.|r", ChatColor::WORLD, ChatColor::YELLOW);
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
