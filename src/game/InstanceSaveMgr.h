/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
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

#ifndef __InstanceSaveMgr_H
#define __InstanceSaveMgr_H

#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "ace/Thread_Mutex.h"
#include <list>
#include <map>
#include "Utilities/UnorderedMapSet.h"
#include "Database/DatabaseEnv.h"
#include "DBCEnums.h"
#include "ObjectGuid.h"

struct InstanceTemplate;
struct MapEntry;
class Player;
class Group;

/*
    Holds the information necessary for creating a new map for an existing instance
    Is referenced in three cases:
    - player-instance binds for solo players (not in group)
    - player-instance binds for permanent heroic/raid saves
    - group-instance binds (both solo and permanent) cache the player binds for the group leader
*/
class InstanceSave
{
    public:
        /* Created either when:
           - any new instance is being generated
           - the first time a player bound to InstanceId logs in
           - when a group bound to the instance is loaded */
        InstanceSave(uint16 MapId, uint32 InstanceId, Difficulty difficulty, time_t resetTime, bool canReset);

        /* Unloaded when m_playerList and m_groupList become empty
           or when the instance is reset */
        ~InstanceSave();

        uint8 GetPlayerCount() { return m_playerList.size(); }
        uint8 GetGroupCount() { return m_groupList.size(); }

        /* A map corresponding to the InstanceId/MapId does not always exist.
        InstanceSave objects may be created on player logon but the maps are
        created and loaded only when a player actually enters the instance. */
        uint32 GetInstanceId() { return m_instanceid; }
        uint32 GetMapId() { return m_mapid; }

        /* Saved when the instance is generated for the first time */
        void SaveToDB();
        /* When the instance is being reset (permanently deleted) */
        void DeleteFromDB();

        /* for normal instances this corresponds to max(creature respawn time) + X hours
           for raid/heroic instances this caches the global respawn time for the map */
        time_t GetResetTime() { return m_resetTime; }
        void SetResetTime(time_t resetTime) { m_resetTime = resetTime; }
        time_t GetResetTimeForDB();

        InstanceTemplate const* GetTemplate();
        MapEntry const* GetMapEntry();

        /* online players bound to the instance (perm/solo)
           does not include the members of the group unless they have permanent saves */
        void AddPlayer(Player *player) { m_playerList.push_back(player); }
        bool RemovePlayer(Player *player) { m_playerList.remove(player); return UnloadIfEmpty(); }
        /* all groups bound to the instance */
        void AddGroup(Group *group) { m_groupList.push_back(group); }
        bool RemoveGroup(Group *group) { m_groupList.remove(group); return UnloadIfEmpty(); }

        /* instances cannot be reset (except at the global reset time)
           if there are players permanently bound to it
           this is cached for the case when those players are offline */
        bool CanReset() { return m_canReset; }
        void SetCanReset(bool canReset) { m_canReset = canReset; }

        /* currently it is possible to omit this information from this structure
           but that would depend on a lot of things that can easily change in future */
        Difficulty GetDifficulty() { return m_difficulty; }

        void SetUsedByMapState(bool state)
        {
            m_usedByMap = state;
            if (!state)
                UnloadIfEmpty();
        }

    private:
        typedef std::list<Player*> PlayerListType;
        typedef std::list<Group*> GroupListType;

        bool UnloadIfEmpty();
        /* the only reason the instSave-object links are kept is because
           the object-instSave links need to be broken at reset time
           TODO: maybe it's enough to just store the number of players/groups */
        PlayerListType m_playerList;
        GroupListType m_groupList;
        time_t m_resetTime;
        uint32 m_instanceid;
        uint32 m_mapid;
        Difficulty m_difficulty;
        bool m_canReset;
        bool m_usedByMap;                                   // true when instance map loaded
};

enum ResetEventType
{
    RESET_EVENT_DUNGEON      = 0,                           // no fixed reset time
    RESET_EVENT_INFORM_1     = 1,                           // raid/heroic warnings
    RESET_EVENT_INFORM_2     = 2,
    RESET_EVENT_INFORM_3     = 3,
    RESET_EVENT_INFORM_LAST  = 4,
};

#define MAX_RESET_EVENT_TYPE   5

/* resetTime is a global propery of each (raid/heroic) map
    all instances of that map reset at the same time */
struct InstanceResetEvent
{
    ResetEventType type   :8;                               // if RESET_EVENT_DUNGEON then InstanceID == 0 and applied to all instances for pair (map,diff)
    Difficulty difficulty :8;                               // used with mapid used as for select reset for global cooldown instances (instamceid==0 for event)
    uint16 mapid;
    uint32 instanceId;                                      // used for select reset for normal dungeons

    InstanceResetEvent() : type(RESET_EVENT_DUNGEON), difficulty(DUNGEON_DIFFICULTY_NORMAL), mapid(0), instanceId(0) {}
    InstanceResetEvent(ResetEventType t, uint32 _mapid, Difficulty d, uint32 _instanceid)
        : type(t), difficulty(d), mapid(_mapid), instanceId(_instanceid) {}
    bool operator == (const InstanceResetEvent& e) { return e.mapid == mapid && e.difficulty == difficulty && e.instanceId == instanceId; }
};

class InstanceSaveManager;

class InstanceResetScheduler
{
    public:                                                 // constructors
        explicit InstanceResetScheduler(InstanceSaveManager& mgr) : m_InstanceSaves(mgr) {}
        void LoadResetTimes();

    public:                                                 // accessors
        time_t GetResetTimeFor(uint32 mapid, Difficulty d) const
        {
            ResetTimeByMapDifficultyMap::const_iterator itr  = m_resetTimeByMapDifficulty.find(MAKE_PAIR32(mapid,d));
            return itr != m_resetTimeByMapDifficulty.end() ? itr->second : 0;
        }

        static uint32 GetMaxResetTimeFor(MapDifficulty const* mapDiff);

    public:                                                 // modifiers
        void SetResetTimeFor(uint32 mapid, Difficulty d, time_t t)
        {
            m_resetTimeByMapDifficulty[MAKE_PAIR32(mapid,d)] = t;
        }

        void ScheduleReset(bool add, time_t time, InstanceResetEvent event);

        void Update();

    private:                                                // fields
        InstanceSaveManager& m_InstanceSaves;


        // fast lookup for reset times (always use existing functions for access/set)
        typedef UNORDERED_MAP<uint32 /*PAIR32(map,difficulty)*/,time_t /*resetTime*/> ResetTimeByMapDifficultyMap;
        ResetTimeByMapDifficultyMap m_resetTimeByMapDifficulty;

        typedef std::multimap<time_t /*resetTime*/, InstanceResetEvent> ResetTimeQueue;
        ResetTimeQueue m_resetTimeQueue;
};

class MANGOS_DLL_DECL InstanceSaveManager : public MaNGOS::Singleton<InstanceSaveManager, MaNGOS::ClassLevelLockable<InstanceSaveManager, ACE_Thread_Mutex> >
{
    friend class InstanceResetScheduler;
    public:
        InstanceSaveManager();
        ~InstanceSaveManager();

        void CleanupInstances();
        void PackInstances();

        InstanceResetScheduler& GetScheduler() { return m_Scheduler; }

        InstanceSave* AddInstanceSave(uint32 mapId, uint32 instanceId, Difficulty difficulty, time_t resetTime, bool canReset, bool load = false);
        void RemoveInstanceSave(uint32 InstanceId);
        static void DeleteInstanceFromDB(uint32 instanceid);

        /* statistics */
        uint32 GetNumInstanceSaves() { return m_instanceSaveById.size(); }
        uint32 GetNumBoundPlayersTotal();
        uint32 GetNumBoundGroupsTotal();

        void Update() { m_Scheduler.Update(); }
    private:
        typedef UNORDERED_MAP<uint32 /*InstanceId*/, InstanceSave*> InstanceSaveHashMap;
        typedef UNORDERED_MAP<uint32 /*mapId*/, InstanceSaveHashMap> InstanceSaveMapMap;

        InstanceSave *GetInstanceSave(uint32 InstanceId);

        //  called by scheduler
        void _ResetOrWarnAll(uint32 mapid, Difficulty difficulty, bool warn, uint32 timeleft);
        void _ResetInstance(uint32 mapid, uint32 instanceId);
        void _CleanupExpiredInstancesAtTime(time_t t);

        void _ResetSave(InstanceSaveHashMap::iterator &itr);
        void _DelHelper(DatabaseType &db, const char *fields, const char *table, const char *queryTail,...);

        // used during global instance resets
        bool lock_instLists;
        // fast lookup by instance id
        InstanceSaveHashMap m_instanceSaveById;

        InstanceResetScheduler m_Scheduler;
};

#define sInstanceSaveMgr MaNGOS::Singleton<InstanceSaveManager>::Instance()
#endif
