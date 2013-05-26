/*
	Based up Neutrino-GUI - Tuxbox-Project
	Copyright (C) 2001 by Steffen Hehn 'McClean'

	volume bar - Neutrino-GUI
	Copyright (C) 2011-2013 M. Liebmann (micha-bbg)
	Copyright (C) 2012,2013 Stefan Seyfried

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public
	License along with this program; if not, write to the
	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
	Boston, MA  02110-1301, USA.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <global.h>
#include <neutrino.h>
#include <gui/infoclock.h>
#include <gui/keybind_setup.h>
#include <system/debug.h>
/* compat header from zapit/include */
#include <audio.h>
#include <system/settings.h>
#include <system/helpers.h>
#include <daemonc/remotecontrol.h>
#include <driver/volume.h>
#include <driver/display.h>
#include <gui/audiomute.h>
#include <gui/mediaplayer.h>
#include <zapit/zapit.h>
#ifdef ENABLE_GRAPHLCD // MARTII
#include <driver/nglcd.h>
#endif


#define VOLUME_SCRIPT	CONFIGDIR "/volume.sh"

extern CRemoteControl * g_RemoteControl;
extern cAudio * audioDecoder;

CVolume::CVolume()
{
	frameBuffer	= CFrameBuffer::getInstance();
	volscale 	= NULL;
	mute_ax		= 0;
	mute_ay		= 0;
	mute_dx		= 0;
	mute_dy		= 0;
	m_mode 		= CNeutrinoApp::getInstance()->getMode();
	channel_id	= 0;
	apid		= 0;
}

CVolume::~CVolume()
{
	delete volscale;
}

CVolume* CVolume::getInstance()
{
	static CVolume* Volume = NULL;
	if(!Volume)
		Volume = new CVolume();
	return Volume;
}

void CVolume::setvol(int vol)
{
	CZapit::getInstance()->SetVolume(vol);
}

void CVolume::setVolume(const neutrino_msg_t key, bool nowait)
{
	if (!g_RCInput) /* don't die... */
		return;
	neutrino_msg_t msg	= key;
	static bool do_vol = true; /* false if volume is handled by external script */
	int mode = CNeutrinoApp::getInstance()->getMode();
	
	if (msg <= CRCInput::RC_MaxRC) {
		if(m_mode != mode) {
			m_mode = mode;
			setVolume(msg);
			return;
		}
	}

	if (volscale){
		volscale->hide();
		delete volscale;
		volscale = NULL;
	}

	if (volscale == NULL){
		volscale = new CVolumeBar();
#ifdef MARTII
		if (g_settings.volume_pos != 6) // off
#endif
		volscale->paint();
	}

	neutrino_msg_data_t data;
	uint64_t timeoutEnd;
	int vol = g_settings.current_volume;

	do {
		if (msg <= CRCInput::RC_MaxRC) 
		{
			bool sub_chan_keybind = g_settings.mode_left_right_key_tv == SNeutrinoSettings::VOLUME
						&& g_RemoteControl && g_RemoteControl->subChannels.size() < 1;
#ifdef MARTII
			if ((msg == g_settings.key_volumeup || msg == g_settings.key_volumedown) ||
			    (sub_chan_keybind && (msg == CRCInput::RC_right || msg == CRCInput::RC_left))) {
				int dir = (msg == g_settings.key_volumeup || msg == CRCInput::RC_right) ? 1 : -1;
#else
			if ((msg == CRCInput::RC_plus || msg == CRCInput::RC_minus) ||
			    (sub_chan_keybind && (msg == CRCInput::RC_right || msg == CRCInput::RC_left))) {
				int dir = (msg == CRCInput::RC_plus || msg == CRCInput::RC_right) ? 1 : -1;
#endif
				if (my_system(2, VOLUME_SCRIPT, dir > 0 ? "up" : "down") == 0)
				{
					do_vol = false;
					/* clear all repeated events */
					neutrino_msg_t tmp = msg;
					while (msg == tmp)
						g_RCInput->getMsg(&tmp, &data, 0);
					if (tmp != CRCInput::RC_timeout)
						g_RCInput->postMsg(tmp, data);
				} else
					do_vol = true;
				if (CNeutrinoApp::getInstance()->isMuted() && (dir > 0 || g_settings.current_volume > 0)) {
					if (volscale){
						if (volscale->isPainted())
							volscale->hide();
						delete volscale;
						volscale = NULL;
					}
					if (do_vol) {
						CAudioMute::getInstance()->AudioMute(false, true);
						if (mode == CNeutrinoApp::mode_audio) {
							CAudioPlayerGui *cap = CMediaPlayerMenu::getInstance()->getPlayerInstance();
							if (cap != NULL)
								cap->refreshMuteIcon();
						}
						setVolume(msg);
						return;
					}
				}

				if (do_vol && !CNeutrinoApp::getInstance()->isMuted()) {
					/* current_volume is char, we need signed to catch v < 0 */
					int v = g_settings.current_volume;
					v += dir * g_settings.current_volume_step;
					if (v > 100)
						v = 100;
					else if (v < 1) {
						v = 0;
						g_settings.current_volume = 0;
						if (g_settings.show_mute_icon) {
							if (volscale) {
								if (volscale->isPainted())
									volscale->hide();
								delete volscale;
								volscale = NULL;
							}
							CAudioMute::getInstance()->AudioMute(true, true);
							if (mode == CNeutrinoApp::mode_audio) {
								CAudioPlayerGui *cap = CMediaPlayerMenu::getInstance()->getPlayerInstance();
									if (cap != NULL)
										cap->refreshMuteIcon();
							}
							setVolume(msg);
							return;
						}
					}
					g_settings.current_volume = v;
#ifdef ENABLE_GRAPHLCD // MARTII
					nGLCD::ShowVolume(false);
#endif
				}
			}
			else if (msg == CRCInput::RC_home)
				break;
			else {
				g_RCInput->postMsg(msg, data);
				break;
			}

			if (do_vol)
				setvol(g_settings.current_volume);
			timeoutEnd = CRCInput::calcTimeoutEnd(nowait ? 1 : 3);
		}
		else if (msg == NeutrinoMessages::EVT_VOLCHANGED) {
			timeoutEnd = CRCInput::calcTimeoutEnd(3);
		}
		else if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::unhandled) {
			g_RCInput->postMsg(msg, data);
			break;
		}

		if (volscale) {
			if(vol != g_settings.current_volume) {
				vol = g_settings.current_volume;
#ifdef MARTII
				if (g_settings.volume_pos != 6) // off
#endif
				volscale->repaintVolScale();
			}
		}

		CVFD::getInstance()->showVolume(g_settings.current_volume);
		if (msg != CRCInput::RC_timeout) {
			g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeoutEnd, true );
		}
	} while (msg != CRCInput::RC_timeout);

#ifdef ENABLE_GRAPHLCD // MARTII
	nGLCD::ShowVolume(false);
#endif
	if (volscale) {
		if (volscale->isPainted())
			volscale->hide();
		delete volscale;
		volscale = NULL;
	}
}

bool CVolume::changeNotify(const neutrino_locale_t OptionName, void * data)
{
	bool ret = false;
	if (ARE_LOCALES_EQUAL(OptionName, NONEXISTANT_LOCALE)) {
		int percent = *(int *) data;
		int vol =  CZapit::getInstance()->GetVolume();
		/* keep resulting volume = (vol * percent)/100 not more than 115 */
		if (vol * percent > 11500)
			percent = 11500 / vol;

		printf("CVolume::changeNotify: percent %d\n", percent);
		CZapit::getInstance()->SetPidVolume(channel_id, apid, percent);
		CZapit::getInstance()->SetVolumePercent(percent);
		*(int *) data = percent;
	}
	return ret;
}
