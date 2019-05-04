/*
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <algorithm>

#include "SDL.h"

#include "dosbox.h"
#include "midi.h"
#include "cross.h"
#include "support.h"
#include "setup.h"
#include "mapper.h"
#include "pic.h"
#include "hardware.h"
#include "timer.h"


INLINE void MIDI_InputMsg(Bit8u msg[4]);
INLINE Bits MIDI_InputSysex(Bit8u *sysex,Bitu len,bool abort);

static Bit8u MIDI_InSysexBuf[SYSEX_SIZE];
Bit8u MIDI_evt_len[256] = {
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x00
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x10
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x20
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x30
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x40
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x50
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x60
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x70

  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x80
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x90
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xa0
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xb0

  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xc0
  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xd0

  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xe0

  0,2,3,2, 0,0,1,0, 1,0,1,1, 1,0,1,0   // 0xf0
};

MidiHandler * handler_list = 0;

MidiHandler::MidiHandler(){
	next = handler_list;
	handler_list = this;
};

MidiHandler Midi_none;

/* Include different midi drivers, lowest ones get checked first for default */

#if defined(MACOSX)

#include "midi_coremidi.h"
#include "midi_coreaudio.h"

#elif defined (WIN32)

#include "midi_win32.h"

#else

#include "midi_oss.h"

#endif

#if defined (HAVE_ALSA)

#include "midi_alsa.h"

#endif

DB_Midi midi;

void MIDI_RawOutRTByte(Bit8u data) {
	if (!midi.realtime) return;
	if (!midi.clockout && data == 0xf8) return;
	midi.cmd_r=data<<24;
	midi.handler->PlayMsg((Bit8u*)&midi.cmd_r);
}

void MIDI_RawOutThruRTByte(Bit8u data) {
	if (midi.thruchan) MIDI_RawOutRTByte(data);
}

void MIDI_RawOutByte(Bit8u data, Bit8u slot) {
	if (midi.sysex[slot].start) {
		Bit32u passed_ticks = GetTicks() - midi.sysex[slot].start;
		if (passed_ticks < midi.sysex[slot].delay) SDL_Delay(midi.sysex[slot].delay - passed_ticks);
	}

	/* Test for a realtime MIDI message */
	if (data>=0xf8) {
		midi.rt_buf[0]=data;
		midi.handler->PlayMsg(midi.rt_buf);
		return;
	}
	/* Test for a active sysex tranfer */
	if (midi.status[slot]==0xf0) {
		if (!(data&0x80)) {
			if (midi.sysex[slot].used<(SYSEX_SIZE-1)) midi.sysex[slot].buf[midi.sysex[slot].used++]=data;
			return;
		} else {
			midi.sysex[slot].buf[midi.sysex[slot].used++] = 0xf7;

			if ((midi.sysex[slot].start) && (midi.sysex[slot].used >= 4) && (midi.sysex[slot].used <= 9) && (midi.sysex[slot].buf[1] == 0x41) && (midi.sysex[slot].buf[3] == 0x16)) {
				LOG(LOG_ALL,LOG_ERROR)("MIDI:Skipping invalid MT-32 SysEx midi message (too short to contain a checksum)");
			} else {
//				LOG(LOG_ALL,LOG_NORMAL)("Play sysex; address:%02X %02X %02X, length:%4d, delay:%3d", midi.sysex.buf[5], midi.sysex.buf[6], midi.sysex.buf[7], midi.sysex.used, midi.sysex.delay);
			midi.sysex[slot].buf[midi.sysex[slot].used++]=0xf7;
			midi.handler->PlaySysex(midi.sysex[slot].buf,midi.sysex[slot].used);
				if (midi.sysex[slot].start) {
					if (midi.sysex[slot].buf[5] == 0x7F) {
						midi.sysex[slot].delay = 290; // All Parameters reset
					} else if (midi.sysex[slot].buf[5] == 0x10 && midi.sysex[slot].buf[6] == 0x00 && midi.sysex[slot].buf[7] == 0x04) {
						midi.sysex[slot].delay = 145; // Viking Child
					} else if (midi.sysex[slot].buf[5] == 0x10 && midi.sysex[slot].buf[6] == 0x00 && midi.sysex[slot].buf[7] == 0x01) {
						midi.sysex[slot].delay = 30; // Dark Sun 1
					} else midi.sysex[slot].delay = (Bitu)(((float)(midi.sysex[slot].used) * 1.25f) * 1000.0f / 3125.0f) + 2;
					midi.sysex[slot].start = GetTicks();
				}
			}

			LOG(LOG_ALL,LOG_NORMAL)("Sysex message size %d", static_cast<int>(midi.sysex[slot].used));
			if (CaptureState & CAPTURE_MIDI) {
				/* don't capture from MIDI passthrough channel */
				if (slot!=MOUT_THRU) CAPTURE_AddMidi( true, midi.sysex[slot].used-1, &midi.sysex[slot].buf[1]);
			}
		}
	}
	if (data&0x80) {
		midi.status[slot]=data;
		midi.cmd[slot].pos=0;
		midi.cmd[slot].len=MIDI_evt_len[data];
		if (midi.status[slot]==0xf0) {
			midi.sysex[slot].buf[0]=0xf0;
			midi.sysex[slot].used=1;
		}
	}
	if (midi.cmd[slot].len) {
		midi.cmd[slot].buf[midi.cmd[slot].pos++]=data;
		if (midi.cmd[slot].pos >= midi.cmd[slot].len) {
			if (CaptureState & CAPTURE_MIDI) {
				if (slot!=MOUT_THRU) CAPTURE_AddMidi(false, midi.cmd[slot].len, midi.cmd[slot].buf);
			}
			if (midi.thruchan || slot!=MOUT_THRU) midi.handler->PlayMsg(midi.cmd[slot].buf);
			midi.cmd[slot].pos=1;		//Use Running status
		}
	}
}

//allow devices to catch input in autodetection mode
Bit32s MIDI_ToggleInputDevice(Bit32u device,bool status) {
	if (!midi.autoinput) return -1;
	if (midi.inputdev == device) {
		if (status==false) {
			midi.inputdev = MDEV_NONE;
			return 2;
		}
		return 1;
	}
	midi.inputdev = device;
	return 0;
}

INLINE void MIDI_InputMsg(Bit8u msg[4]) {
	switch (midi.inputdev) {
		case MDEV_MPU:
			MPU401_InputMsg(msg);
			break;
		case MDEV_SBUART:
			SB_UART_InputMsg(msg);
			break;
		case MDEV_GUS:
			GUS_UART_InputMsg(msg);
			break;
	}
}

INLINE Bits MIDI_InputSysex(Bit8u *sysex,Bitu len, bool abort) {
	switch (midi.inputdev) {
		case MDEV_MPU:
			return MPU401_InputSysex(sysex,len,abort);
		case MDEV_SBUART:
			return SB_UART_InputSysex(sysex,len,abort);
		case MDEV_GUS:
			return GUS_UART_InputSysex(sysex,len,abort);
		default:
			return 0;
	}
}

void MIDI_ClearBuffer(Bit8u slot) {
	midi.sysex[slot].used=0;
	midi.status[slot]=0x00;
	midi.cmd[slot].pos=0;
	midi.cmd[slot].len=0;
}

bool MIDI_Available(void)  {
	return midi.available;
}

class MIDI:public Module_base{
public:
	MIDI(Section* configuration):Module_base(configuration){
		Section_prop * section=static_cast<Section_prop *>(configuration);
		const char * dev = "";
		char * sel_indevice = 0;
		const char * dev_list=section->Get_string("mididevice");
		std::string fullconf=section->Get_string("midiconfig");
		const char * inconf=section->Get_string("inconfig");
		char * mflags=(char*)section->Get_string("midioptions");
		const char * conf = fullconf.c_str();

		midi.realtime = true;
		midi.inputdev = MDEV_MPU;
		midi.autoinput = true;
		midi.thruchan = false;
		midi.clockout = false;

		std::string devstr;
		std::string indevstr;

		while (dev_list!=NULL) {
			char * devtoken = strtok((char*)dev_list,",");
			if (devtoken==NULL) break;
			devstr = devtoken;
			devstr = devstr.erase(devstr.find_last_not_of(" ")+1);
			devstr = devstr.erase(0,devstr.find_first_not_of(" "));
			dev = (char*)devstr.c_str();

			devtoken = strtok(NULL,",");
			if (devtoken==NULL) break;
			indevstr = devtoken;
			indevstr = indevstr.erase(indevstr.find_last_not_of(" ")+1);
			indevstr = indevstr.erase(0,indevstr.find_first_not_of(" "));
			sel_indevice = (char*)indevstr.c_str();
			break;
		}

		if (mflags!=NULL) {
			char * mflagtoken = strtok(mflags,",");
			while (mflagtoken!=NULL) {
				std::string flag(mflagtoken);
				flag = flag.erase(flag.find_last_not_of(" ")+1);
				flag = flag.erase(0,flag.find_first_not_of(" "));
				const char * flag_w = flag.c_str();

				//input to internal device options
				if (!strcasecmp(flag_w,"autoinput")) midi.autoinput=true;
				if (!strcasecmp(flag_w,"inputmpu401")) {midi.inputdev = MDEV_MPU;midi.autoinput=false;}
				if (!strcasecmp(flag_w,"inputsbuart")) {midi.inputdev = MDEV_SBUART;midi.autoinput=false;}
				if (!strcasecmp(flag_w,"inputgus")) {midi.inputdev = MDEV_GUS;midi.autoinput=false;}

				if (!strcasecmp(flag_w,"norealtime")) midi.realtime = false;
				if (!strcasecmp(flag_w,"passthrough")) midi.thruchan = true;
				if (!strcasecmp(flag_w,"clockout")) midi.clockout = true;
				if (!strcasecmp(flag_w,"throttle")) MPU401_SetupTxHandler();
				//TODO:add remaining options
				mflagtoken = strtok(NULL,",");
			}
		}

		/* If device = "default" go for first handler that works */
		MidiHandler * handler;
//		MAPPER_AddHandler(MIDI_SaveRawEvent,MK_f8,MMOD1|MMOD2,"caprawmidi","Cap MIDI");
		for (Bitu slot=0;slot<MIDI_DEVS;slot++) {
			midi.status[slot]=0x00;
			midi.cmd[slot].pos=0;
			midi.cmd[slot].len=0;
			midi.sysex[slot].used=0;
			midi.sysex[slot].delay=0;
			midi.sysex[slot].start=0;
		if (fullconf.find("delaysysex") != std::string::npos) {
			midi.sysex[slot].start = GetTicks();
        }
		}
		if (!strcasecmp(dev,"none")) {
			if (sel_indevice) goto midiin;
			else return;
		}
		if (fullconf.find("delaysysex") != std::string::npos) {
			fullconf.erase(fullconf.find("delaysysex"));
			LOG_MSG("MIDI: Using delayed SysEx processing");
		}
		trim(fullconf);
		if (!strcasecmp(dev,"default")) goto getdefault;
		handler=handler_list;
		while (handler) {
			if (!strcasecmp(dev,handler->GetName())) {
				if (!handler->Open(conf)) {
					LOG_MSG("MIDI: Can't open device:%s with config:%s.",dev,conf);
					goto getdefault;
				}
				midi.handler=handler;
				midi.available=true;
				LOG_MSG("MIDI: Opened device:%s",handler->GetName());
				goto midiin;
			}
			handler=handler->next;
		}
		LOG_MSG("MIDI: Can't find device:%s, finding default handler.",dev);
getdefault:
		handler=handler_list;
		while (handler) {
			if (handler->Open(conf)) {
				midi.available=true;
				midi.handler=handler;
				LOG_MSG("MIDI: Opened device:%s",handler->GetName());
				goto midiin;
			}
			handler=handler->next;
		}
midiin:
		if (sel_indevice && !strcasecmp(sel_indevice,"none")) return;
		if (strcasecmp(inconf,"none")) {
			if ((sel_indevice) && strcasecmp(sel_indevice,"")) {
				MidiHandler * sel_handler=handler_list;
				while (sel_handler) {
					if (!strcasecmp(sel_indevice,sel_handler->GetName())) {
						if (sel_handler->OpenInput(inconf)) {
							LOG_MSG("MIDI:Opened input device:%s.",sel_handler->GetName());
							midi.in_available=true;
							midi.in_handler=sel_handler;
						}
						else LOG_MSG("MIDI:Can't open input device:%s.",sel_handler->GetName());
						return;
					}
					sel_handler=sel_handler->next;
				}
				LOG_MSG("MIDI:Can't find input device:%s.",sel_handler->GetName());
				return;
			}
			//first try to open same handler
			MidiHandler * output_handler=0;
			if (midi.available) {
					output_handler=handler;
					if (handler->OpenInput(inconf)) {
						LOG_MSG("MIDI:Opened input device:%s.",handler->GetName());
						midi.in_available=true;
						midi.in_handler=handler;
						return;
					}
			}
			handler=handler_list;
			while (handler) {
				if (output_handler!=handler)
					if (handler->OpenInput(inconf)) {
						LOG_MSG("MIDI:Opened input device:%s.",handler->GetName());
						midi.in_available=true;
						midi.in_handler=handler;
						return;
					}
				handler=handler->next;
			}
        }
	}
	~MIDI(){
		if (midi.in_available && midi.in_handler!=midi.handler) midi.in_handler->Close();
		if(midi.available) midi.handler->Close();
		midi.available = false;
		midi.in_available = false;
		midi.handler = 0;
		midi.in_handler = 0;
	}
};


static MIDI* test;
void MIDI_Destroy(Section* /*sec*/){
	delete test;
}
void MIDI_Init(Section * sec) {
	test = new MIDI(sec);
	sec->AddDestroyFunction(&MIDI_Destroy,true);
}
