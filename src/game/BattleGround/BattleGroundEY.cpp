/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Object.h"
#include "Player.h"
#include "BattleGround.h"
#include "BattleGroundEY.h"
#include "Creature.h"
#include "ObjectMgr.h"
#include "BattleGroundMgr.h"
#include "Language.h"
#include "WorldPacket.h"
#include "Util.h"
#include "MapManager.h"
#include "GameEventMgr.h"
#include "ObjectMgr.h"

BattleGroundEY::BattleGroundEY()
{
    m_BuffChange = true;
    m_BgObjects.resize(EY_OBJECT_MAX);

    m_StartMessageIds[BG_STARTING_EVENT_FIRST]  = 0;
    m_StartMessageIds[BG_STARTING_EVENT_SECOND] = LANG_BG_EY_START_ONE_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_THIRD] = LANG_BG_EY_START_HALF_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_FOURTH] = LANG_BG_EY_HAS_BEGUN;
}

BattleGroundEY::~BattleGroundEY()
{
}

void BattleGroundEY::Update(uint32 diff)
{
    BattleGround::Update(diff);

    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    // resource counter
    if (m_resourceUpdateTimer < diff)
    {
        UpdateResources();
        m_resourceUpdateTimer = EY_RESOURCES_UPDATE_TIME;
    }
    else
        m_resourceUpdateTimer -= diff;

    // flag respawn
    if (m_flagState == EY_FLAG_STATE_WAIT_RESPAWN || m_flagState == EY_FLAG_STATE_ON_GROUND)
    {
        if (m_flagRespawnTimer < diff)
        {
            m_flagRespawnTimer = 0;
            if (m_flagState == EY_FLAG_STATE_WAIT_RESPAWN)
                RespawnFlag();
            else
                RespawnDroppedFlag();
        }
        else
            m_flagRespawnTimer -= diff;
    }
}

void BattleGroundEY::StartingEventCloseDoors()
{
}

void BattleGroundEY::StartingEventOpenDoors()
{
    // eye-doors are despawned, not opened
    SpawnEvent(BG_EVENT_DOOR, 0, false);

    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
    {
        //randomly spawn buff
        uint8 buff = urand(0, 2);
        SpawnBGObject(m_BgObjects[EY_OBJECT_SPEEDBUFF_FEL_REAVER_RUINS + buff + i * 3], RESPAWN_IMMEDIATELY);
    }

    // Players that join battleground after start are not eligible to get achievement.
    StartTimedAchievement(ACHIEVEMENT_CRITERIA_TYPE_WIN_BG, EY_EVENT_START_BATTLE);
}

void BattleGroundEY::AddPoints(Team team, uint32 points)
{
    TeamIndex team_index = GetTeamIndex(team);
    m_TeamScores[team_index] += points;
    m_honorScoreTicks[team_index] += points;
    m_ExperienceTics[team_index] += points;
    if (m_honorScoreTicks[team_index] >= m_honorTicks)
    {
        RewardHonorToTeam(GetBonusHonorFromKill(1), team);
        m_honorScoreTicks[team_index] -= m_honorTicks;
    }
    if (m_ExperienceTics[team_index] >= BG_EY_ExperienceTicks)
    {
        RewardXpToTeam(0, 0.8f, team);
        m_ExperienceTics[team_index] -= BG_EY_ExperienceTicks;
    }
}

void BattleGroundEY::UpdateResources()
{
    if (m_towersAlliance > 0)
    {
        AddPoints(ALLIANCE, eyTickPoints[m_towersAlliance - 1]);
        UpdateTeamScore(ALLIANCE);
    }
    if (m_towersHorde > 0)
    {
        AddPoints(HORDE, eyTickPoints[m_towersHorde - 1]);
        UpdateTeamScore(HORDE);
    }
}

void BattleGroundEY::UpdateTeamScore(Team team)
{
    uint32 score = m_TeamScores[GetTeamIndex(team)];

    if (score >= EY_MAX_TEAM_SCORE)
    {
        score = EY_MAX_TEAM_SCORE;
        EndBattleGround(team);
    }

    if (team == ALLIANCE)
        UpdateWorldState(WORLD_STATE_EY_RESOURCES_ALLIANCE, score);
    else
        UpdateWorldState(WORLD_STATE_EY_RESOURCES_HORDE, score);
}

void BattleGroundEY::EndBattleGround(Team winner)
{
    //win reward
    if (winner == ALLIANCE)
    {
        RewardHonorToTeam(GetBonusHonorFromKill(1), ALLIANCE);
        RewardXpToTeam(0, 0.8f, winner);
    }
    if (winner == HORDE)
    {
        RewardHonorToTeam(GetBonusHonorFromKill(1), HORDE);
        RewardXpToTeam(0, 0.8f, winner);
    }
    //complete map reward
    RewardHonorToTeam(GetBonusHonorFromKill(1), ALLIANCE);
    RewardHonorToTeam(GetBonusHonorFromKill(1), HORDE);
    RewardXpToTeam(0, 0.8f, ALLIANCE);
    RewardXpToTeam(0, 0.8f, HORDE);

    // disable capture points
    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
        if (GameObject* go = GetBgMap()->GetGameObject(m_towers[i]))
            go->SetLootState(GO_JUST_DEACTIVATED);

    BattleGround::EndBattleGround(winner);
}

void BattleGroundEY::AddPlayer(Player* plr)
{
    BattleGround::AddPlayer(plr);
    // create score and add it to map
    BattleGroundEYScore* sc = new BattleGroundEYScore;

    m_PlayerScores[plr->GetObjectGuid()] = sc;
}

void BattleGroundEY::RemovePlayer(Player* plr, ObjectGuid guid)
{
    // sometimes flag aura not removed :(
    if (IsFlagPickedUp())
    {
        if (m_flagCarrier == guid)
        {
            if (plr)
                EventPlayerDroppedFlag(plr);
            else
            {
                ClearFlagCarrier();
                RespawnFlag();
            }
        }
    }
}

void BattleGroundEY::HandleGameObjectCreate(GameObject* go)
{
    // set initial data and activate capture points
    switch (go->GetEntry())
    {
        case GO_CAPTURE_POINT_BLOOD_ELF_TOWER:
            m_towers[NODE_BLOOD_ELF_TOWER] = go->GetObjectGuid();
            go->SetCapturePointSlider(CAPTURE_SLIDER_MIDDLE);
            break;
        case GO_CAPTURE_POINT_FEL_REAVER_RUINS:
            m_towers[NODE_FEL_REAVER_RUINS] = go->GetObjectGuid();
            go->SetCapturePointSlider(CAPTURE_SLIDER_MIDDLE);
            break;
        case GO_CAPTURE_POINT_MAGE_TOWER:
            m_towers[NODE_MAGE_TOWER] = go->GetObjectGuid();
            go->SetCapturePointSlider(CAPTURE_SLIDER_MIDDLE);
            break;
        case GO_CAPTURE_POINT_DRAENEI_RUINS:
            m_towers[NODE_DRAENEI_RUINS] = go->GetObjectGuid();
            go->SetCapturePointSlider(CAPTURE_SLIDER_MIDDLE);
            break;
    }
}

// process the capture events
bool BattleGroundEY::HandleEvent(uint32 eventId, GameObject* go)
{
    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
    {
        if (eyTowers[i] == go->GetEntry())
        {
            for (uint8 j = 0; j < 4; ++j)
            {
                if (eyTowerEvents[i][j].eventEntry == eventId)
                {
                    ProcessCaptureEvent(go, i, eyTowerEvents[i][j].team, eyTowerEvents[i][j].worldState, eyTowerEvents[i][j].message);

                    // no need to iterate other events or towers
                    return false;
                }
            }
            // no need to iterate other towers
            return false;
        }
    }

    return false;
}

void BattleGroundEY::ProcessCaptureEvent(GameObject* go, uint32 towerId, Team team, uint32 newWorldState, uint32 message)
{
    if (team == ALLIANCE)
    {
        // update counter
        ++m_towersAlliance;
        UpdateWorldState(WORLD_STATE_EY_TOWER_COUNT_ALLIANCE, m_towersAlliance);

        SendMessageToAll(message, CHAT_MSG_BG_SYSTEM_ALLIANCE);

        // spawn gameobjects
        SpawnEvent(towerId, TEAM_INDEX_ALLIANCE, true);
    }
    else if (team == HORDE)
    {
        // update counter
        ++m_towersHorde;
        UpdateWorldState(WORLD_STATE_EY_TOWER_COUNT_HORDE, m_towersHorde);

        SendMessageToAll(message, CHAT_MSG_BG_SYSTEM_HORDE);

        // spawn gameobjects
        SpawnEvent(towerId, TEAM_INDEX_HORDE, true);
    }
    else
    {
        if (m_towerOwner[towerId] == ALLIANCE)
        {
            // update counter
            --m_towersAlliance;
            UpdateWorldState(WORLD_STATE_EY_TOWER_COUNT_ALLIANCE, m_towersAlliance);

            SendMessageToAll(message, CHAT_MSG_BG_SYSTEM_ALLIANCE);
        }
        else
        {
            // update counter
            --m_towersHorde;
            UpdateWorldState(WORLD_STATE_EY_TOWER_COUNT_HORDE, m_towersHorde);

            SendMessageToAll(message, CHAT_MSG_BG_SYSTEM_HORDE);
        }

        // despawn gameobjects
        SpawnEvent(towerId, EY_NEUTRAL_TEAM, true);
    }

    // update tower state
    UpdateWorldState(m_towerWorldState[towerId], WORLD_STATE_REMOVE);
    m_towerWorldState[towerId] = newWorldState;
    UpdateWorldState(m_towerWorldState[towerId], WORLD_STATE_ADD);

    // update capture point owner
    m_towerOwner[towerId] = team;
}

void BattleGroundEY::HandleAreaTrigger(Player* source, uint32 Trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (!source->isAlive())                                  //hack code, must be removed later
        return;

    switch (Trigger)
    {
        case AREATRIGGER_BLOOD_ELF_TOWER_POINT:
            if (m_towerOwner[NODE_BLOOD_ELF_TOWER] == source->GetTeam())
                EventPlayerCapturedFlag(source, NODE_BLOOD_ELF_TOWER);
            break;
        case AREATRIGGER_FEL_REAVER_RUINS_POINT:
            if (m_towerOwner[NODE_FEL_REAVER_RUINS] == source->GetTeam())
                EventPlayerCapturedFlag(source, NODE_FEL_REAVER_RUINS);
            break;
        case AREATRIGGER_MAGE_TOWER_POINT:
            if (m_towerOwner[NODE_MAGE_TOWER] == source->GetTeam())
                EventPlayerCapturedFlag(source, NODE_MAGE_TOWER);
            break;
        case AREATRIGGER_DRAENEI_RUINS_POINT:
            if (m_towerOwner[NODE_DRAENEI_RUINS] == source->GetTeam())
                EventPlayerCapturedFlag(source, NODE_DRAENEI_RUINS);
            break;
    }
}

bool BattleGroundEY::SetupBattleGround()
{
    // buffs
    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
    {
        AreaTriggerEntry const* at = sAreaTriggerStore.LookupEntry(eyTriggers[i]);
        if (!at)
        {
            sLog.outError("BattleGroundEY: Unknown trigger: %u", eyTriggers[i]);
            continue;
        }
        if (!AddObject(EY_OBJECT_SPEEDBUFF_FEL_REAVER_RUINS + i * 3, Buff_Entries[0], at->x, at->y, at->z, 0.907571f, QuaternionData(0, 0, 0.438371f, 0.898794f), RESPAWN_ONE_DAY)
                || !AddObject(EY_OBJECT_SPEEDBUFF_FEL_REAVER_RUINS + i * 3 + 1, Buff_Entries[1], at->x, at->y, at->z, 0.907571f, QuaternionData(0, 0, 0.438371f, 0.898794f), RESPAWN_ONE_DAY)
                || !AddObject(EY_OBJECT_SPEEDBUFF_FEL_REAVER_RUINS + i * 3 + 2, Buff_Entries[2], at->x, at->y, at->z, 0.907571f, QuaternionData(0, 0, 0.438371f, 0.898794f), RESPAWN_ONE_DAY))
            sLog.outError("BattleGroundEY: Cannot spawn buff");
    }

    return true;
}

void BattleGroundEY::Reset()
{
    //call parent's class reset
    BattleGround::Reset();

    m_TeamScores[TEAM_INDEX_ALLIANCE] = 0;
    m_TeamScores[TEAM_INDEX_HORDE] = 0;

    m_towersAlliance = 0;
    m_towersHorde = 0;

    m_honorTicks = BattleGroundMgr::IsBGWeekend(GetTypeID()) ? EY_WEEKEND_HONOR_INTERVAL : EY_NORMAL_HONOR_INTERVAL;
    m_honorScoreTicks[TEAM_INDEX_ALLIANCE] = 0;
    m_honorScoreTicks[TEAM_INDEX_HORDE] = 0;
    m_ExperienceTics[TEAM_INDEX_ALLIANCE] = 0;
    m_ExperienceTics[TEAM_INDEX_HORDE] = 0;

    m_flagState = EY_FLAG_STATE_ON_BASE;
    m_flagCarrier.Clear();
    m_DroppedFlagGuid.Clear();

    m_flagRespawnTimer = 0;
    m_resourceUpdateTimer = 0;

    m_towerWorldState[NODE_BLOOD_ELF_TOWER] = WORLD_STATE_EY_BLOOD_ELF_TOWER_NEUTRAL;
    m_towerWorldState[NODE_FEL_REAVER_RUINS] = WORLD_STATE_EY_FEL_REAVER_RUINS_NEUTRAL;
    m_towerWorldState[NODE_MAGE_TOWER] = WORLD_STATE_EY_MAGE_TOWER_NEUTRAL;
    m_towerWorldState[NODE_DRAENEI_RUINS] = WORLD_STATE_EY_DRAENEI_RUINS_NEUTRAL;

    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
    {
        m_towerOwner[i] = TEAM_NONE;
        m_ActiveEvents[i] = EY_NEUTRAL_TEAM;
    }

    // the flag in the middle is spawned at beginning
    m_ActiveEvents[EY_EVENT_CAPTURE_FLAG] = EY_EVENT2_FLAG_CENTER;
}

void BattleGroundEY::RespawnFlag()
{
    m_flagState = EY_FLAG_STATE_ON_BASE;
    // will despawn captured flags at the node and spawn in center
    SpawnEvent(EY_EVENT_CAPTURE_FLAG, EY_EVENT2_FLAG_CENTER, true);

    PlaySoundToAll(EY_SOUND_FLAG_RESET);
    SendMessageToAll(LANG_BG_EY_RESETED_FLAG, CHAT_MSG_BG_SYSTEM_NEUTRAL);

    UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_READY, WORLD_STATE_ADD);
}

void BattleGroundEY::RespawnDroppedFlag()
{
    RespawnFlag();

    GameObject *obj = GetBgMap()->GetGameObject(GetDroppedFlagGuid());
    if (obj)
        obj->Delete();
    else
        ERROR_LOG("BattleGroundEY: Unknown dropped flag: %s", GetDroppedFlagGuid().GetString().c_str());

    ClearDroppedFlagGuid();
}

void BattleGroundEY::HandleKillPlayer(Player *player, Player *killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattleGround::HandleKillPlayer(player, killer);
    EventPlayerDroppedFlag(player);
}

void BattleGroundEY::EventPlayerDroppedFlag(Player* source)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
    {
        // if not running, do not cast things at the dropper player, neither send unnecessary messages
        // just take off the aura
        if (IsFlagPickedUp() && GetFlagCarrierGuid() == source->GetObjectGuid())
        {
            ClearFlagCarrier();
            source->RemoveAurasDueToSpell(EY_NETHERSTORM_FLAG_SPELL);
        }
        return;
    }

    if (!IsFlagPickedUp())
        return;

    if (GetFlagCarrierGuid() != source->GetObjectGuid())
        return;

    ClearFlagCarrier();
    source->RemoveAurasDueToSpell(EY_NETHERSTORM_FLAG_SPELL);
    m_flagState = EY_FLAG_STATE_ON_GROUND;
    m_flagRespawnTimer = EY_FLAG_RESPAWN_TIME;
    source->CastSpell(source, SPELL_RECENTLY_DROPPED_FLAG, true);
    source->CastSpell(source, EY_PLAYER_DROPPED_FLAG_SPELL, true);

    if (source->GetTeam() == ALLIANCE)
    {
        UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_ALLIANCE, 1);
        SendMessageToAll(LANG_BG_EY_DROPPED_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, NULL);
    }
    else
    {
        UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_HORDE, 1);
        SendMessageToAll(LANG_BG_EY_DROPPED_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, NULL);
    }
}

void BattleGroundEY::EventPlayerClickedOnFlag(Player* source, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS || IsFlagPickedUp() || !source->IsWithinDistInMap(target_obj, 10))
        return;

    if (m_flagState == EY_FLAG_STATE_ON_BASE)
        UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_READY, WORLD_STATE_REMOVE);

    if (source->GetTeam() == ALLIANCE)
    {
        PlaySoundToAll(EY_SOUND_FLAG_PICKED_UP_ALLIANCE);

        m_flagState = EY_FLAG_STATE_ON_ALLIANCE_PLAYER;
        UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_ALLIANCE, 2);
    }
    else
    {
        PlaySoundToAll(EY_SOUND_FLAG_PICKED_UP_HORDE);

        m_flagState = EY_FLAG_STATE_ON_HORDE_PLAYER;
        UpdateWorldState(WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_HORDE, 2);
    }

    // despawn center-flag
    SpawnEvent(EY_EVENT_CAPTURE_FLAG, EY_EVENT2_FLAG_CENTER, false);

    SetFlagCarrier(source->GetObjectGuid());
    // get flag aura on player
    source->CastSpell(source, EY_NETHERSTORM_FLAG_SPELL, true);
    source->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);

    if (source->GetTeam() == ALLIANCE)
        PSendMessageToAll(LANG_BG_EY_HAS_TAKEN_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, NULL, source->GetName());
    else
        PSendMessageToAll(LANG_BG_EY_HAS_TAKEN_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, NULL, source->GetName());
}

void BattleGroundEY::EventPlayerCapturedFlag(Player* source, EYNodes node)
{
    if (GetStatus() != STATUS_IN_PROGRESS || GetFlagCarrierGuid() != source->GetObjectGuid())
        return;

    ClearFlagCarrier();

    m_flagState = EY_FLAG_STATE_WAIT_RESPAWN;
    m_flagRespawnTimer = EY_FLAG_RESPAWN_TIME;

    source->RemoveAurasDueToSpell(EY_NETHERSTORM_FLAG_SPELL);
    source->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);

    if (source->GetTeam() == ALLIANCE)
    {
        PlaySoundToAll(EY_SOUND_FLAG_CAPTURED_ALLIANCE);

        if (m_towersAlliance > 0)
            AddPoints(ALLIANCE, eyFlagPoints[m_towersAlliance - 1]);

        SendMessageToAll(LANG_BG_EY_CAPTURED_FLAG_A, CHAT_MSG_BG_SYSTEM_ALLIANCE, source);
    }
    else
    {
        PlaySoundToAll(EY_SOUND_FLAG_CAPTURED_HORDE);

        if (m_towersHorde > 0)
            AddPoints(HORDE, eyFlagPoints[m_towersHorde - 1]);

        SendMessageToAll(LANG_BG_EY_CAPTURED_FLAG_H, CHAT_MSG_BG_SYSTEM_HORDE, source);
    }

    SpawnEvent(EY_EVENT_CAPTURE_FLAG, node, true);

    RewardXpToTeam(0, 0.6f, source->GetTeam());

    UpdatePlayerScore(source, SCORE_FLAG_CAPTURES, 1);
}

void BattleGroundEY::UpdatePlayerScore(Player *Source, uint32 type, uint32 value)
{
    BattleGroundScoreMap::iterator itr = m_PlayerScores.find(Source->GetObjectGuid());
    if (itr == m_PlayerScores.end())                        // player not found
        return;

    switch(type)
    {
        case SCORE_FLAG_CAPTURES:                           // flags captured
            ((BattleGroundEYScore*)itr->second)->FlagCaptures += value;
            Source->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, 1, EY_OBJECTIVE_CAPTURE_FLAG);
            break;
        default:
            BattleGround::UpdatePlayerScore(Source, type, value);
            break;
    }
}

void BattleGroundEY::FillInitialWorldStates(WorldPacket& data, uint32& count)
{
    // counter states
    FillInitialWorldState(data, count, WORLD_STATE_EY_TOWER_COUNT_ALLIANCE, m_towersAlliance);
    FillInitialWorldState(data, count, WORLD_STATE_EY_TOWER_COUNT_HORDE, m_towersHorde);

    FillInitialWorldState(data, count, WORLD_STATE_EY_RESOURCES_ALLIANCE, m_TeamScores[TEAM_INDEX_ALLIANCE]);
    FillInitialWorldState(data, count, WORLD_STATE_EY_RESOURCES_HORDE, m_TeamScores[TEAM_INDEX_HORDE]);

    // tower world states
    FillInitialWorldState(data, count, WORLD_STATE_EY_BLOOD_ELF_TOWER_ALLIANCE, m_towerOwner[NODE_BLOOD_ELF_TOWER] == ALLIANCE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_BLOOD_ELF_TOWER_HORDE, m_towerOwner[NODE_BLOOD_ELF_TOWER] == HORDE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_BLOOD_ELF_TOWER_NEUTRAL, m_towerOwner[NODE_BLOOD_ELF_TOWER] == TEAM_NONE);

    FillInitialWorldState(data, count, WORLD_STATE_EY_FEL_REAVER_RUINS_ALLIANCE, m_towerOwner[NODE_FEL_REAVER_RUINS] == ALLIANCE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_FEL_REAVER_RUINS_HORDE, m_towerOwner[NODE_FEL_REAVER_RUINS] == HORDE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_FEL_REAVER_RUINS_NEUTRAL, m_towerOwner[NODE_FEL_REAVER_RUINS] == TEAM_NONE);

    FillInitialWorldState(data, count, WORLD_STATE_EY_MAGE_TOWER_ALLIANCE, m_towerOwner[NODE_MAGE_TOWER] == ALLIANCE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_MAGE_TOWER_HORDE, m_towerOwner[NODE_MAGE_TOWER] == HORDE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_MAGE_TOWER_NEUTRAL, m_towerOwner[NODE_MAGE_TOWER] == TEAM_NONE);

    FillInitialWorldState(data, count, WORLD_STATE_EY_DRAENEI_RUINS_ALLIANCE, m_towerOwner[NODE_DRAENEI_RUINS] == ALLIANCE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_DRAENEI_RUINS_HORDE, m_towerOwner[NODE_DRAENEI_RUINS] == HORDE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_DRAENEI_RUINS_NEUTRAL, m_towerOwner[NODE_DRAENEI_RUINS] == TEAM_NONE);

    // flag states
    FillInitialWorldState(data, count, WORLD_STATE_EY_NETHERSTORM_FLAG_READY, m_flagState == EY_FLAG_STATE_ON_BASE);
    FillInitialWorldState(data, count, WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_ALLIANCE, m_flagState == EY_FLAG_STATE_ON_ALLIANCE_PLAYER ? 2 : 1);
    FillInitialWorldState(data, count, WORLD_STATE_EY_NETHERSTORM_FLAG_STATE_HORDE, m_flagState == EY_FLAG_STATE_ON_HORDE_PLAYER ? 2 : 1);

    // capture point states
    // if you leave the bg while being in capture point radius - and later join same type of bg the slider would still be displayed because the client caches it
    FillInitialWorldState(data, count, WORLD_STATE_EY_CAPTURE_POINT_SLIDER_DISPLAY, WORLD_STATE_REMOVE);
}

WorldSafeLocsEntry const *BattleGroundEY::GetClosestGraveYard(Player* player)
{
    uint32 g_id = 0;

    switch(player->GetTeam())
    {
        case ALLIANCE: g_id = GRAVEYARD_EY_MAIN_ALLIANCE; break;
        case HORDE:    g_id = GRAVEYARD_EY_MAIN_HORDE;    break;
        default:       return NULL;
    }

    float distance, nearestDistance;

    WorldSafeLocsEntry const* entry = sWorldSafeLocsStore.LookupEntry(g_id);
    WorldSafeLocsEntry const* nearestEntry = entry;

    if (!entry)
    {
        ERROR_LOG("BattleGroundEY: Not found the main team graveyard. Graveyard system isn't working!");
        return NULL;
    }

    float plr_x = player->GetPositionX();
    float plr_y = player->GetPositionY();
    float plr_z = player->GetPositionZ();

    distance = (entry->x - plr_x) * (entry->x - plr_x) + (entry->y - plr_y) * (entry->y - plr_y) + (entry->z - plr_z) * (entry->z - plr_z);
    nearestDistance = distance;

    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
    {
        if (m_towerOwner[i] == player->GetTeam())
        {
            entry = sWorldSafeLocsStore.LookupEntry(eyGraveyards[i]);
            if (!entry)
                sLog.outError("BattleGroundEY: Not found graveyard: %u", eyGraveyards[i]);
            else
            {
                distance = (entry->x - plr_x)*(entry->x - plr_x) + (entry->y - plr_y)*(entry->y - plr_y) + (entry->z - plr_z)*(entry->z - plr_z);
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearestEntry = entry;
                }
            }
        }
    }

    return nearestEntry;
}

bool BattleGroundEY::IsAllNodesControlledByTeam(Team team) const
{
    for (uint8 i = 0; i < EY_NODES_MAX; ++i)
        if (m_towerOwner[i] != team)
            return false;

    return true;
}

uint32 BattleGroundEY::GetPlayerScore(Player *Source, uint32 type)
{
    BattleGroundScoreMap::const_iterator itr = m_PlayerScores.find(Source->GetObjectGuid());

    if (itr == m_PlayerScores.end())                         // player not found...
        return 0;

    switch(type)
    {
        case SCORE_FLAG_CAPTURES:
            return ((BattleGroundEYScore*)itr->second)->FlagCaptures;
    }

    return BattleGround::GetPlayerScore(Source, type);
}

void BattleGroundEY::CheckBuggers()
{
    if (GetStatus() == STATUS_WAIT_JOIN)
    {
        for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* plr = sObjectMgr.GetPlayer(itr->first))
            {
                if (plr->isGameMaster() || plr->IsBeingTeleported())
                    continue;

                // horde buggers
                if (plr->GetPositionX() < 2160.0f)
                {
                    if (plr->GetDistance2d(1807.7f, 1539.41f) > 15.0f)
                        plr->TeleportTo(GetMapId(), 1807.74f, 1539.41f, 1268.62f, plr->GetOrientation());
                }
                // alliance buggers
                else
                {
                    if (plr->GetDistance2d(2523.68f, 1596.59f) > 15.0f)
                        plr->TeleportTo(GetMapId(), 2523.68f, 1596.59f, 1269.34f, plr->GetOrientation());
                }
            }
        }
    }
}
