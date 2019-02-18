/*
 *  Squeeze2cast - LMS to Alexa gateway
 *
 *  (c) Philippe 2019- philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezedefs.h"
#if WIN
#include <process.h>
#endif
#include "squeeze2alexa.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "util_common.h"
#include "log_util.h"
#include "util.h"
#include "mdnssd-itf.h"

#define DISCOVERY_TIME 	20
#define MAX_IDLE_TIME	(30*1000)

/*----------------------------------------------------------------------------*/
/* globals 																	  */
/*----------------------------------------------------------------------------*/
s32_t		glLogLimit = -1;
char		glUPnPSocket[128] = "?";
struct sMR	glMRDevices[MAX_RENDERERS];

log_level	slimproto_loglevel = lINFO;
log_level	slimmain_loglevel = lWARN;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lWARN;
log_level	main_loglevel = lINFO;
log_level	util_loglevel = lWARN;

tMRConfig			glMRConfig = {
							true,	// enabled
							false,	// stop_receiver
							1,      // volume_on_play
							true,	// send_metadata
							true,   // send_coverart
							false,	// autoplay
							0.5,	// media_volume
							0,		// remove_timeout
					};

static u8_t LMSVolumeMap[101] = {
				0, 1, 1, 1, 2, 2, 2, 3,  3,  4,
				5, 5, 6, 6, 7, 8, 9, 9, 10, 11,
				12, 13, 14, 15, 16, 16, 17, 18, 19, 20,
				22, 23, 24, 25, 26, 27, 28, 29, 30, 32,
				33, 34, 35, 37, 38, 39, 40, 42, 43, 44,
				46, 47, 48, 50, 51, 53, 54, 56, 57, 59,
				60, 61, 63, 65, 66, 68, 69, 71, 72, 74,
				75, 77, 79, 80, 82, 84, 85, 87, 89, 90,
				92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110,
				112, 113, 115, 117, 119, 121, 123, 125, 127, 128
			};

sq_dev_param_t glDeviceParam = {
					HTTP_NO_LENGTH, 	 		// stream_length
					 // both are multiple of 3*4(2) for buffer alignement on sample
					STREAMBUF_SIZE,			// stream_buffer_size
					OUTPUTBUF_SIZE,			// output_buffer_size
					"aac,ogg,flc,aif,pcm,mp3",		// codecs
					"thru",					// encode
					"wav",					// raw_audio_format
					"?",                    // server
					SQ_RATE_96000,          // sample_rate
					L24_PACKED_LPCM,		// L24_format
					FLAC_NORMAL_HEADER,	    // flac_header
					"",						// name
					22150,					// port
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,					// send_icy
#ifdef RESAMPLE
					"",						// resample_options
#endif
					false, 					// roon_mode
					"",						// store_prefix
					{ 	true,				// use_cli
						"" },   			// server
				} ;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 			*loglevel = &main_loglevel;
#if LINUX || FREEBSD
static bool					glDaemonize = false;
#endif
static bool					glMainRunning = true;
static pthread_t 			glMainThread, glmDNSsearchThread;
static struct mDNShandle_s	*glmDNSsearchHandle = NULL;
static char					*glLogFile;
static bool					glDiscovery = false;
static bool					glAutoSaveConfigFile = false;
static pthread_mutex_t		glMainMutex;
static pthread_cond_t		glMainCond;
static bool					glInteractive = true;
static char					*glPidFile = NULL;
static bool					glGracefullShutdown = true;
static void					*glConfigID = NULL;
static char					glConfigName[_STR_LEN_] = "./config.xml";

static char usage[] =

			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server[:port]>\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -b <address[:port]>]\tNetwork address and port to bind to\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|slimmain|stream|decode|output|main|util|cast, level: error|warn|info|debug|sdebug\n"
#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
#if FFMPEG
		   " FFMPEG"
#endif
#if RESAMPLE
		   " RESAMPLE"
#endif
#if CODECS
		   " CODECS"
#endif
#if LINKALL
		   " LINKALL"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n";

#define SET_LOGLEVEL(log) 		    \
	if (!strcmp(resp, #log"dbg")) { \
		char level[20];           	\
		i = scanf("%s", level);   	\
		log ## _loglevel = debug2level(level); \
	}

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *MRThread(void *args);
static void LoadAlexa(void);
static bool AddAlexaDevice(struct sMR *Device, char *Name, char *UDN);

// functions prefixed with _ require device's mutex to be locked
static void _SyncNotifyState(const char *State, struct sMR* Device);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, u8_t *cookie, void *param)
{
	struct sMR *Device = caller;
	char *p = (char*) param;
	bool rc = true;

	pthread_mutex_lock(&Device->Mutex);

	// this is async, so player might have been deleted
	if (!Device->Running) {
		pthread_mutex_unlock(&Device->Mutex);
		LOG_WARN("[%p] device has been removed", Device);
		return false;
	}

	if (action == SQ_ONOFF) {
		Device->on = *((bool*) param);

		LOG_INFO("[%p]: device set %s", caller, Device->on ? "ON" : "OFF");

		if (Device->on) {
			//CastPowerOn(Device->CastCtx);
			// candidate for busyraise/drop as it's using cli
			if (Device->Config.AutoPlay) sq_notify(Device->SqueezeHandle, Device, SQ_PLAY, NULL, &Device->on);
		} else {
			// cannot disconnect when LMS is configured for pause when OFF
			if (Device->sqState == SQ_STOP) {
				Device->IdleTimer = -1;
				//CastPowerOff(Device->CastCtx);
			}
		}
	}

	if (!Device->on && action != SQ_SETNAME && action != SQ_SETSERVER) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		pthread_mutex_unlock(&Device->Mutex);
		return false;
	}

	LOG_SDEBUG("callback for %s (%d)", Device->FriendlyName, action);

	switch (action) {

		case SQ_SET_TRACK: {
			struct track_param *p = (struct track_param*) param;

			NFREE(Device->NextURI);
			sq_free_metadata(&Device->NextMetaData);
			if (!Device->Config.SendCoverArt) NFREE(p->metadata.artwork);

			LOG_INFO("[%p]:\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n"
					 "\tduration:%d.%03d\n\tsize:%d\n\tcover:%s\n\toffset:%u", Device,
					p->metadata.artist, p->metadata.album, p->metadata.title,
					p->metadata.genre, div(p->metadata.duration, 1000).quot,
					div(p->metadata.duration,1000).rem, p->metadata.file_size,
					p->metadata.artwork ? p->metadata.artwork : "", p->offset);

			if (p->offset) {
				// to know what is expected next
				strcpy(Device->NextMime, p->mimetype);
				// this is a structure copy, pointers remains valid
				Device->NextMetaData = p->metadata;
				Device->NextURI = strdup(p->uri);
				LOG_INFO("[%p]: next URI %s", Device, Device->NextURI);
			} else {
				/*
				rc = CastLoad(Device->CastCtx, p->uri, p->mimetype, (
							  Device->Config.SendMetaData) ? &p->metadata : NULL);
				*/
#if !defined(REPOS_TIME)
				Device->StartTime = sq_get_time(Device->SqueezeHandle);
#endif
				sq_free_metadata(&p->metadata);
				LOG_INFO("[%p]: current URI %s", Device, p->uri);
			}
			break;
		}
		case SQ_UNPAUSE:
			// got it, don't need to send it more than once ...
			if (Device->sqState == SQ_PLAY) break;

/*
			if (Device->Config.VolumeOnPlay == 1 && Device->Volume != -1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx);
*/
			Device->sqState = SQ_PLAY;
			break;
		case SQ_PLAY:
			// got it, don't need to send it more than once ...
			if (Device->sqState == SQ_PLAY) break;

/*
			if (Device->Config.VolumeOnPlay == 1 && Device->Volume != -1)
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);

			CastPlay(Device->CastCtx);
*/
			Device->sqState = SQ_PLAY;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_STOP:
			//CastStop(Device->CastCtx);
			NFREE(Device->NextURI);
			sq_free_metadata(&Device->NextMetaData);
			Device->sqState = action;
			break;
		case SQ_PAUSE:
			//CastPause(Device->CastCtx);
			Device->sqState = action;
			Device->sqStamp = gettime_ms();
			break;
		case SQ_NEXT:
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			u16_t Volume = *(u16_t*)p;
			u32_t now = gettime_ms();
			int i;

			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);

			Device->Volume = (double) i / 100;
			LOG_INFO("Volume %d", i);

/*
			if (((Device->VolumeStamp + 1000) - now > 1000) &&
				(!Device->Config.VolumeOnPlay || (Device->Config.VolumeOnPlay == 1 && Device->sqState == SQ_PLAY)))
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);
*/

			break;
		}
		case SQ_SETNAME:
			strcpy(Device->sq_config.name, param);
			break;
		case SQ_SETSERVER:
			strcpy(Device->sq_config.dynamic.server, inet_ntoa(*(struct in_addr*) param));
			break;
		default:
			break;
	}

	pthread_mutex_unlock(&Device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
static void _SyncNotifyState(const char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;

	/*
	DEVICE MUTEX IS LOCKED
	*/

	if (!strcasecmp(State, "CLOSED") && Device->State != STOPPED) {
		Device->State = STOPPED;
		Param = true;
		Event = SQ_STOP;
	}

	if (!strcasecmp(State, "BUFFERING") && Device->State != BUFFERING) {
		Event = SQ_TRANSITION;
		Device->State = BUFFERING;
	}

	if (!strcasecmp(State, "STOPPED") && Device->State != STOPPED) {
		LOG_INFO("[%p]: Cast stop", Device);
		if (Device->NextURI) {
			// fake a "SETURI" and a "PLAY" request
/*
			CastLoad(Device->CastCtx, Device->NextURI, Device->NextMime,
					  (Device->Config.SendMetaData) ? &Device->NextMetaData : NULL);

			CastPlay(Device->CastCtx);
*/

			LOG_INFO("[%p]: gapped transition %s", Device, Device->NextURI);

			sq_free_metadata(&Device->NextMetaData);
			NFREE(Device->NextURI);
#if !defined(REPOS_TIME)
			Device->StartTime = 0;
#endif
		} else {
			// Can be a user stop, an error or a normal stop
			Event = SQ_STOP;
		}

		Device->State = STOPPED;
	}

	if (!strcasecmp(State, "PLAYING") && Device->State != PLAYING) {
		LOG_INFO("[%p]: Cast playing", Device);
		switch (Device->sqState) {
		case SQ_PAUSE:
			Param = true;
		case SQ_PLAY:
			Event = SQ_PLAY;
			break;
		default:
			/*
			can be a local playing after stop or a N-1 playing after a quick
			sequence of "next" when a N stop has been sent ==> ignore it
			*/
			LOG_ERROR("[%p]: unhandled playing", Device);
			break;
		}

		Device->State = PLAYING;
	}

	if (!strcasecmp(State, "PAUSED")) {
		/*
		Cast devices start "paused" so there is a first status received with
		that state, even if the play request has been sent. Make sure that
		this is filtered out and that "pause" state are only taken into account
		when already playing
		*/
		if (Device->State == PLAYING) {
			// detect unsollicited pause, but do not confuse it with a fast pause/play
			if (Device->sqState != SQ_PAUSE && ((Device->sqStamp + 2000) - gettime_ms() > 2000)) Param = true;
			Event = SQ_PAUSE;
			LOG_INFO("%s: Cast pause", Device->FriendlyName);

			Device->State = PAUSED;
		}
	}

	// candidate for busyraise/drop as it's using cli
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Device, Event, NULL, &Param);
}


/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last = gettime_ms();
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	while (p->Running) {
		double Volume = -1;

		// context is valid until this thread ends, no deletion issue
		//data = GetTimedEvent(p->CastCtx, 500);
		elapsed = gettime_ms() - last;

		// need to protect against events from CC threads, not from deletion
		pthread_mutex_lock(&p->Mutex);

		LOG_SDEBUG("Cast thread timer %d", elapsed);

		// a message has been received
#if 0
		if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *url;
				const char *state = GetMediaItem_S(data, 0, "playerState");

				// so far, buffering and playing can be merged
				if (state && !strcasecmp(state, "PLAYING")) {
					_SyncNotifyState("PLAYING", p);
				}

				if (state && !strcasecmp(state, "PAUSED")) {
					_SyncNotifyState("PAUSED", p);
				}

				if (state && !strcasecmp(state, "IDLE")) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause) {
						if (p->State != STOPPED) p->IdleTimer = 0;
						_SyncNotifyState("STOPPED", p);
					}
				}

				/*
				Discard any time info unless we are confirmed playing. Cast
				devices seems to report time according to seekpoint, so in case
				difference is too large, it means that we have a LMS repositioning
				*/
				if (p->State == PLAYING && p->sqState == SQ_PLAY && CastIsMediaSession(p->CastCtx)) {
					u32_t elapsed = 1000L * GetMediaItem_F(data, 0, "currentTime");
					s32_t gap = elapsed - sq_self_time(p->SqueezeHandle);

					LOG_DEBUG("elapsed %u, self %u, gap %u", elapsed, sq_self_time(p->SqueezeHandle), abs(gap));
#if !defined(REPOS_TIME)
					// no time correction in case of flow ... huh
					if (!strstr(p->sq_config.mode, "flow") && p->StartTime > 500 && abs(gap) > 2000) {
						if (elapsed > p->StartTime)	elapsed -= p->StartTime;
						else elapsed = 0;
					}
#endif
					sq_notify(p->SqueezeHandle, p, SQ_TIME, NULL, &elapsed);
				}

				url = GetMediaInfoItem_S(data, 0, "contentId");
				if (url) sq_notify(p->SqueezeHandle, p, SQ_TRACK_INFO, NULL, (void*) url);

			}

			// check for volume at the receiver level, but only record the change
			if (type && !strcasecmp(type, "RECEIVER_STATUS")) {
				double volume;
				bool muted;

				if (!p->Group && GetMediaVolume(data, 0, &volume, &muted)) {
					if (volume != -1 && !muted && volume != p->Volume) Volume = volume;
				}
			}

			// now apply the volume change if any
			if (Volume != -1 && fabs(Volume - p->Volume) >= 0.01) {
				u16_t VolFix = Volume * 100 + 0.5;
				p->VolumeStamp = gettime_ms();
				LOG_INFO("[%p]: Volume local change %u (%0.4lf)", p, VolFix, Volume);
				// candidate for busyraise/drop as it's using cli
				sq_notify(p->SqueezeHandle, p, SQ_VOLUME, NULL, &VolFix);
			}

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) _SyncNotifyState("CLOSED", p);

			json_decref(data);
		}
#endif


		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			// if (p->State != STOPPED) CastGetMediaStatus(p->CastCtx);
		}

		if (p->State == STOPPED && p->IdleTimer != -1) {
			p->IdleTimer += elapsed;
			if (p->IdleTimer > MAX_IDLE_TIME) {
				p->IdleTimer = -1;
				// CastRelease(p->CastCtx);
				LOG_INFO("[%p]: Idle timeout, releasing cast device", p);
			}
		}

		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static void LoadAlexa(void)
{
	struct sMR *Device;
	int i, j;
	IXML_NodeList *list;

	list = ixmlDocument_getElementsByTagName((IXML_Document*) glConfigID, "device");

	for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
		char *UDN, *Name;
		IXML_Node *device, *node;
		char *Model;
		char *MimeCaps[] = {"audio/flac", "audio/mpeg", "audio/wav", "audio/ogg", "audio/aac", "audio/L16", NULL };

		device = ixmlNodeList_item(list, i);

		node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) device, "udn");
		node = ixmlNode_getFirstChild(node);
		UDN = (char*) ixmlNode_getNodeValue(node);

		node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) device, "name");
		node = ixmlNode_getFirstChild(node);
		Name = (char*) ixmlNode_getNodeValue(node);

		// device creation so search a free spot.
		for (j = 0; j < MAX_RENDERERS && glMRDevices[j].Running; j++);

		// no more room !
		if (j == MAX_RENDERERS) {
			LOG_ERROR("Too many Cast devices", NULL);
			break;
		}

		Device = glMRDevices + j;

		if (AddAlexaDevice(Device, Name, UDN) && !glDiscovery) {
			// create a new slimdevice
			Device->SqueezeHandle = sq_reserve_device(Device, Device->on, MimeCaps, &sq_callback);
			if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
			if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
				sq_release_device(Device->SqueezeHandle);
				Device->SqueezeHandle = 0;
				LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
			}
		}
	}
}


/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	while (glMainRunning) {

		pthread_mutex_lock(&glMainMutex);
		pthread_cond_reltimedwait(&glMainCond, &glMainMutex, 30*1000);
		pthread_mutex_unlock(&glMainMutex);

		if (glLogFile && glLogLimit != - 1) {
			u32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				u32_t Sum, BufSize = 16384;
				u8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool AddAlexaDevice(struct sMR *Device, char *Name, char *UDN)
{
	unsigned long mac_size = 6;

	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	Device->Magic 			= MAGIC;
	Device->TimeOut			= false;
	Device->IdleTimer		= -1;
	Device->SqueezeHandle 	= 0;
	Device->Running 		= true;
	Device->sqState 		= SQ_STOP;
	Device->State 			= STOPPED;
	Device->VolumeStamp    	= Device->TrackPoll = 0;
	Device->NextMime[0]	 	= '\0';
	Device->NextURI 		= NULL;

	strcpy(Device->UDN, UDN);
	strcpy(Device->FriendlyName, Name);

	memset(&Device->NextMetaData, 0, sizeof(metadata_t));

	if (Device->sq_config.roon_mode) {
		Device->on = true;
		Device->sq_config.dynamic.use_cli = false;
	} else Device->on = false;

	// optional
	Device->sqStamp = 0;
	Device->Volume = -1;
#if !defined(REPOS_TIME)
	Device->StartTime = 0;
#endif

	LOG_INFO("[%p]: adding renderer (%s)", Device, Name);

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size)) {
		u32_t hash = hash32(UDN);

		LOG_INFO("[%p]: creating MAC %x", Device, Device->FriendlyName, hash);
		memcpy(Device->sq_config.mac + 2, &hash, 4);
		memset(Device->sq_config.mac, 0xee, 2);
	}

	// virtual players duplicate mac address
	MakeMacUnique(Device);

	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	struct in_addr addr;
	char IPaddr[16] = "";
	unsigned Port = 0;
	int i;

	memset(&glMRDevices, 0, sizeof(glMRDevices));
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	if (!strstr(glUPnPSocket, "?")) sscanf(glUPnPSocket, "%[^:]:%u", IPaddr, &Port);

	if (*IPaddr) {
		addr.s_addr = inet_addr(IPaddr);
	} else {
		addr.s_addr = get_localhost(NULL);
		strcpy(IPaddr, inet_ntoa(addr));
	}

	if (!Port) Port = HTTP_DEFAULT_PORT;

	// start squeeze piece
	sq_init(IPaddr, Port);

	LOG_INFO("Binding to %s:%d", IPaddr, Port);

	// init mutex & cond no matter what
	pthread_mutex_init(&glMainMutex, 0);
	pthread_cond_init(&glMainCond, 0);
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

	InitSSL();

	/* start the main thread */
	pthread_create(&glMainThread, NULL, &MainThread, NULL);

	LoadAlexa();

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Stop(void)
{
	int i;

	LOG_INFO("stopping squeezelite devices ...", NULL);
	sq_stop();

	glMainRunning = false;

	LOG_DEBUG("terminate main thread ...", NULL);
	pthread_cond_signal(&glMainCond);
	pthread_join(glMainThread, NULL);
	pthread_mutex_destroy(&glMainMutex);
	pthread_cond_destroy(&glMainCond);
	for (i = 0; i < MAX_RENDERERS; i++) pthread_mutex_destroy(&glMRDevices[i].Mutex);

	EndSSL();

	if (glConfigID) ixmlDocument_free(glConfigID);

#if WIN
	winsock_close();
#endif

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;

	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
//			if (p->Running && p->sqState == SQ_PLAY) CastStop(p->CastCtx);
		}
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}

	Stop();
	exit(EXIT_SUCCESS);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdfpibc", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk", opt)) {
			optarg = NULL;
			optind += 1;
		} else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 'c':
			strcpy(glDeviceParam.store_prefix, optarg);
			break;
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'b':
			strcpy(glUPnPSocket, optarg);
			break;
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;

#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto"))	slimproto_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))	slimmain_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
				} else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

#if WIN
	winsock_init();
#endif

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting squeeze2cast version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_ERROR("Wrong GLIBC version, use -static build", NULL);
		exit(1);
	}

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}

	// just do device discovery and exit
	if (glDiscovery) {
		Start();
		sleep(DISCOVERY_TIME + 1);
		Stop();
		return(0);
	}

#if LINUX || FREEBSD
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		} else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (!Start()) {
		LOG_ERROR("Cannot start", NULL);
		strcpy(resp, "exit");
	}

	while (strcmp(resp, "exit")) {

#if LINUX || FREEBSD
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		SET_LOGLEVEL(stream);
		SET_LOGLEVEL(output);
		SET_LOGLEVEL(decode);
		SET_LOGLEVEL(slimproto);
		SET_LOGLEVEL(slimmain);
		SET_LOGLEVEL(main);
		SET_LOGLEVEL(util);

		if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			bool all = !strcmp(resp, "dumpall");

			for (i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u] [%p::%p]\n",
						p->FriendlyName, p->Running, Locked, p->State,
						p, sq_get_ptr(p->SqueezeHandle));
			}
		}
	}

	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




