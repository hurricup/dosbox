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


#define ALSA_PCM_OLD_HW_PARAMS_API
#define ALSA_PCM_OLD_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <string>
#include <sstream>
#define ADDR_DELIM	".:"

#if ((SND_LIB_MINOR >= 6) && (SND_LIB_MAJOR == 0)) || (SND_LIB_MAJOR >= 1)
#define snd_seq_flush_output(x) snd_seq_drain_output(x)
#define snd_seq_set_client_group(x,name)	/*nop */
#define my_snd_seq_open(seqp) snd_seq_open(seqp, "hw", SND_SEQ_OPEN_DUPLEX, 0)
#else
/* SND_SEQ_OPEN_OUT causes oops on early version of ALSA */
#define my_snd_seq_open(seqp) snd_seq_open(seqp, SND_SEQ_OPEN)
#endif

SDL_Thread * input_thread;
snd_seq_t * mySeqHandle;
int input_exit;

int input_handler(void * val) {
	snd_seq_event_t * ev_in = NULL;

#if SIZEOF_UNSIGNED_LONG == 8
	int shift = 3;
#else
	int shift = 2;
#endif

	Bit8u msg[4];
	Bitu * m_ptr = (Bitu*) msg;
	Bitu len;
	int tval,status,channel;
	Bit8u * sysex_pos;
	while (!input_exit) {
		status = snd_seq_event_input(mySeqHandle, &ev_in);
		if (input_exit) break;
		if (status < 0) continue;
		//LOG_MSG("Alsa sequencer event %d",ev_in->type);
		switch (ev_in->type) {
			case SND_SEQ_EVENT_NOTEON:
				channel = ev_in->data.note.channel;
				 *m_ptr = 
					((Bit32u(0x90 | ev_in->data.note.channel))) |
					((ev_in->data.note.note & 0xff)<<8) |
					((ev_in->data.note.velocity & 0xff)<<16) | 0x03000000 ;
				MIDI_InputMsg(msg);
				break;
			case SND_SEQ_EVENT_NOTEOFF:
				channel = ev_in->data.note.channel;
				 *m_ptr = 
					(0x80 | ev_in->data.note.channel) |
					((ev_in->data.note.note & 0xff)<<8) |
					((ev_in->data.note.velocity & 0xff)<<16) | 0x03000000;
				MIDI_InputMsg(msg);
				break;
			case SND_SEQ_EVENT_CONTROLLER:
				*m_ptr =
					(0xB0 | ev_in->data.note.channel) |
					((ev_in->data.control.param & 0xff)<<8) |
					((ev_in->data.control.value & 0xff)<<16) | 0x03000000 ;
				MIDI_InputMsg(msg);
				break;
			case SND_SEQ_EVENT_PGMCHANGE:
				*m_ptr =
					(0xC0 | ev_in->data.note.channel) |
					((ev_in->data.control.value & 0xff)<<8) | 0x02000000 ;
				MIDI_InputMsg(msg);
				break;
			case SND_SEQ_EVENT_PITCHBEND:
				tval = (int)ev_in->data.control.value + (int)0x2000;
				*m_ptr =
					(0xE0 | ev_in->data.control.channel) |
					((tval & 0x7F)<<8),
					((tval<<9) & 0x7f0000) | 0x03000000 ;
				MIDI_InputMsg(msg);
				break;
			case SND_SEQ_EVENT_SYSEX:
				len=ev_in->data.ext.len << shift;
				sysex_pos=(Bit8u *) ev_in->data.ext.ptr;
				{
					Bitu cnt=5;
					while (cnt) { //abort if timeout
						Bitu ret = Bitu(MIDI_InputSysex(sysex_pos,len,false));
						if (!ret) {len=0;break;}
						if (len==ret) cnt--; else cnt=5;
						sysex_pos+=len-ret;
						len=ret;
						usleep(5000);//usec
					}
					if (len) MIDI_InputSysex(sysex_pos,0,true);
				}
				break;
		}
	}
    return 0;
}

class MidiHandler_alsa : public MidiHandler {
private:
	snd_seq_event_t ev;
	snd_seq_t *seq_handle;
	int seq_client, seq_port;
	int my_client, my_port;

	int seqin_client, seqin_port;
	int myin_client, myin_port;

	void send_event(int do_flush) {
		snd_seq_ev_set_direct(&ev);
		snd_seq_ev_set_source(&ev, my_port);
		snd_seq_ev_set_dest(&ev, seq_client, seq_port);

		snd_seq_event_output(seq_handle, &ev);
		if (do_flush)
			snd_seq_flush_output(seq_handle);
	}

	bool parse_addr(const char *arg, int *client, int *port) {
		std::string in(arg);
		if(in.empty()) return false;

		if(in[0] == 's' || in[0] == 'S') {
			*client = SND_SEQ_ADDRESS_SUBSCRIBERS;
			*port = 0;
			return true;
		}

		if(in.find_first_of(ADDR_DELIM) == std::string::npos) return false;
		std::istringstream inp(in);
		int val1, val2; char c;
		if(!(inp >> val1)) return false;
		if(!(inp >> c   )) return false;
		if(!(inp >> val2)) return false;
		*client = val1; *port = val2;
		return true;
	}
public:
	MidiHandler_alsa() : MidiHandler() {};
	const char* GetName(void) { return "alsa"; }
	void PlaySysex(Bit8u * sysex,Bitu len) {
		snd_seq_ev_set_sysex(&ev, len, sysex);
		send_event(1);
	}

	void PlayMsg(Bit8u * msg) {
		ev.type = SND_SEQ_EVENT_OSS;

		ev.data.raw32.d[0] = msg[0];
		ev.data.raw32.d[1] = msg[1];
		ev.data.raw32.d[2] = msg[2];

		unsigned char chanID = msg[0] & 0x0F;
		switch (msg[0] & 0xF0) {
		case 0x80:
			snd_seq_ev_set_noteoff(&ev, chanID, msg[1], msg[2]);
			send_event(1);
			break;
		case 0x90:
			snd_seq_ev_set_noteon(&ev, chanID, msg[1], msg[2]);
			send_event(1);
			break;
		case 0xA0:
			snd_seq_ev_set_keypress(&ev, chanID, msg[1], msg[2]);
			send_event(1);
			break;
		case 0xB0:
			snd_seq_ev_set_controller(&ev, chanID, msg[1], msg[2]);
			send_event(1);
			break;
		case 0xC0:
			snd_seq_ev_set_pgmchange(&ev, chanID, msg[1]);
			send_event(0);
			break;
		case 0xD0:
			snd_seq_ev_set_chanpress(&ev, chanID, msg[1]);
			send_event(0);
			break;
		case 0xE0:{
				long theBend = ((long)msg[1] + (long)(msg[2] << 7)) - 0x2000;
				snd_seq_ev_set_pitchbend(&ev, chanID, theBend);
				send_event(1);
			}
			break;
		default:
			//Maybe filter out FC as it leads for at least one user to crash, but the entire midi stream has not yet been checked.
			LOG(LOG_MISC,LOG_WARN)("ALSA:Unknown Command: %02X %02X %02X", msg[0],msg[1],msg[2]);
			send_event(1);
			break;
		}
	}	

	void Close(void) {
		if (input_thread) input_exit=1;
		if (seq_handle)
			snd_seq_close(seq_handle);
		seq_handle=0;
	}

	bool Open(const char * conf) {
		char var[10];
		unsigned int caps;
		bool defaultport = true; //try 17:0. Seems to be default nowadays

		// try to use port specified in config file
		if (conf && conf[0]) { 
			safe_strncpy(var, conf, 10);
			if (!parse_addr(var, &seq_client, &seq_port)) {
				LOG_MSG("ALSA:Invalid alsa port %s", var);
				return false;
			}
			defaultport = false;
		}
		// default port if none specified
		else if (!parse_addr("65:0", &seq_client, &seq_port)) {
				LOG_MSG("ALSA:Invalid alsa port 65:0");
				return false;
		}

		if (my_snd_seq_open(&seq_handle)) {
			LOG_MSG("ALSA:Can't open sequencer");
			return false;
		}
	
		my_client = snd_seq_client_id(seq_handle);
		snd_seq_set_client_name(seq_handle, "DOSBOX");
		snd_seq_set_client_group(seq_handle, "input");
	
		caps = SND_SEQ_PORT_CAP_READ;
		if (seq_client == SND_SEQ_ADDRESS_SUBSCRIBERS)
			caps = ~SND_SEQ_PORT_CAP_SUBS_READ;
		my_port =
		          snd_seq_create_simple_port(seq_handle, "DOSBOX", caps,
		          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
		if (my_port < 0) {
			snd_seq_close(seq_handle);
			seq_handle=0;
			LOG_MSG("ALSA:Can't create ALSA port");
			return false;
		}
	
		if (seq_client != SND_SEQ_ADDRESS_SUBSCRIBERS) {
			/* subscribe to MIDI port */
			if (snd_seq_connect_to(seq_handle, my_port, seq_client, seq_port) < 0) {
				if (defaultport) { //if port "65:0" (default) try "17:0" as well
					seq_client = 17; seq_port = 0; //Update reported values
					if(snd_seq_connect_to(seq_handle,my_port,seq_client,seq_port) < 0) { //Try 128:0 Timidity port as well
//						seq_client = 128; seq_port = 0; //Update reported values
//						if(snd_seq_connect_to(seq_handle,my_port,seq_client,seq_port) < 0) {
						seq_client=128; seq_port=0; //try timidity defalt
						if (snd_seq_connect_to(seq_handle,my_port,seq_client,seq_port) < 0) {
							snd_seq_close(seq_handle);
							seq_handle=0;
							LOG_MSG("ALSA:Can't subscribe to MIDI port (65:0),  (17:0) nor (128:0)");
							return false;
						}
					}
				} else {
					snd_seq_close(seq_handle);
					seq_handle=0;
					LOG_MSG("ALSA:Can't subscribe to MIDI port (%d:%d)", seq_client, seq_port);
					return false;
				}
			}
		}

		LOG_MSG("ALSA:MIDI Client initialised [%d:%d]", seq_client, seq_port);
		return true;
	}
	bool OpenInput(const char * conf) {
		LOG_MSG("opening input...");
		char var[10];
		unsigned int caps;

		// try to use port specified in config file
		if (conf) {
			if  (conf[0]) { 
				safe_strncpy(var, conf, 10);
				if (parse_addr(var, &seqin_client, &seqin_port) < 0) {
					LOG_MSG("ALSA:Invalid input alsa port %s", var);
					return false;
				}
			}
			else seqin_client=-1;
		} else return false;

		bool openedOutput = true;

		if (!seq_handle) {
			if (my_snd_seq_open(&seq_handle)) {
				LOG_MSG("ALSA:Can't open sequencer for input");
				return false;
			}

			bool openedOutput = false;
			my_client = snd_seq_client_id(seq_handle);
			snd_seq_set_client_name(seq_handle, "DOSBOX");
			snd_seq_set_client_group(seq_handle, "input");
		}

		caps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;

		myin_port =
			snd_seq_create_simple_port(seq_handle, "DOSBOX_IN", caps,
					SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
					//SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
		if (myin_port < 0) {
			if (!openedOutput) {snd_seq_close(seq_handle);seq_handle=0;}
			LOG_MSG("ALSA:Can't create ALSA input port");
			return false;
		}

		if (seqin_client != SND_SEQ_ADDRESS_SUBSCRIBERS && seqin_client>=0) {
			/* subscribe to MIDI port */
			if (snd_seq_connect_to(seq_handle, myin_port, seqin_client, seqin_port) < 0) {
				LOG_MSG("ALSA:Can't subscribe to MIDI port (%d:%d) for input", seqin_client, seqin_port);
				seqin_client=-1;
				//return false;
			}
		}

		//start receiving thread
		input_exit=0;
		mySeqHandle = seq_handle;
		input_thread = SDL_CreateThread(input_handler, NULL);

		if (seqin_client>=0) LOG_MSG("ALSA:MIDI input client connected [%d:%d]", seqin_client, seqin_port);
		else LOG_MSG("ALSA:MIDI receiving client initialised");
		return true;
	}

};

MidiHandler_alsa Midi_alsa;
