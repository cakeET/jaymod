#include <bgame/impl.h>

#include <omnibot/et/g_etbot_interface.h>

/*
===============================================================================

Antiwarp

===============================================================================
*/
void G_AddUsercmd( int clientNum, usercmd_t *cmd )
{
    Client& client = g_clientObjects[clientNum];

    client.cmdQueue.push_back(*cmd);
	client.cmdCount++;
}

static float G_CmdScale( gentity_t *ent, usercmd_t *cmd )
{
	float	scale;

	scale = abs( cmd->forwardmove );
	if ( abs( cmd->rightmove ) > scale )
		scale = abs( cmd->rightmove );
	// zinx - don't count crouch/jump; just count moving in water
	if ( ent->waterlevel && abs( cmd->upmove ) > scale )
		scale = abs( cmd->upmove );

	scale /= 127.f;

	return scale;
}

static void G_ProcessAntiwarpQueue( gentity_t *ent )
{
    Client& client = g_clientObjects[ent->s.number];

	usercmd_t lastCmd;

    int lastTime;
	int latestTime;
	int drop_threshold = LAG_MAX_DROP_THRESHOLD;
	int startPackets = client.cmdCount;

    // Nothing to do
	if ( client.cmdCount <= 0 )
		return;

	// allow some more movement if time has passed
	latestTime = trap_Milliseconds();
	if ( client.cmdLastRealTime > latestTime ) {
		// zinx - stoopid server went backwards in time, reset the delta
		// instead of giving them even -less- movement ability
		client.cmdDelta = 0;
	} else {
		client.cmdDelta -= (latestTime - client.cmdLastRealTime);
	}
	if ( client.cmdCount <= 1 && client.cmdDelta < 0 )
		client.cmdDelta = 0;
	client.cmdLastRealTime = latestTime;

    lastCmd = client.cmdQueue.back();

	lastTime = ent->client->ps.commandTime;
	latestTime = lastCmd.serverTime;

	while ( client.cmdCount > 0 ) {
		usercmd_t *cmd = &client.cmdQueue.front();
		float speed, delta, scale;
		int savedTime;
		qboolean deltahax = qfalse;

		int serverTime = cmd->serverTime;
		int totalDelta = latestTime - cmd->serverTime;
		int timeDelta;

		if ( ent->client->pers.pmoveFixed ) {
			serverTime = ((serverTime + pmove_msec.integer-1) / pmove_msec.integer) * pmove_msec.integer;
		}

		timeDelta = serverTime - lastTime;

		if ( totalDelta >= drop_threshold ) {
			// zinx - whoops. too lagged.
			drop_threshold = LAG_MIN_DROP_THRESHOLD;
			lastTime = ent->client->ps.commandTime = cmd->serverTime;
			goto drop_packet;
		}

		if ( totalDelta < 0 ) {
			// zinx - oro? packet from the future
			goto drop_packet;
		}
		
		if ( timeDelta <= 0 ) {
			// zinx - packet from the past
			goto drop_packet;
		}

		scale = 1.f / LAG_DECAY;

		speed = G_CmdScale( ent, cmd );
		delta = (speed * (float)timeDelta);
		delta *= scale;

		if ( timeDelta > 50 ) {
			timeDelta = 50;
			delta = (speed * (float)timeDelta);
			delta *= scale;
			deltahax = qtrue;
		}

		if ( (client.cmdDelta + delta) >= LAG_MAX_DELTA ) {
			// too many commands this server frame

			// if it'll fit in the next frame, just wait until then.
			if ( delta < LAG_MAX_DELTA
			 && (totalDelta + delta) < LAG_MIN_DROP_THRESHOLD )
				break;

			// try to split it up in to smaller commands

			delta = ((float)LAG_MAX_DELTA - client.cmdDelta);
			timeDelta = (int)ceilf(delta / speed); // prefer speedup
			delta = (float)timeDelta * speed;

			if ( timeDelta < 1 )
				break;

			delta *= scale;
			deltahax = qtrue;
		}

		client.cmdDelta += delta;

		if ( deltahax ) {
			savedTime = cmd->serverTime;
			cmd->serverTime = lastTime + timeDelta;
		} else {
			savedTime = 0;	// zinx - shut up compiler
		}

		// zinx - erh.  hack, really. make it run for the proper amount of time.
		ent->client->ps.commandTime = lastTime;
		ClientThink_cmd( ent - g_entities, cmd, true );
		lastTime = ent->client->ps.commandTime;

		if ( deltahax ) {
			cmd->serverTime = savedTime;

			if ( delta <= 0.1f )
				break;

			continue;
		}

	  drop_packet:
		if ( client.cmdCount <= 0 ) {
			// ent->client was cleared...
			break;
		}

        client.cmdQueue.pop_front();
		client.cmdCount--;

		continue;
	}

	// zinx - added ping, packets processed this frame
	// warning: eats bandwidth like popcorn
	if ( g_antiwarp.integer & 32 ) {
		trap_SendServerCommand(
			ent - g_entities,
			va( "cp \"%d %d\n\"", latestTime - lastTime, startPackets - client.cmdCount )
		);
	}

	// zinx - debug; size is added lag (amount above player's network lag)
	// rotation is time
    /*
	if ( (g_antiwarp.integer & 16) && ent->client->cmdcount ) {
		vec3_t org, parms;

		VectorCopy( ent->client->ps.origin, org );
		SnapVector( org );

		parms[0] = 3;
		parms[1] = (float)(latestTime - ent->client->ps.commandTime) / 10.f;
		if (parms[1] < 1.f) parms[1] = 1.f;
		parms[2] = (ent->client->ps.commandTime * 180.f) / 1000.f;

		etpro_AddDebugLine( org, parms, ((ent - g_entities) % 32), LINEMODE_SPOKES, LINESHADER_RAILCORE, 0, qfalse );
	}
    */
	
	ent->client->ps.stats[STAT_ANTIWARP_DELAY] = latestTime - ent->client->ps.commandTime;
	if (ent->client->ps.stats[STAT_ANTIWARP_DELAY] < 0)
		ent->client->ps.stats[STAT_ANTIWARP_DELAY] = 0;
}

/*
===============
G_DamageFeedback

Called just before a snapshot is sent to the given player.
Totals up all damage and generates both the player_state_t
damage values to that client for pain blends and kicks, and
global pain sound events for all clients.
===============
*/
void P_DamageFeedback( gentity_t *player ) {
	gclient_t* client;
	int        count;
	vec3_t	   angles;

	client = player->client;

	if ( client->ps.pm_type == PM_DEAD ) {
		return;
	}

	// Jaymod - poison syringes
	if(G_IsPoisoned(player))  {
		client->ps.eFlags |= EF_POISON;
		return;
	}
	else {
		client->ps.eFlags &= ~EF_POISON;
	}

	// Jaybird - disorientation
	if (client->sess.disoriented[1]) {
		client->sess.disoriented[2] = qtrue;
		client->ps.viewangles[ROLL] = 0;
		client->ps.delta_angles[ROLL] = 32000;
	}
	else if (client->sess.disoriented[2]) {
		client->ps.viewangles[ROLL] = 0;
		client->ps.delta_angles[ROLL] = 0;
		client->sess.disoriented[2] = qfalse;
	}

	// total points of damage shot at the player this frame
	count = client->damage_blood;
	if ( count == 0 ) {
		return;		// didn't take any damage
	}

	if ( count > 127 ) {
		count = 127;
	}

	// send the information to the client

	// world damage (falling, slime, etc) uses a special code
	// to make the blend blob centered instead of positional
	if ( client->damage_fromWorld ) {
		client->ps.damagePitch = 255;
		client->ps.damageYaw = 255;

		client->damage_fromWorld = qfalse;
	} else {
		vectoangles( client->damage_from, angles );
		client->ps.damagePitch = int( angles[PITCH]/360.0f * 256 );
		client->ps.damageYaw = int( angles[YAW]/360.0f * 256 );
	}

	// play an apropriate pain sound
	if ( (level.time > player->pain_debounce_time) && !(player->flags & FL_GODMODE) && !(player->s.powerups & PW_INVULNERABLE) ) {	//----(SA)	
		player->pain_debounce_time = level.time + 700;
		G_AddEvent( player, EV_PAIN, player->health );
	}

	client->ps.damageEvent++;	// Ridah, always increment this since we do multiple view damage anims

	client->ps.damageCount = count;

	//
	// clear totals
	//
	client->damage_blood = 0;
	client->damage_knockback = 0;
}


#define MIN_BURN_INTERVAL 399 // JPW NERVE set burn timeinterval so we can do more precise damage (was 199 old model)

/*
=============
P_WorldEffects

Check for lava / slime contents and drowning
=============
*/
void P_WorldEffects( gentity_t *ent ) {
	int			waterlevel;

	if ( ent->client->noclip ) {
		ent->client->airOutTime = level.time + HOLDBREATHTIME;	// don't need air
		return;
	}

	waterlevel = ent->waterlevel;

	//
	// check for drowning
	//
	if ( waterlevel == 3 ) {
		// if out of air, start drowning
		if ( ent->client->airOutTime < level.time) {

			// drown!
			ent->client->airOutTime += 1000;
			if ( ent->health > 0 ) {
				// take more damage the longer underwater
				ent->damage += 2;
				if (ent->damage > 15)
					ent->damage = 15;

				// play a gurp sound instead of a normal pain sound
				if (ent->health <= ent->damage) {
					G_Sound(ent, G_SoundIndex("*drown.wav"));
				} else if (rand()&1) {
					G_Sound(ent, G_SoundIndex("sound/player/gurp1.wav"));
				} else {
					G_Sound(ent, G_SoundIndex("sound/player/gurp2.wav"));
				}

				// don't play a normal pain sound
				ent->pain_debounce_time = level.time + 200;

				G_Damage (ent, NULL, NULL, NULL, NULL, ent->damage, 0, MOD_WATER);
			}
		}
	} else {
		ent->client->airOutTime = level.time + HOLDBREATHTIME;
		ent->damage = 2;
	}

	//
	// check for sizzle damage (move to pmove?)
	//
	if (waterlevel && (ent->watertype&CONTENTS_LAVA) ) {
		if (ent->health > 0	&& ent->pain_debounce_time <= level.time ) {

				if (ent->watertype & CONTENTS_LAVA) {
					G_Damage (ent, NULL, NULL, NULL, NULL, 
						30*waterlevel, 0, MOD_LAVA);
				}

		}
	}

	//
	// check for burning from flamethrower
	//
	// JPW NERVE MP way
	if (ent->s.onFireEnd && ent->client) {
		if (level.time - ent->client->lastBurnTime >= MIN_BURN_INTERVAL) { 

			// JPW NERVE server-side incremental damage routine / player damage/health is int (not float)
			// so I can't allocate 1.5 points per server tick, and 1 is too weak and 2 is too strong.  
			// solution: allocate damage far less often (MIN_BURN_INTERVAL often) and do more damage.
			// That way minimum resolution (1 point) damage changes become less critical.

			ent->client->lastBurnTime = level.time;
			if ((ent->s.onFireEnd > level.time) && (ent->health > 0)) {
				gentity_t *attacker;
   				attacker = g_entities + ent->flameBurnEnt;
				G_Damage (ent, attacker, attacker, NULL, NULL, 5, DAMAGE_NO_KNOCKBACK, MOD_FLAMETHROWER); // JPW NERVE was 7
			}
		}
	}
	// jpw
}



/*
===============
G_SetClientSound
===============
*/
void G_SetClientSound( gentity_t *ent ) {
/*	if (ent->waterlevel && (ent->watertype & CONTENTS_LAVA) )	//----(SA)	modified since slime is no longer deadly
		ent->s.loopSound = level.snd_fry;
	else*/ // Gordon: doesnt exist
		ent->s.loopSound = 0;
}

/*
==============
PushBot
==============
*/
void BotVoiceChatAfterIdleTime( int client, const char *id, int mode, int delay, qboolean voiceonly, int idleTime, qboolean forceIfDead );

void PushBot( gentity_t *ent, gentity_t *other ) {
	vec3_t dir, ang, f, r;
	float oldspeed;
	//

	// dont push when mounted in certain stationary weapons
	if(other->client && (Bot_Util_AllowPush(other->client->ps.weapon) == qfalse || !other->client->sess.botPush))
		return;

	// dont push if scripted not to
	if(ent->client && !ent->client->sess.botPush)
		return;

	oldspeed = VectorLength( other->client->ps.velocity );
	if (oldspeed < 200)
		oldspeed = 200;
	//
	VectorSubtract( other->r.currentOrigin, ent->r.currentOrigin, dir );
	VectorNormalize( dir );
	vectoangles( dir, ang );
	AngleVectors( ang, f, r, NULL );
	f[2] = 0;
	r[2] = 0;
	//
	VectorMA( other->client->ps.velocity, 200, f, other->client->ps.velocity );
	VectorMA( other->client->ps.velocity, 100 * ((level.time+(ent->s.number*1000))%4000 < 2000 ? 1.0 : -1.0), r, other->client->ps.velocity );
	//
	if (VectorLengthSquared( other->client->ps.velocity ) > SQR(oldspeed)) {
		VectorNormalize( other->client->ps.velocity );
		VectorScale( other->client->ps.velocity, oldspeed, other->client->ps.velocity );
	}
}

/*
==============
ClientNeedsAmmo
==============
*/
qboolean ClientNeedsAmmo( int client ) {
	return AddMagicAmmo( &g_entities[client], 0 ) ? qtrue : qfalse;
}

// Does ent have enough "energy" to call artillery?
qboolean ReadyToCallArtillery( gentity_t* ent ) {
	if( ent->client->sess.skill[SK_SIGNALS] >= 2 ) {
		if( level.time - ent->client->ps.classWeaponTime <= (level.lieutenantChargeTime[ent->client->sess.sessionTeam-1]*0.66f) )
			return qfalse;
	} else if( level.time - ent->client->ps.classWeaponTime <= level.lieutenantChargeTime[ent->client->sess.sessionTeam-1] ) {
		return qfalse;
	}

	return qtrue;
}


// Are we ready to construct?  Optionally, will also update the time while we are constructing
qboolean ReadyToConstruct(gentity_t *ent, gentity_t *constructible, qboolean updateState)
{
	int weaponTime = ent->client->ps.classWeaponTime;

	// "Ammo" for this weapon is time based
	if( weaponTime + level.engineerChargeTime[ent->client->sess.sessionTeam-1] < level.time ) {
		weaponTime = level.time - level.engineerChargeTime[ent->client->sess.sessionTeam-1];
	}

	if( g_debugConstruct.integer ) {
		weaponTime += int( 0.5f*((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->constructibleStats.duration/(float)FRAMETIME)) );
	} else {
		if( ent->client->sess.skill[SK_EXPLOSIVES_AND_CONSTRUCTION] >= 3 )
			weaponTime += int( 0.66f*constructible->constructibleStats.chargebarreq*((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->constructibleStats.duration/(float)FRAMETIME)) );
			//weaponTime += 0.66f*((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->wait/(float)FRAMETIME));
			//weaponTime += 0.66f * 2.f * ((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->wait/(float)FRAMETIME));
		else
			weaponTime += int( constructible->constructibleStats.chargebarreq*((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->constructibleStats.duration/(float)FRAMETIME)) );
			//weaponTime += 2.f * ((float)level.engineerChargeTime[ent->client->sess.sessionTeam-1]/(constructible->wait/(float)FRAMETIME));
	}

	// if the time is in the future, we have NO energy left
	if (weaponTime > level.time)
	{
		// if we're supposed to update the state, reset the time to now
//		if( updateState )
//			ent->client->ps.classWeaponTime = level.time;

		return qfalse;
	}

	// only set the actual weapon time for this entity if they want us to
	if( updateState )
		ent->client->ps.classWeaponTime = weaponTime;

	return qtrue;
}

//==============================================================

/*
==============
ClientImpacts
==============
*/
void ClientImpacts( gentity_t *ent, pmove_t *pm ) {
	int		i, j;
	gentity_t	*other;
	trace_t	trace;

	memset( &trace, 0, sizeof(trace) );
	for (i=0 ; i<pm->numtouch ; i++) {
		for (j=0 ; j<i ; j++) {
			if (pm->touchents[j] == pm->touchents[i] ) {
				break;
			}
		}
		if (j != i) {
			continue;	// duplicated
		}
		other = &g_entities[ pm->touchents[i] ];

		// RF, bot should get pushed out the way
		if ( (ent->client) /*&& !(ent->r.svFlags & SVF_BOT)*/ && (other->r.svFlags & SVF_BOT) && 
			!other->client->ps.powerups[PW_INVULNERABLE] ) {
/*			vec3_t dir;
			// if we are not heading for them, ignore
			VectorSubtract( other->r.currentOrigin, ent->r.currentOrigin, dir );
			VectorNormalize( dir );
			if (DotProduct( ent->client->ps.velocity, dir ) > 0) {
				PushBot( ent, other );
			}
*/
			PushBot( ent, other );
		}

		// if we are standing on their head, then we should be pushed also
		if ( (ent->r.svFlags & SVF_BOT) && (ent->s.groundEntityNum == other->s.number && other->client) &&
			!other->client->ps.powerups[PW_INVULNERABLE]) {
			PushBot( other, ent );
		}

		if ( !other->touch ) {
			continue;
		}

		other->touch( other, ent, &trace );
	}

}

/*
============
G_TouchTriggers

Find all trigger entities that ent's current position touches.
Spectators will only interact with teleporters.
============
*/
void	G_TouchTriggers( gentity_t *ent ) {
	int			i, num;
	int			touch[MAX_GENTITIES];
	gentity_t	*hit;
	trace_t		trace;
	vec3_t		mins, maxs;
	static vec3_t	range = { 40, 40, 52 };

	if ( !ent->client ) {
		return;
	}

	// Arnout: reset the pointer that keeps track of trigger_objective_info tracking
	ent->client->touchingTOI = NULL;

	// dead clients don't activate triggers!
	if ( ent->client->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	VectorSubtract( ent->client->ps.origin, range, mins );
	VectorAdd( ent->client->ps.origin, range, maxs );

	num = trap_EntitiesInBox( mins, maxs, touch, MAX_GENTITIES );

	// can't use ent->absmin, because that has a one unit pad
	VectorAdd( ent->client->ps.origin, ent->r.mins, mins );
	VectorAdd( ent->client->ps.origin, ent->r.maxs, maxs );

	for ( i=0 ; i<num ; i++ ) {
		hit = &g_entities[touch[i]];

		if ( !hit->touch && !ent->touch ) {
			continue;
		}
		if ( !( hit->r.contents & CONTENTS_TRIGGER ) ) {
			continue;
		}

		// Arnout: invisible entities can't be touched
		// Gordon: radiant tabs arnout! ;)
		if( hit->entstate == STATE_INVISIBLE ||
			hit->entstate == STATE_UNDERCONSTRUCTION ) {
			continue;
		}

		// ignore most entities if a spectator
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
			if ( hit->s.eType != ET_TELEPORT_TRIGGER ) {
				continue;
			}
		}

		// use seperate code for determining if an item is picked up
		// so you don't have to actually contact its bounding box
		if ( hit->s.eType == ET_ITEM ) {
			if ( !BG_PlayerTouchesItem( &ent->client->ps, &hit->s, level.time ) ) {
				continue;
			}
		} else {
			// MrE: always use capsule for player
			if ( !trap_EntityContactCapsule( mins, maxs, hit ) ) {
			//if ( !trap_EntityContact( mins, maxs, hit ) ) {
				continue;
			}
		}

		memset( &trace, 0, sizeof(trace) );

		if ( hit->touch ) {
			hit->touch (hit, ent, &trace);
		}
	}
}

/*
=================
SpectatorThink
=================
*/
void SpectatorThink( gentity_t *ent, usercmd_t *ucmd ) {
	pmove_t	pm;
	gclient_t	*client;
	gentity_t *crosshairEnt = NULL; // rain - #480

	client = ent->client;

	// rain - #480 - sanity check - check .active in case the client sends us
	// something completely bogus
	crosshairEnt = &g_entities[ent->client->ps.identifyClient];

	if (crosshairEnt->inuse && crosshairEnt->client &&
		(ent->client->sess.sessionTeam == crosshairEnt->client->sess.sessionTeam ||
		crosshairEnt->client->ps.powerups[PW_OPS_DISGUISED])) {

		// rain - identifyClientHealth sent as unsigned char, so we
		// can't transmit negative numbers
		if (crosshairEnt->health >= 0)
			ent->client->ps.identifyClientHealth = crosshairEnt->health;
		else
			ent->client->ps.identifyClientHealth = 0;
	}

	if ( client->sess.spectatorState != SPECTATOR_FOLLOW ) {
		client->ps.pm_type = PM_SPECTATOR;
		client->ps.speed = 800;	// was: 400 // faster than normal
		if (client->ps.sprintExertTime)
			client->ps.speed *= 3;	// (SA) allow sprint in free-cam mode


		// OSP - dead players are frozen too, in a timeout
		if((client->ps.pm_flags & PMF_LIMBO) && level.match_pause != PAUSE_NONE) {
			client->ps.pm_type = PM_FREEZE;
		} else if( client->noclip ) {
			client->ps.pm_type = PM_NOCLIP;
		}

		// set up for pmove
		memset (&pm, 0, sizeof(pm));
		pm.ps = &client->ps;
		pm.pmext = &client->pmext;
		pm.character = client->pers.character;
		pm.cmd = *ucmd;
		pm.skill = client->sess.skill;
		pm.tracemask = MASK_PLAYERSOLID & ~CONTENTS_BODY;	// spectators can fly through bodies
		pm.trace = trap_TraceCapsuleNoEnts;
		pm.pointcontents = trap_PointContents;

		Pmove( &pm ); // JPW NERVE

		// Rafael - Activate
		// Ridah, made it a latched event (occurs on keydown only)
		if (client->latched_buttons & BUTTON_ACTIVATE)
		{
			Cmd_Activate_f (ent);
		}

		// save results of pmove
		VectorCopy( client->ps.origin, ent->s.origin );

		G_TouchTriggers( ent );
		trap_UnlinkEntity( ent );
	}

	if (ent->flags & FL_NOFATIGUE)
		ent->client->pmext.sprintTime = int(SPRINTTIME);


	client->oldbuttons = client->buttons;
	client->buttons = ucmd->buttons;

//----(SA)	added
	client->oldwbuttons = client->wbuttons;
	client->wbuttons = ucmd->wbuttons;

	// MV clients use these buttons locally for other things
	if(client->pers.mvCount < 1) {

		// attack button cycles through spectators
		// Jaybird - set up flags 1 and 2 for g_spectator
		if (((client->buttons & BUTTON_ATTACK ) && !( client->oldbuttons & BUTTON_ATTACK)) ||
            ((client->wbuttons & WBUTTON_ATTACK2) && !(client->oldwbuttons & WBUTTON_ATTACK2))) {
			// If they are NOT following someone
			if( client->sess.spectatorState != SPECTATOR_FOLLOW ) {

				// Click and spec
				if( g_spectator.integer & SPEC_CLICK ) {
					vec3_t forward, right, up, offset, end;

					AngleVectors(ent->client->ps.viewangles, forward, right, up);
					VectorCopy(ent->client->ps.origin, offset);
					offset[2] += ent->client->ps.viewheight;
					VectorMA(offset, 8192, forward, end);

                    TraceContext trx(*ent, *ent, MASK_PLAYERSOLID, reinterpret_cast<vec_t(&)[3]>(*offset), reinterpret_cast<vec_t(&)[3]>(*end) );
                    if (AbstractBulletModel::fireWorld( trx ) && trx.resultIsPlayer()) {
					    ent->client->sess.spectatorState = SPECTATOR_FOLLOW;
					    ent->client->sess.spectatorClient = trx.data.entityNum;
					    return;
                    }
				}
			}
			
			// Miss and spec (or normal operation if you're already following)
			if (!(g_spectator.integer & SPEC_CLICKMISS))
				return;
				
			// Use the correct direction
            if (client->buttons & BUTTON_ATTACK)
			    Cmd_FollowCycle_f(ent, 1);
            else
                Cmd_FollowCycle_f(ent, -1);
				
        } else if ( 
			( client->sess.sessionTeam == TEAM_SPECTATOR ) && // don't let dead team players do free fly
			( client->sess.spectatorState == SPECTATOR_FOLLOW ) && 
			( ( ( client->buttons & BUTTON_ACTIVATE ) && 
			! ( client->oldbuttons & BUTTON_ACTIVATE )) || ucmd->upmove > 0 ) &&
			G_allowFollow(ent, TEAM_AXIS) && G_allowFollow(ent, TEAM_ALLIES) )
		{
			// code moved to StopFollowing
			StopFollowing(ent);
		}
	}
}


/*
=================
ClientInactivityTimer

Returns qfalse if the client is dropped
=================
*/
qboolean ClientInactivityTimer( gclient_t *client ) {
	// OSP - modified
	if( ( g_inactivity.integer == 0 && client->sess.sessionTeam != TEAM_SPECTATOR ) || ( g_spectatorInactivity.integer == 0 && client->sess.sessionTeam == TEAM_SPECTATOR ) ) {

		// give everyone some time, so if the operator sets g_inactivity during
		// gameplay, everyone isn't kicked
		client->inactivityTime = level.time + 60 * 1000;
		client->inactivityWarning = qfalse;
	} else if ( client->pers.cmd.forwardmove || 
		client->pers.cmd.rightmove || 
		client->pers.cmd.upmove ||
		(client->pers.cmd.wbuttons & WBUTTON_ATTACK2) ||
		(client->pers.cmd.buttons & BUTTON_ATTACK) ||
		(client->pers.cmd.wbuttons & WBUTTON_LEANLEFT) ||
		(client->pers.cmd.wbuttons & WBUTTON_LEANRIGHT)
		|| client->ps.pm_type == PM_DEAD ) {

		client->inactivityWarning = qfalse;
		client->inactivityTime = level.time + 1000 *
								 ((client->sess.sessionTeam != TEAM_SPECTATOR) ?
												g_inactivity.integer :
												g_spectatorInactivity.integer);

	} else if ( !client->pers.localClient ) {
		// Jaybird - Check for shrubbot permission
		if ( level.time > client->inactivityTime && client->inactivityWarning && !cmd::entityHasPermission( &g_entities[client-level.clients], priv::base::voteImmunity )) {
			client->inactivityWarning = qfalse;
			client->inactivityTime = level.time + 60 * 1000;
			trap_DropClient(client - level.clients, "Dropped due to inactivity", 0 );
			return(qfalse);
		}

		if ( !client->inactivityWarning && level.time > client->inactivityTime - 10000 ) {
			CPx(client - level.clients, "cp \"^310 seconds until inactivity drop!\n\"");
			CPx(client - level.clients, "print \"^310 seconds until inactivity drop!\n\"");
			G_Printf("10s inactivity warning issued to: %s\n", client->pers.netname);

			client->inactivityWarning = qtrue;
			client->inactivityTime = level.time + 10000;	// Just for safety
		}
	}
	return qtrue;
}

/*
==================
G_MedicRegen

Handles medic regeneration
==================
*/
static void G_MedicRegen( gentity_t* ent )
{
    // No regeneration for poisoned players
    if (G_IsPoisoned( ent ))
        return;

    // Completely disabled
    if (g_medics.integer & MEDIC_NOREGEN)
        return;

    // If the delayed regen is enabled, make sure there's 5 seconds
    // between being hit and allowing regen to start occurring.
    if ((g_medics.integer & MEDIC_DELAYREGEN) && (level.time - ent->client->lasthurt_time) < 5000)
        return;

    if (ent->health < ent->client->ps.stats[STAT_MAX_HEALTH]) {
        ent->health += 3;

        // Jaybird - handle g_medics less regen
        if (g_medics.integer & MEDIC_LESSREGEN)
	        ent->health --;

        if ( ent->health > ent->client->ps.stats[STAT_MAX_HEALTH] * 1.1f){
	        ent->health = int( ent->client->ps.stats[STAT_MAX_HEALTH] * 1.1f );
        }

        return;
    }
    else if( ent->health < ent->client->ps.stats[STAT_MAX_HEALTH] * 1.12f) {
        ent->health += 2;

        // Jaybird - handle g_medics less regen
        if (g_medics.integer & MEDIC_LESSREGEN)
	        ent->health --;

        if( ent->health > ent->client->ps.stats[STAT_MAX_HEALTH] * 1.12f ) {
	        ent->health = int( ent->client->ps.stats[STAT_MAX_HEALTH] * 1.12f );
        }

        return;
    }
}

/*
==================
ClientTimerActions

Actions that happen once a second
==================
*/
void ClientTimerActions( gentity_t *ent, int msec ) {
	gclient_t *client;

	client = ent->client;
	client->timeResidual += msec;

	while( client->timeResidual >= 1000 ) {
		client->timeResidual -= 1000;

		// Medic regeneration
        if( client->sess.playerType == PC_MEDIC ) {
            G_MedicRegen( ent );
        } else {
			// count down health when over max
            if ( ent->health > client->ps.stats[STAT_MAX_HEALTH] ) {
				ent->health--;
            }
        }

        if (client->pers.shakeTime > level.time) {
			G_ShakeClient(ent, 1000000);
		}

		if (G_IsPoisoned(ent)) {

			// Force animation every 3 seconds
			if (!((level.time/1000) % 3))
				BG_AnimScriptEvent( &ent->client->ps, ent->client->pers.character->animModelInfo, ANIM_ET_PAIN, qfalse, qtrue );

			// Sound
			G_ClientSound( ent, "sound/jaymod/heartbeat.wav" );
		}
	}

	// Jaybird - reset stealTime
	if( ent->stealProgress && level.time-ent->stealTime > 5000 ) {
		ent->stealProgress = ent->stealTime = 0;
	}

	// Jaybird - handle poison needles
	G_RunPoisonEvents(ent);
}

void ClientTimerRegenCarryOver( gentity_t *ent, int msec ) {
	gclient_t* client = ent->client;
	client->regenCarryOverTime += msec;

	while (client->regenCarryOverTime >= SK5G_MEDCARRY_TIMER) {
		client->regenCarryOverTime -= SK5G_MEDCARRY_TIMER;

        if (ent->client->sess.skill[SK_FIRST_AID] < 5)
			continue;

		if (ent->health >= client->ps.stats[STAT_MAX_HEALTH])
			continue;

		if (G_IsPoisoned(ent))
			continue;

        switch (client->sess.playerType) {
            case PC_COVERTOPS:
                if (cvars::bg_sk5_medic.ivalue & SK5_MED_CARRY_CVO)
                    ent->health += 1;
                break;

            case PC_ENGINEER:
	            if (cvars::bg_sk5_medic.ivalue & SK5_MED_CARRY_ENG)
                    ent->health += 1;
                break;

            case PC_FIELDOPS:
                if (cvars::bg_sk5_medic.ivalue & SK5_MED_CARRY_FDO)
                    ent->health += 1;
                break;

            default:
            case PC_MEDIC:
                break;

            case PC_SOLDIER:
	            if (cvars::bg_sk5_medic.ivalue & SK5_MED_CARRY_SOL)
                    ent->health += 1;
                break;
        }
    }
}

/*
====================
ClientIntermissionThink
====================
*/
void ClientIntermissionThink( gclient_t *client ) {
	client->ps.eFlags &= ~EF_TALK;
	client->ps.eFlags &= ~EF_FIRING;

	// the level will exit when everyone wants to or after timeouts

	// swap and latch button actions
	client->oldbuttons = client->buttons;
	client->buttons = client->pers.cmd.buttons;

//----(SA)	added
	client->oldwbuttons = client->wbuttons;
	client->wbuttons = client->pers.cmd.wbuttons;
}


/*
================
ClientEvents

Events will be passed on to the clients for presentation,
but any server game effects are handled here
================
*/
void ClientEvents( gentity_t *ent, int oldEventSequence ) {
	int			i;
	int			event;
	int			eventParm;
	gclient_t	*client;

	client = ent->client;

	if ( oldEventSequence < client->ps.eventSequence - MAX_EVENTS ) {
		oldEventSequence = client->ps.eventSequence - MAX_EVENTS;
	}
	for ( i = oldEventSequence ; i < client->ps.eventSequence ; i++ ) {
		event = client->ps.events[ i & (MAX_EVENTS-1) ];
		eventParm = client->ps.eventParms[ i & (MAX_EVENTS-1) ];

		switch ( event ) {
		case EV_FALL_NDIE:
		case EV_FALL_SHORT:
		case EV_FALL_DMG_10:
		case EV_FALL_DMG_15:
		case EV_FALL_DMG_25:
		case EV_FALL_DMG_50:
			G_FallDamage(ent, event);
            ent->client->pmext.wasShoved = qfalse;
			break;

		case EV_FIRE_WEAPON_MG42:

			// Gordon: reset player disguise on stealing docs
			ent->client->ps.powerups[PW_OPS_DISGUISED] = 0;

			mg42_fire( ent );

			// Only 1 stats bin for mg42
#ifndef DEBUG_STATS
			if(cvars::gameState.ivalue == GS_PLAYING)
#endif
				ent->client->sess.aWeaponStats[BG_WeapStatForWeapon(WP_MOBILE_MG42)].atts++;

			break;
		case EV_FIRE_WEAPON_MOUNTEDMG42:
			// Gordon: reset player disguise on stealing docs
			ent->client->ps.powerups[PW_OPS_DISGUISED] = 0;

			mountedmg42_fire( ent );
			// Only 1 stats bin for mg42
#ifndef DEBUG_STATS
			if(cvars::gameState.ivalue == GS_PLAYING)
#endif
				ent->client->sess.aWeaponStats[BG_WeapStatForWeapon(WP_MOBILE_MG42)].atts++;

			break;

		case EV_FIRE_WEAPON_AAGUN:

			// Gordon: reset player disguise on stealing docs
			ent->client->ps.powerups[PW_OPS_DISGUISED] = 0;

			aagun_fire( ent );
			break;

		case EV_FIRE_WEAPON:
		case EV_FIRE_WEAPONB:
		case EV_FIRE_WEAPON_LASTSHOT:
			FireWeapon( ent );
			break;

		case EV_THROW_KNIFE:
			G_ThrowingKnife_launch( *ent, eventParm );
			break;

		case EV_THROW_MOLOTOV:
			molotov::launch( *ent, eventParm );
			break;

		default:
			break;
		}
	}

}

/*
==================
LookAtKiller
==================
*/
void WolfFindKiller( gentity_t* self )
{
	gclient_t	*cl = NULL;
	vec3_t	start, end;
//	vec3_t	temp;	// rain - unused
	trace_t	tr;

	self->client->ps.viewlocked_entNum = 0;
	self->client->ps.viewlocked = 0;
	self->client->ps.stats[STAT_DEAD_YAW] = 999;

	VectorCopy( self->s.pos.trBase, start );
	start[2] += self->client->ps.viewheight;

    // Check if the last killed by client is set, if not, wasn't killed by a player
    if (self->client->lastkilledby_client < 0)
        return;

    // Only do this for 5 seconds
    if (level.time - self->client->lasthurt_time > 5000)
        return;

    // Sanity check
    gentity_t* ent = &g_entities[self->client->lastkilledby_client];
    if (!ent || !ent->client)
        return;

    cl = ent->client;

    // No dead players
	if( cl->ps.pm_type == PM_DEAD ) {
		return;
	}

    // No playdead players
    if (cl->ps.eFlags & EF_PLAYDEAD) {
        return;
    }

    // No limbo'ed players
	if( cl->ps.pm_flags & PMF_LIMBO ) {
		return;
	}

    // Again, no dead players :)
	if( cl->ps.stats[ STAT_HEALTH ] <= 0 ) {
		return;
	}

    // Check if we can actually see them
	VectorCopy(g_entities[cl - level.clients].s.pos.trBase, end );
	end[2] += cl->ps.viewheight;
    trap_Trace (&tr, start, NULL, NULL, end, self->s.number, MASK_SHOT);
    if (tr.entityNum >= MAX_CLIENTS || tr.entityNum != ent->s.number)
        return;

    // Lock onto them
    self->client->ps.viewlocked_entNum = cl->ps.clientNum;
	self->client->ps.viewlocked = 7;
}

// DHM - Nerve
void WolfFindMedic( gentity_t *self ) {
	int i, medic=-1;
	gclient_t	*cl;
	vec3_t	start, end;
//	vec3_t	temp;	// rain - unused
	trace_t	tr;
	float	bestdist=1024, dist;

	self->client->ps.viewlocked_entNum = 0;
	self->client->ps.viewlocked = 0;
	self->client->ps.stats[STAT_DEAD_YAW] = 999;

	VectorCopy( self->s.pos.trBase, start );
	start[2] += self->client->ps.viewheight;

	for( i = 0; i < level.numConnectedClients; i++ ) {
		cl = &level.clients[ level.sortedClients[i] ];

		if( level.sortedClients[i] == self->client->ps.clientNum ) {
			continue;
		}

		if( cl->sess.sessionTeam != self->client->sess.sessionTeam ) {
			continue;
		}

		if( cl->ps.pm_type == PM_DEAD ) {
			continue;
		}

        // playdead medics should not be locked to
        if (cl->ps.eFlags & EF_PLAYDEAD) {
            continue;
        }

		// zinx - limbo'd players are not PM_DEAD or STAT_HEALTH <= 0.
		// and we certainly don't want to lock to them
		// fix for bug #345
		if( cl->ps.pm_flags & PMF_LIMBO ) {
			continue;
		}

		if( cl->ps.stats[ STAT_HEALTH ] <= 0 ) {
			continue;
		}

		if( cl->ps.stats[ STAT_PLAYER_CLASS ] != PC_MEDIC ) {
			continue;
		}

		VectorCopy( g_entities[level.sortedClients[i]].s.pos.trBase, end );
		end[2] += cl->ps.viewheight;

		trap_Trace (&tr, start, NULL, NULL, end, self->s.number, CONTENTS_SOLID);
		if( tr.fraction < 0.95 ) {
			continue;
		}

		VectorSubtract( end, start, end );
		dist = VectorNormalize( end );

		if ( dist < bestdist ) {
			medic = cl->ps.clientNum;
#if 0 // rain - not sure what the point of this is
			vectoangles( end, temp );
			self->client->ps.stats[STAT_DEAD_YAW] = temp[YAW];
#endif
			bestdist = dist;
		}
	}

	if ( medic >= 0 ) {
		self->client->ps.viewlocked_entNum = medic;
		self->client->ps.viewlocked = 7;
	}
}


//void ClientDamage( gentity_t *clent, int entnum, int enemynum, int id );		// NERVE - SMF

/*
==============
ClientThink

This will be called once for each client frame, which will
usually be a couple times for each server frame on fast clients.

If "g_synchronousClients 1" is set, this will be called exactly
once for each server frame, which makes for smooth demo recording.
==============
*/
void ClientThink_real( gentity_t *ent, bool skipServerTime ) {
	int			msec, oldEventSequence;
	pmove_t		pm;
	usercmd_t	*ucmd;
	gclient_t	*client = ent->client;


	// don't think if the client is not yet connected (and thus not yet spawned in)
	if (client->pers.connected != CON_CONNECTED) {
		return;
	}

	if( ent->s.eFlags & EF_MOUNTEDTANK && ent->tagParent ) {
		client->pmext.centerangles[YAW] = ent->tagParent->r.currentAngles[ YAW ];
		client->pmext.centerangles[PITCH] = ent->tagParent->r.currentAngles[ PITCH ];
	}

	// mark the time, so the connection sprite can be removed
	ucmd = &ent->client->pers.cmd;

	ent->client->ps.identifyClient = ucmd->identClient;		// NERVE - SMF

	// sanity check the command time to prevent speedup cheating
    if (!skipServerTime) {
	    if ( ucmd->serverTime > level.time + 200 ) {
		    ucmd->serverTime = level.time + 200;
	    }
	    if ( ucmd->serverTime < level.time - 1000 ) {
		    ucmd->serverTime = level.time - 1000;
	    } 
    }

	// frameOffset should be about the number of milliseconds into a frame 
	// this command packet was received, depending on how fast the server
	// does a G_RunFrame()
	client->frameOffset = trap_Milliseconds() - level.frameStartTime;


//unlagged - true ping
	// save the estimated ping in a queue for averaging later

	// we use level.previousTime to account for 50ms lag correction
	// besides, this will turn out numbers more like what players are used to
	client->pers.pingsamples[client->pers.samplehead] = level.previousTime + client->frameOffset - ucmd->serverTime;
	client->pers.samplehead++;
	if ( client->pers.samplehead >= NUM_PING_SAMPLES ) {
		client->pers.samplehead -= NUM_PING_SAMPLES;
	}

	// initialize the real ping
	if ( g_truePing.integer ) {
		int i, sum = 0;

		// get an average of the samples we saved up
		for ( i = 0; i < NUM_PING_SAMPLES; i++ ) {
			sum += client->pers.pingsamples[i];
		}

		client->pers.realPing = sum / NUM_PING_SAMPLES;
	}
	else {
		// if g_truePing is off, use the normal ping
		client->pers.realPing = client->ps.ping;
	}
//unlagged - true ping


//unlagged - smooth clients #1
	// keep track of this for later - we'll use this to decide whether or not
	// to send extrapolated positions for this client
	client->lastUpdateFrame = level.framenum;
//unlagged - smooth clients #1


//unlagged - true ping
	// make sure the true ping is over 0 - with cl_timenudge it can be less
	if ( client->pers.realPing < 0 ) {
		client->pers.realPing = 0;
	}
//unlagged - true ping


    // Unmute the player if the time is expired
    if (connectedUsers[ent-g_entities]->muted) {
        User& user = *connectedUsers[ent-g_entities];

		if (user.muteExpiry && user.muteExpiry <= time(NULL)) {
            // Clear normal mute
            G_UnmutePlayer(ent);
		    CPx(ent - g_entities, "chat \"^3Your mute has expired, and you have been auto-unmuted.\"");
        }
	}

	msec = ucmd->serverTime - client->ps.commandTime;
	// following others may result in bad times, but we still want
	// to check for follow toggles
	if ( msec < 1 && client->sess.spectatorState != SPECTATOR_FOLLOW ) {
		return;
	}
	if ( msec > 200 ) {
		msec = 200;
	}

	if ( !skipServerTime && (pmove_fixed.integer || client->pers.pmoveFixed)) {
		ucmd->serverTime = ((ucmd->serverTime + pmove_msec.integer-1) / pmove_msec.integer) * pmove_msec.integer;
	}

	if( client->wantsscore ) {
		G_SendScore( ent );
		client->wantsscore = qfalse;
	}

	//
	// check for exiting intermission
	//
	if ( level.intermissiontime ) {
		ClientIntermissionThink( client );
		return;
	}

	// check for inactivity timer, but never drop the local client of a non-dedicated server
	// OSP - moved here to allow for spec inactivity checks as well
	if ( !ClientInactivityTimer( client ) ) {
		return;
	}
	
	if( !(ent->r.svFlags & SVF_BOT) && level.time - client->pers.lastCCPulseTime > 2000 ) {
		G_SendMapEntityInfo( ent );
		client->pers.lastCCPulseTime = level.time;
	}

	if( !(ucmd->flags & 0x01) || ucmd->forwardmove || ucmd->rightmove || ucmd->upmove || ucmd->wbuttons || ucmd->doubleTap ) {
		ent->r.svFlags &= ~(SVF_SELF_PORTAL_EXCLUSIVE|SVF_SELF_PORTAL);
	}

	// Jaybird - Check auth stuff
	if( !( ent->r.svFlags & SVF_BOT ) && ent->clientCheckAlarm && level.frameTime >= ent->clientCheckAlarm ) {
		G_AuthCheck( ent, NULL );
	}

	// spectators don't do much
	// DHM - Nerve :: In limbo use SpectatorThink
	if ( client->sess.sessionTeam == TEAM_SPECTATOR || client->ps.pm_flags & PMF_LIMBO ) {
		/*if ( client->sess.spectatorState == SPECTATOR_SCOREBOARD ) {
			return;
		}*/
		SpectatorThink( ent, ucmd );
		return;
	}

	if((client->ps.eFlags & EF_VIEWING_CAMERA) || level.match_pause != PAUSE_NONE) {
		ucmd->buttons = 0;
		ucmd->forwardmove = 0;
		ucmd->rightmove = 0;
		ucmd->upmove = 0;
		ucmd->wbuttons = 0;
		ucmd->doubleTap = 0;

		// freeze player (RELOAD_FAILED still allowed to move/look)
		if(level.match_pause != PAUSE_NONE) {
			client->ps.pm_type = PM_FREEZE;
		} else if((client->ps.eFlags & EF_VIEWING_CAMERA)) {
			VectorClear(client->ps.velocity);
			client->ps.pm_type = PM_FREEZE;
		}
	} else if ( client->noclip ) {
		client->ps.pm_type = PM_NOCLIP;
	} else if ( client->ps.pm_type == PM_PLAYDEAD) {
		// no need to change it since it will
		// be adjusted by PM_CheckPlayDead regardless
	} else if ( client->ps.stats[STAT_HEALTH] <= 0 ) {
		client->ps.pm_type = PM_DEAD;
	} else {
		client->ps.pm_type = PM_NORMAL;
	}

	client->ps.aiState = AISTATE_COMBAT;
	client->ps.gravity = g_gravity.integer;

	// set speed
	client->ps.speed = g_speed.integer;

	if( client->speedScale )				// Goalitem speed scale
		client->ps.speed = int( client->ps.speed * (client->speedScale * 0.01f) );

	// set up for pmove
	oldEventSequence = client->ps.eventSequence;

	client->currentAimSpreadScale = (float)client->ps.aimSpreadScale/255.0f;

	memset (&pm, 0, sizeof(pm));

	pm.ps = &client->ps;
	pm.pmext = &client->pmext;
	pm.character = client->pers.character;
	pm.cmd = *ucmd;
	pm.oldcmd = client->pers.oldcmd;
	// MrE: always use capsule for AI and player
	pm.trace = trap_TraceCapsule;

	if ( pm.ps->pm_type == PM_DEAD ) {
		pm.tracemask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
		// DHM-Nerve added:: EF_DEAD is checked for in Pmove functions, but wasn't being set until after Pmove
		pm.ps->eFlags |= EF_DEAD;
	} else if( pm.ps->pm_type == PM_SPECTATOR ) {
		pm.trace = trap_TraceCapsuleNoEnts;
	} else {
        // Normal player mask
		pm.tracemask = MASK_PLAYERSOLID;
	}

    if (pm.ps->eFlags & EF_PLAYDEAD) {
        // Playdead players can be walked through
        ent->r.contents = CONTENTS_CORPSE;
    }

	//DHM - Nerve :: We've gone back to using normal bbox traces
	//pm.trace = trap_Trace;
	pm.pointcontents = trap_PointContents;
	pm.debugLevel = g_debugMove.integer;
	pm.noFootsteps = qfalse;

	pm.pmove_fixed = pmove_fixed.integer | client->pers.pmoveFixed;
	pm.pmove_msec = pmove_msec.integer;

	pm.noWeapClips = qfalse;

	VectorCopy( client->ps.origin, client->oldOrigin );

	// NERVE - SMF
	pm.gametype = g_gametype.integer;
	pm.ltChargeTime = level.lieutenantChargeTime[client->sess.sessionTeam-1];
	pm.soldierChargeTime = level.soldierChargeTime[client->sess.sessionTeam-1];
	pm.engineerChargeTime = level.engineerChargeTime[client->sess.sessionTeam-1];
	pm.medicChargeTime = level.medicChargeTime[client->sess.sessionTeam-1];
	// -NERVE - SMF

	pm.skill = client->sess.skill;

	client->pmext.airleft = ent->client->airOutTime - level.time;

	pm.covertopsChargeTime = level.covertopsChargeTime[client->sess.sessionTeam-1];

	if( client->ps.pm_type != PM_DEAD && level.timeCurrent - client->pers.lastBattleSenseBonusTime > 45000 ) {
		/*switch( client->combatState )
		{
		case COMBATSTATE_COLD:	G_AddSkillPoints( ent, SK_BATTLE_SENSE, 0.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 0.f, "combatstate cold" ); break;
		case COMBATSTATE_WARM:	G_AddSkillPoints( ent, SK_BATTLE_SENSE, 2.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 2.f, "combatstate warm" ); break;
		case COMBATSTATE_HOT:	G_AddSkillPoints( ent, SK_BATTLE_SENSE, 5.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 5.f, "combatstate hot" ); break;
		case COMBATSTATE_SUPERHOT:	G_AddSkillPoints( ent, SK_BATTLE_SENSE, 8.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 8.f, "combatstate super-hot" ); break;
		}*/

		if( client->combatState != COMBATSTATE_COLD ) {
			if( client->combatState & (1<<COMBATSTATE_KILLEDPLAYER) && client->combatState & (1<<COMBATSTATE_DAMAGERECEIVED) ) {
				G_AddSkillPoints( ent, SK_BATTLE_SENSE, 8.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 8.f, "combatstate super-hot" );
			} else if( client->combatState & (1<<COMBATSTATE_DAMAGEDEALT) && client->combatState & (1<<COMBATSTATE_DAMAGERECEIVED) ) {
				G_AddSkillPoints( ent, SK_BATTLE_SENSE, 5.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 5.f, "combatstate hot" );
			} else {
				G_AddSkillPoints( ent, SK_BATTLE_SENSE, 2.f ); G_DebugAddSkillPoints( ent, SK_BATTLE_SENSE, 2.f, "combatstate warm" );
			}
		}

		client->pers.lastBattleSenseBonusTime = level.timeCurrent;
		client->combatState = COMBATSTATE_COLD;	// cool down again
	}

	pm.leadership = qfalse; // never used anyways

	// Gordon: bit hacky, stop the slight lag from client -> server even on locahost, switching back to the weapon you were holding
	//			and then back to what weapon you should have, became VERY noticible for the kar98/carbine + gpg40, esp now i've added the
	//			animation locking
	if( level.time - client->pers.lastSpawnTime < 1000 ) {
		pm.cmd.weapon = client->ps.weapon;
	}

	Pmove( &pm );

	// Gordon: thx to bani for this
	// ikkyo - fix leaning players bug                                       
	VectorCopy( client->ps.velocity, ent->s.pos.trDelta );                  
	SnapVector( ent->s.pos.trDelta );
	// end

	// server cursor hints
	// TAT 1/10/2003 - bots don't need to check for cursor hints
	if ( !(ent->r.svFlags & SVF_BOT) && ent->lastHintCheckTime < level.time )
	{
		G_CheckForCursorHints(ent);

		ent->lastHintCheckTime = level.time + FRAMETIME;
	}

	// DHM - Nerve :: Set animMovetype to 1 if ducking
	if ( ent->client->ps.pm_flags & PMF_DUCKED )
		ent->s.animMovetype = 1;
	else
		ent->s.animMovetype = 0;

	// save results of pmove
	if ( ent->client->ps.eventSequence != oldEventSequence ) {
		ent->eventTime = level.time;
		ent->r.eventTime = level.time;
	}

    qboolean snap = (cvars::g_snap.ivalue & G_SNAP_CLIENT_THINK) ? qtrue : qfalse;
    BG_PlayerStateToEntityState( &ent->client->ps, &ent->s, level.time, snap );

	if ( !( ent->client->ps.eFlags & EF_FIRING ) ) {
		client->fireHeld = qfalse;		// for grapple
	}

//	// use the snapped origin for linking so it matches client predicted versions
	VectorCopy( ent->s.pos.trBase, ent->r.currentOrigin );

	VectorCopy (pm.mins, ent->r.mins);
	VectorCopy (pm.maxs, ent->r.maxs);

	ent->waterlevel = pm.waterlevel;
	ent->watertype = pm.watertype;

	// execute client events
	if(level.match_pause == PAUSE_NONE) {
		ClientEvents( ent, oldEventSequence );
        if ( ent->client->ps.groundEntityNum != ENTITYNUM_NONE ) {
			if ( !(ent->client->ps.pm_flags & PMF_TIME_LAND) )
				ent->client->pmext.wasShoved = qfalse;
		}
	}

	// link entity now, after any personal teleporters have been used
	trap_LinkEntity (ent);
	if ( !ent->client->noclip ) {
		G_TouchTriggers( ent );
	}

	// NOTE: now copy the exact origin over otherwise clients can be snapped into solid
	VectorCopy( ent->client->ps.origin, ent->r.currentOrigin );

	// touch other objects
	ClientImpacts( ent, &pm );

	// save results of triggers and client events
	if (ent->client->ps.eventSequence != oldEventSequence) {
		ent->eventTime = level.time;
	}

	// swap and latch button actions
	client->oldbuttons = client->buttons;
	client->buttons = ucmd->buttons;
	client->latched_buttons = client->buttons & ~client->oldbuttons;
//	client->latched_buttons |= client->buttons & ~client->oldbuttons;	// FIXME:? (SA) MP method (causes problems for us.  activate 'sticks')

	//----(SA)	added
	client->oldwbuttons = client->wbuttons;
	client->wbuttons = ucmd->wbuttons;
	client->latched_wbuttons = client->wbuttons & ~client->oldwbuttons;
//	client->latched_wbuttons |= client->wbuttons & ~client->oldwbuttons;	// FIXME:? (SA) MP method

	// Rafael - Activate
	// Ridah, made it a latched event (occurs on keydown only)
	if( client->latched_buttons & BUTTON_ACTIVATE ) {
		Cmd_Activate_f( ent );
	}

	if (ent->flags & FL_NOFATIGUE)
		ent->client->pmext.sprintTime = int(SPRINTTIME);

	if (g_entities[ent->client->ps.identifyClient].client &&
            (g_entities[ent->client->ps.identifyClient].team == ent->team ||
            (ent->client->ps.viewlocked == 7 && ent->client->ps.viewlocked_entNum == ent->client->ps.identifyClient))) {
		ent->client->ps.identifyClientHealth = g_entities[ent->client->ps.identifyClient].health;
	} else {
		ent->client->ps.identifyClient = -1;
		ent->client->ps.identifyClientHealth = 0;
	}

	// Omni-bot: used for class changes, bot will /kill 2 seconds before spawn
	Bot_Util_CheckForSuicide(ent);

	// check for respawning
	// Jaybird - added playdead check
	if( client->ps.stats[STAT_HEALTH] <= 0 && !(client->ps.eFlags & EF_PLAYDEAD)) {
		// DHM - Nerve

        WolfFindMedic( ent );
        if (ent->client->ps.viewlocked == 0)
            WolfFindKiller( ent );

		// See if we need to hop to limbo
		if( level.timeCurrent > client->respawnTime && !(ent->client->ps.pm_flags & PMF_LIMBO) ) {
			if( ucmd->upmove > 0 ) {
				if( g_gametype.integer == GT_WOLF_LMS || client->ps.persistant[PERS_RESPAWNS_LEFT] >= 0 ) {
					trap_SendServerCommand( ent-g_entities, "reqforcespawn" );
				} else {
					limbo( ent, ( client->ps.stats[STAT_HEALTH] > GIB_HEALTH ) ? qtrue : qfalse );
				}
			}

			if((g_forcerespawn.integer > 0 && level.timeCurrent - client->respawnTime > g_forcerespawn.integer * 1000) || client->ps.stats[STAT_HEALTH] <= GIB_HEALTH) {
				limbo(ent, (client->ps.stats[STAT_HEALTH] > GIB_HEALTH) ? qtrue : qfalse );
			}
		}

		return;
	}

	if( level.gameManager && level.timeCurrent - client->pers.lastHQMineReportTime > 20000 ) {
		if( level.gameManager->s.modelindex && client->sess.sessionTeam == TEAM_AXIS ) {
			if( G_SweepForLandmines( ent, 256.f, TEAM_AXIS ) ) {
				client->pers.lastHQMineReportTime = level.timeCurrent;
				trap_SendServerCommand(ent-g_entities, "cp \"Mines have been reported in this area.\" 1");
			}
		} else if( level.gameManager->s.modelindex2 && client->sess.sessionTeam == TEAM_ALLIES ) {
			if( G_SweepForLandmines( ent, 256.f, TEAM_ALLIES ) ) {
				client->pers.lastHQMineReportTime = level.timeCurrent;
				trap_SendServerCommand(ent-g_entities, "cp \"Mines have been reported in this area.\" 1");
			}
		}
	}

	// perform once-a-second actions
	if (level.match_pause == PAUSE_NONE) {
		ClientTimerActions( ent, msec );
		ClientTimerRegenCarryOver( ent, msec );
	}
}

bool G_AllowAntiwarp( int clientNum )
{
    gentity_t* ent = &g_entities[clientNum];

    // First check if antiwarp is even enabled
    if (!g_antiwarp.integer)
        return false;

    // Do not allow antiwarp when in intermission (screws up pings)
    if (cvars::gameState.ivalue == GS_INTERMISSION)
        return false;

    // Don't antiwarp when the player is in spec (there's no need to)
    if (ent->client->sess.sessionTeam == TEAM_SPECTATOR)
        return false;

    // Same goes for limbo
    if (ent->client->ps.pm_flags & PMF_LIMBO)
        return false;

    // Don't antiwarp bots (they are server-side, and thus will not miss commands)
    if (ent->r.svFlags & SVF_BOT)
        return false;

    // Don't antiwarp during map load
    if (ent->client->ps.pm_flags & PMF_TIME_LOAD)
        return qfalse; 

    // Don't antiwarp if they haven't been connected for 5 seconds
    // Note: this check is generally only triggered during mid-map
    // connects, because clients connect before loading the map.
    if ((level.time - ent->client->pers.connectTime) < 5000)
        return qfalse;

    return true;
}

/*
==================
ClientThink

A new command has arrived from the client
==================
*/
void ClientThink( int clientNum )
{
	gentity_t *ent = g_entities + clientNum;

    // Get the command
    usercmd_t cmd;
    trap_GetUsercmd(clientNum, &cmd);

    if (!G_AllowAntiwarp(clientNum)) {
        // If we're not using antiwarp, just run the command now
        // Allow ClientThink_real to adjust serverTime
	    ClientThink_cmd( clientNum, &cmd, false );
    } else {
        // First check if there is a duplicate serverTime
        // zinx recommends this
        Client& client = g_clientObjects[ent->s.number];
        if (client.cmdCount) {
            usercmd_t& oldcmd = client.cmdQueue.back();

            // If the times match, just return, thus dropping the command
            if (oldcmd.serverTime == cmd.serverTime)
                return;
        }

        // If we're using antiwarp, add it to the command queue
        // and then immediately process the queue (normal operation
        // will result in the command be executed immediately).
        G_AddUsercmd(clientNum, &cmd);
        G_ProcessAntiwarpQueue( ent );
    }
}

/*
===================
ClientThink_cmd

Assigns a new command to a user and runs it
===================
*/
void ClientThink_cmd(int clientNum, usercmd_t* cmd, bool skipServerTime)
{
    gentity_t* ent = &g_entities[clientNum];

    ent->client->pers.oldcmd = ent->client->pers.cmd;
    ent->client->pers.cmd = *cmd;

    ClientThink_real(ent, skipServerTime);
}

void G_RunClient( gentity_t *ent ) {
	// Gordon: special case for uniform grabbing
	if( ent->client->pers.cmd.buttons & BUTTON_ACTIVATE ) {
		Cmd_Activate2_f( ent );
	}

	if( ent->health <= 0 && ent->client->ps.pm_flags & PMF_LIMBO ) {
		if( ent->r.linked ) {
			trap_UnlinkEntity( ent );
		}
	}

    // Run the antiwarp queue
    if (g_antiwarp.integer)
        G_ProcessAntiwarpQueue(ent);
}

/*
==================
SpectatorClientEndFrame

==================
*/
void SpectatorClientEndFrame( gentity_t *ent )
{
	// OSP - specs periodically get score updates for useful demo playback info
	if(/*ent->client->pers.mvCount > 0 &&*/ ent->client->pers.mvScoreUpdate < level.time) {
		ent->client->pers.mvScoreUpdate = level.time + MV_SCOREUPDATE_INTERVAL;
		ent->client->wantsscore = qtrue;
//		G_SendScore(ent);
	}

	// if we are doing a chase cam or a remote view, grab the latest info
	if((ent->client->sess.spectatorState == SPECTATOR_FOLLOW) || (ent->client->ps.pm_flags & PMF_LIMBO)) {
		int clientNum, testtime;
		gclient_t *cl;
		qboolean do_respawn = qfalse; // JPW NERVE

		// Players can respawn quickly in warmup
		if(cvars::gameState.ivalue != GS_PLAYING && ent->client->respawnTime <= level.timeCurrent &&
		  ent->client->sess.sessionTeam != TEAM_SPECTATOR) {
			do_respawn = qtrue;
		} else if(ent->client->sess.sessionTeam == TEAM_AXIS) {
			testtime = (level.dwRedReinfOffset + level.timeCurrent - level.startTime) % g_redlimbotime.integer;
			do_respawn = (testtime < ent->client->pers.lastReinforceTime) ? qtrue : qfalse;
			ent->client->pers.lastReinforceTime = testtime;
		}
		else if (ent->client->sess.sessionTeam == TEAM_ALLIES) {
			testtime = (level.dwBlueReinfOffset + level.timeCurrent - level.startTime) % g_bluelimbotime.integer;
			do_respawn = (testtime < ent->client->pers.lastReinforceTime) ? qtrue : qfalse;
			ent->client->pers.lastReinforceTime = testtime;
		}

		if( g_gametype.integer != GT_WOLF_LMS ) {
			if ( ( g_maxlives.integer > 0 || g_alliedmaxlives.integer > 0 || g_axismaxlives.integer > 0 )
				&& ent->client->ps.persistant[PERS_RESPAWNS_LEFT] == 0 ) {
				if( do_respawn ) {
					if( g_maxlivesRespawnPenalty.integer ) {
						if( ent->client->ps.persistant[PERS_RESPAWNS_PENALTY] > 0 ) {
							ent->client->ps.persistant[PERS_RESPAWNS_PENALTY]--;
							do_respawn = qfalse;
						}
					} else {
						do_respawn = qfalse;
					}
				}
			}
		}

		if( g_gametype.integer == GT_WOLF_LMS && cvars::gameState.ivalue == GS_PLAYING ) {
			// Force respawn in LMS when nobody is playing and we aren't at the timelimit yet
			if( !level.teamEliminateTime &&
				level.numTeamClients[0] == level.numFinalDead[0] && level.numTeamClients[1] == level.numFinalDead[1] &&
				ent->client->respawnTime <= level.timeCurrent && ent->client->sess.sessionTeam != TEAM_SPECTATOR ) {
				do_respawn = qtrue;
			} else {
				do_respawn = qfalse;
			}
		}

		if ( do_respawn ) {
			reinforce(ent);
			return;
		}

		// Limbos aren't following while in MV
		if((ent->client->ps.pm_flags & PMF_LIMBO) && ent->client->pers.mvCount > 0) {
			return;
		}

		clientNum = ent->client->sess.spectatorClient;

		// team follow1 and team follow2 go to whatever clients are playing
		if ( clientNum == -1 ) {
			clientNum = level.follow1;
		} else if ( clientNum == -2 ) {
			clientNum = level.follow2;
		}

		if ( clientNum >= 0 ) {
			cl = &level.clients[ clientNum ];
			if(cl->pers.connected == CON_CONNECTED && cl->sess.sessionTeam != TEAM_SPECTATOR) {
				int flags = (cl->ps.eFlags & ~(EF_VOTED)) | (ent->client->ps.eFlags & (EF_VOTED));
				int ping = ent->client->ps.ping;

				if(ent->client->sess.sessionTeam != TEAM_SPECTATOR && (ent->client->ps.pm_flags & PMF_LIMBO)) {
					int savedScore = ent->client->ps.persistant[PERS_SCORE];
					int savedRespawns = ent->client->ps.persistant[PERS_RESPAWNS_LEFT];
					int savedRespawnPenalty = ent->client->ps.persistant[PERS_RESPAWNS_PENALTY];
					int savedClass = ent->client->ps.stats[STAT_PLAYER_CLASS];
					int savedMVList = ent->client->ps.powerups[PW_MVCLIENTLIST];

					do_respawn = (qboolean)ent->client->ps.pm_time;

					ent->client->ps = cl->ps;
					ent->client->ps.pm_flags |= PMF_FOLLOW;
					ent->client->ps.pm_flags |= PMF_LIMBO;

					ent->client->ps.pm_time = do_respawn;							// put pm_time back
					ent->client->ps.persistant[PERS_RESPAWNS_LEFT] = savedRespawns;
					ent->client->ps.persistant[PERS_RESPAWNS_PENALTY] = savedRespawnPenalty;
					ent->client->ps.persistant[PERS_SCORE] = savedScore;			// put score back
					ent->client->ps.powerups[PW_MVCLIENTLIST] = savedMVList;
					ent->client->ps.stats[STAT_PLAYER_CLASS] = savedClass;			// NERVE - SMF - put player class back
				} else {
					ent->client->ps = cl->ps;
					ent->client->ps.pm_flags |= PMF_FOLLOW;
				}

				// DHM - Nerve :: carry flags over
				ent->client->ps.eFlags = flags;
				ent->client->ps.ping = ping;

				return;
			} else {
				// drop them to free spectators unless they are dedicated camera followers
				if ( ent->client->sess.spectatorClient >= 0 ) {
					ent->client->sess.spectatorState = SPECTATOR_FREE;
					ClientBegin( ent->client - level.clients );
				}
			}
		}
	}

	/*if ( ent->client->sess.spectatorState == SPECTATOR_SCOREBOARD ) {
		ent->client->ps.pm_flags |= PMF_SCOREBOARD;
	} else {
		ent->client->ps.pm_flags &= ~PMF_SCOREBOARD;
	}*/

	// we are at a free-floating spec state for a player,
	// set speclock status, as appropriate
	//	 --> Can we use something besides a powerup slot?
	if(ent->client->pers.mvCount < 1) {
		ent->client->ps.powerups[PW_BLACKOUT] = (G_blockoutTeam(ent, TEAM_AXIS) * TEAM_AXIS) |
												(G_blockoutTeam(ent, TEAM_ALLIES) * TEAM_ALLIES);
	}
}


// DHM - Nerve :: After reviving a player, their contents stay CONTENTS_CORPSE until it is determined
//					to be safe to return them to PLAYERSOLID

qboolean StuckInClient( gentity_t *self ) {
	int i;
	vec3_t	hitmin, hitmax;
	vec3_t	selfmin, selfmax;
	gentity_t *hit;

	for(i=0; i<level.numConnectedClients; i++) {
		hit = g_entities + level.sortedClients[i];

		if(!hit->inuse || hit == self || !hit->client ||
		  !hit->s.solid || hit->health <= 0) {
			continue;
		}

		VectorAdd(hit->r.currentOrigin, hit->r.mins, hitmin);
		VectorAdd(hit->r.currentOrigin, hit->r.maxs, hitmax);
		VectorAdd(self->r.currentOrigin, self->r.mins, selfmin);
		VectorAdd(self->r.currentOrigin, self->r.maxs, selfmax);

		if(hitmin[0] > selfmax[0]) continue;
		if(hitmax[0] < selfmin[0]) continue;
		if(hitmin[1] > selfmax[1]) continue;
		if(hitmax[1] < selfmin[1]) continue;
		if(hitmin[2] > selfmax[2]) continue;
		if(hitmax[2] < selfmin[2]) continue;

		return(qtrue);
	}

	return(qfalse);
}

extern vec3_t	playerMins, playerMaxs;
#define WR_PUSHAMOUNT 25

void WolfRevivePushEnt( gentity_t *self, gentity_t *other ) {
	vec3_t	dir, push;

	VectorSubtract( self->r.currentOrigin, other->r.currentOrigin, dir );
	dir[2] = 0;
	VectorNormalizeFast( dir );

	VectorScale( dir, WR_PUSHAMOUNT, push );

	if ( self->client ) {
		VectorAdd( self->s.pos.trDelta, push, self->s.pos.trDelta );
		VectorAdd( self->client->ps.velocity, push, self->client->ps.velocity );
	}

	VectorScale( dir, -WR_PUSHAMOUNT, push );
	push[2] = WR_PUSHAMOUNT/2;

	VectorAdd( other->s.pos.trDelta, push, other->s.pos.trDelta );
	VectorAdd( other->client->ps.velocity, push, other->client->ps.velocity );
}

// Arnout: completely revived for capsules
void WolfReviveBbox( gentity_t *self ) {
	int			touch[MAX_GENTITIES];
	int			num,i, touchnum=0;
	gentity_t	*hit = NULL; // TTimo: init
	vec3_t		mins, maxs;

	hit = G_TestEntityPosition( self );

	if( hit && ( hit->s.number == ENTITYNUM_WORLD || ( hit->client && (hit->client->ps.persistant[PERS_HWEAPON_USE] || (hit->client->ps.eFlags & EF_MOUNTEDTANK))) ) ) {
		G_DPrintf( "WolfReviveBbox: Player stuck in world or MG42 using player\n" );
		// Move corpse directly to the person who revived them
		if ( self->props_frame_state >= 0 ) {
//			trap_UnlinkEntity( self );
			VectorCopy( g_entities[self->props_frame_state].client->ps.origin, self->client->ps.origin );
			VectorCopy( self->client->ps.origin, self->r.currentOrigin );
			trap_LinkEntity( self );

			// Reset value so we don't continue to warp them
			self->props_frame_state = -1;
		}
		return;
	}

	VectorAdd( self->r.currentOrigin, playerMins, mins );
	VectorAdd( self->r.currentOrigin, playerMaxs, maxs );

	num = trap_EntitiesInBox( mins, maxs, touch, MAX_GENTITIES );

	for (i=0 ; i<num ; i++) {
		hit = &g_entities[touch[i]];

		// Always use capsule for player
		if ( !trap_EntityContactCapsule( mins, maxs, hit ) ) {
		//if ( !trap_EntityContact( mins, maxs, hit ) ) {
			continue;
		}

		if ( hit->client && hit->health > 0 ) {
			if ( hit->s.number != self->s.number ) {
				WolfRevivePushEnt( hit, self );
				touchnum++;
			}
		} else if ( hit->r.contents & ( CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_PLAYERCLIP ) ) {
			WolfRevivePushEnt( hit, self );
			touchnum++;
		}
	}

	G_DPrintf( "WolfReviveBbox: Touchnum: %d\n", touchnum );

	if ( touchnum == 0 ) {
		G_DPrintf( "WolfReviveBbox:  Player is solid now!\n" );
		self->r.contents = CONTENTS_BODY;
	}
}

/*
==============
ClientEndFrame

Called at the end of each server frame for each connected client
A fast client will have multiple ClientThink for each ClientEndFrame,
while a slow client may have multiple ClientEndFrame between ClientThink.
==============
*/
void ClientEndFrame( gentity_t *ent ) {
	int			i;
//unlagged - smooth clients #1
	int frames;
//unlagged - smooth clients #1

	// used for informing of speclocked teams.
	// Zero out here and set only for certain specs
	ent->client->ps.powerups[PW_BLACKOUT] = 0;

	if (( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) || (ent->client->ps.pm_flags & PMF_LIMBO)) { // JPW NERVE
		SpectatorClientEndFrame( ent );
		return;
	}

		// turn off any expired powerups
		// OSP -- range changed for MV
		for ( i = 0 ; i < PW_NUM_POWERUPS ; i++ ) {

			if(	i == PW_FIRE ||				// these aren't dependant on level.time
				i == PW_ELECTRIC ||
				i == PW_BREATHER ||
				i == PW_NOFATIGUE ||
				ent->client->ps.powerups[i] == 0		// OSP
				|| i == PW_OPS_CLASS_1
				|| i == PW_OPS_CLASS_2
				|| i == PW_OPS_CLASS_3
				|| i == PW_OPS_DISGUISED
				) {

				continue;
			}
			// OSP -- If we're paused, update powerup timers accordingly.
			// Make sure we dont let stuff like CTF flags expire.
			if(level.match_pause != PAUSE_NONE &&
			  ent->client->ps.powerups[i] != INT_MAX) {
				ent->client->ps.powerups[i] += level.time - level.previousTime;
			}


			if ( ent->client->ps.powerups[ i ] < level.time ) {
				ent->client->ps.powerups[ i ] = 0;
			}
		}

		ent->client->ps.stats[STAT_XP] = 0;
		for( i = 0; i < SK_NUM_SKILLS; i++ ) {
			ent->client->ps.stats[STAT_XP] += int( ent->client->sess.skillpoints[i] );
		}

		// OSP - If we're paused, make sure other timers stay in sync
		//		--> Any new things in ET we should worry about?
		if(level.match_pause != PAUSE_NONE) {
			int time_delta = level.time - level.previousTime;

			ent->client->airOutTime += time_delta;
			ent->client->inactivityTime += time_delta;
			ent->client->lastBurnTime += time_delta;
			ent->client->pers.connectTime += time_delta;
			ent->client->pers.enterTime += time_delta;
			ent->client->pers.teamState.lastreturnedflag += time_delta;
			ent->client->pers.teamState.lasthurtcarrier += time_delta;
			ent->client->pers.teamState.lastfraggedcarrier += time_delta;
			ent->client->ps.classWeaponTime += time_delta;
//			ent->client->respawnTime += time_delta;
//			ent->client->sniperRifleFiredTime += time_delta;
			ent->lastHintCheckTime += time_delta;
			ent->pain_debounce_time += time_delta;
			ent->s.onFireEnd += time_delta;
		}

	// save network bandwidth
#if 0
	if ( !g_synchronousClients->integer && ent->client->ps.pm_type == PM_NORMAL ) {
		// FIXME: this must change eventually for non-sync demo recording
		VectorClear( ent->client->ps.viewangles );
	}
#endif

	//
	// If the end of unit layout is displayed, don't give
	// the player any normal movement attributes
	//
	if ( level.intermissiontime ) {
		return;
	}

	// burn from lava, etc
	P_WorldEffects (ent);

	// apply all the damage taken this frame
	P_DamageFeedback (ent);

//unlagged - smooth clients #1
	// this is handled differently now
/*
	// add the EF_CONNECTION flag if we haven't gotten commands recently
	if ( level.time - ent->client->lastCmdTime > 1000 ) {
		ent->s.eFlags |= EF_CONNECTION;
	} else {
		ent->s.eFlags &= ~EF_CONNECTION;
	}
*/
//unlagged - smooth clients #1

	ent->client->ps.stats[STAT_HEALTH] = ent->health;	// FIXME: get rid of ent->health...
															// Gordon: WHY? other ents use it.
	G_SetClientSound (ent);

	// set the latest information
	
    qboolean snap = (cvars::g_snap.ivalue & G_SNAP_CLIENT_ENDFRAME) ? qtrue : qfalse;
    BG_PlayerStateToEntityState( &ent->client->ps, &ent->s, level.time, snap );

	// DHM - Nerve :: If it's been a couple frames since being revived, and props_frame_state
	//					wasn't reset, go ahead and reset it
	if ( ent->props_frame_state >= 0 && ( (level.time - ent->s.effect3Time) > 100 ) )
		ent->props_frame_state = -1;

	if ( ent->health > 0 && StuckInClient( ent ) ) {
		G_DPrintf( "%s is stuck in a client.\n", ent->client->pers.netname );
		ent->r.contents = CONTENTS_CORPSE;
	}

	// Jaybird - added playdead check
	if ( ent->health > 0 && ent->r.contents == CONTENTS_CORPSE && !(ent->s.eFlags & EF_MOUNTEDTANK) && !(ent->s.eFlags & EF_PLAYDEAD)) {
		WolfReviveBbox( ent );
	}

	// DHM - Nerve :: Reset 'count2' for flamethrower
	if ( !(ent->client->buttons & BUTTON_ATTACK) )
		ent->count2 = 0;
	// dhm

	// zinx - #280 - run touch functions here too, so movers don't have to wait
	// until the next ClientThink, which will be too late for some map
	// scripts (railgun)
	G_TouchTriggers( ent );

	// run entity scripting
	G_Script_ScriptRun( ent );

//unlagged - smooth clients #1
	// mark as not missing updates initially
	ent->client->ps.eFlags &= ~EF_CONNECTION;

	// see how many frames the client has missed
	frames = level.framenum - ent->client->lastUpdateFrame - 1;

	// don't extrapolate more than two frames
	if ( frames > 2 ) {
		frames = 2;

		// if they missed more than two in a row, show the phone jack
		ent->client->ps.eFlags |= EF_CONNECTION;
		ent->s.eFlags |= EF_CONNECTION;
	}

	// did the client miss any frames?
	if ( !g_antiwarp.integer && frames > 0 && g_smoothClients.integer ) {
		// yep, missed one or more, so extrapolate the player's movement
		G_PredictPlayerMove( ent, (float)frames / sv_fps.integer );

        // save network bandwidth
        if (cvars::g_snap.ivalue & G_SNAP_CLIENT_EXTRAPOLATE)
            SnapVector( ent->s.pos.trBase );
        SnapVector( ent->s.pos.trDelta );
    }
//unlagged - smooth clients #1


	if (BULLETMODEL_MDXACTIVE || HITMODEL_MDXACTIVE) {
		// zinx - realistic hitboxes
		mdx_PlayerAnimation( ent );
	}

	G_TeamDamageRestriction( ent );
}

