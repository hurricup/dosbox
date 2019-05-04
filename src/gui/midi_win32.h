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


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <sstream>

void CALLBACK Win32_midiInCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	//LOG_MSG("wMsg:%x %x %x",wMsg, dwParam1, dwParam2);
	Bit8u msg[4] = {(Bit8u(dwParam1&0xff)),(Bit8u((dwParam1&0xff00)>>8)),
					(Bit8u((dwParam1&0xff0000)>>16)),MIDI_evt_len[(Bit8u(dwParam1&0xff))]};
	Bit8u *sysex;
	Bitu len;
	MIDIHDR *t_hdr;
	switch (wMsg) {
		case MM_MIM_DATA:  /* 0x3C3 - midi message */
			MIDI_InputMsg(msg);
			break;
		case MM_MIM_OPEN:  /* 0x3C1 */
			break;
		case MM_MIM_CLOSE: /* 0x3C2 */
			break;
		case MM_MIM_LONGDATA: /* 0x3C4 - sysex */
			t_hdr=(MIDIHDR*)dwParam1;
			sysex=(Bit8u*)t_hdr->lpData; 
			len=(Bitu)t_hdr->dwBytesRecorded;
			{
				Bitu cnt=5;
				while (cnt) { //abort if timed out
					Bitu ret = Bitu(MIDI_InputSysex(sysex,len,false));
					if (!ret) {len=0;break;}
					if (len==ret) cnt--; else cnt=5;
					sysex+=len-ret;
					len=ret;
					Sleep(5);//msec
				}
				if (len) MIDI_InputSysex(sysex,0,false);
			}
			midiInUnprepareHeader(hMidiIn,t_hdr,sizeof(*t_hdr));
			t_hdr->dwBytesRecorded = 0 ;
			midiInPrepareHeader(hMidiIn,t_hdr,sizeof(*t_hdr));
			break;
		case MM_MIM_ERROR:
		case MM_MIM_LONGERROR:
			break;
		default:
			LOG(LOG_MISC, LOG_NORMAL) ("MIDI: Unhandled input type %x",wMsg);
	}
};

class MidiHandler_win32: public MidiHandler {
private:
	HMIDIOUT m_out;
	HMIDIIN m_in;
	MIDIHDR m_hdr;
	MIDIHDR m_inhdr;
	HANDLE m_event;
	bool isOpen;
	bool isOpenInput;
public:
	MidiHandler_win32() : isOpen(false),isOpenInput(false),MidiHandler() {};
	const char * GetName(void) { return "win32";};
	bool Open(const char * conf) {
		if (isOpen) return false;
		m_event = CreateEvent (NULL, true, true, NULL);
		MMRESULT res = MMSYSERR_NOERROR;
		if(conf && *conf) {
			std::string strconf(conf);
			std::istringstream configmidi(strconf);
			unsigned int total = midiOutGetNumDevs();
			unsigned int nummer = total;
			configmidi >> nummer;
			if (configmidi.fail() && total) {
				lowcase(strconf);
				for(unsigned int i = 0; i< total;i++) {
					MIDIOUTCAPS mididev;
					midiOutGetDevCaps(i, &mididev, sizeof(MIDIOUTCAPS));
					std::string devname(mididev.szPname);
					lowcase(devname);
					if (devname.find(strconf) != std::string::npos) {
						nummer = i;
						break;
					}
				}
			}

			if (nummer < total) {
				MIDIOUTCAPS mididev;
				midiOutGetDevCaps(nummer, &mididev, sizeof(MIDIOUTCAPS));
				LOG_MSG("MIDI: win32 selected %s",mididev.szPname);
				res = midiOutOpen(&m_out, nummer, (DWORD_PTR)m_event, 0, CALLBACK_EVENT);
			}
		} else {
			res = midiOutOpen(&m_out, MIDI_MAPPER, (DWORD_PTR)m_event, 0, CALLBACK_EVENT);
		}
		if (res != MMSYSERR_NOERROR) return false;
		isOpen=true;
		return true;
	};
	bool OpenInput(const char *inconf) {
		if (isOpenInput) return false;
		MMRESULT res;
		if(inconf && *inconf) {
			std::string strinconf(inconf);
			std::istringstream configmidiin(strinconf);
			unsigned int nummer = midiInGetNumDevs();
			configmidiin >> nummer;
			if(nummer < midiInGetNumDevs()){
				MIDIINCAPS mididev;
				midiInGetDevCaps(nummer, &mididev, sizeof(MIDIINCAPS));
				LOG_MSG("MIDI:win32 selected input %s",mididev.szPname);
				res = midiInOpen (&m_in, nummer, (DWORD_PTR)Win32_midiInCallback, 0, CALLBACK_FUNCTION);
			}
		} else {
			res = midiInOpen(&m_in, MIDI_MAPPER, (DWORD_PTR)Win32_midiInCallback, 0, CALLBACK_FUNCTION);
		}
		if (res != MMSYSERR_NOERROR) return false;

		m_inhdr.lpData = (char*)&MIDI_InSysexBuf[0];
		m_inhdr.dwBufferLength = SYSEX_SIZE;
		m_inhdr.dwBytesRecorded = 0 ;
		m_inhdr.dwUser = 0;
		midiInPrepareHeader(m_in,&m_inhdr,sizeof(m_inhdr));
		midiInStart(m_in);
		isOpenInput=true;
		return true;
	};
	void Close(void) {
		if (isOpen) {
			isOpen=false;
			midiOutClose(m_out);
			CloseHandle (m_event);
		}
		if (isOpenInput) {
			isOpenInput=false;
			midiInStop(m_in);
			midiInClose(m_in);
		}
	};
	void PlayMsg(Bit8u * msg) {
		midiOutShortMsg(m_out, *(Bit32u*)msg);
	};
	void PlaySysex(Bit8u * sysex,Bitu len) {
		if (WaitForSingleObject (m_event, 2000) == WAIT_TIMEOUT) {
			LOG(LOG_MISC,LOG_ERROR)("Can't send midi message");
			return;
		}
		midiOutUnprepareHeader (m_out, &m_hdr, sizeof (m_hdr));

		m_hdr.lpData = (char *) sysex;
		m_hdr.dwBufferLength = len ;
		m_hdr.dwBytesRecorded = len ;
		m_hdr.dwUser = 0;

		MMRESULT result = midiOutPrepareHeader (m_out, &m_hdr, sizeof (m_hdr));
		if (result != MMSYSERR_NOERROR) return;
		ResetEvent (m_event);
		result = midiOutLongMsg (m_out,&m_hdr,sizeof(m_hdr));
		if (result != MMSYSERR_NOERROR) {
			SetEvent (m_event);
			return;
		}
	}
	void ListAll(Program* base) {
		unsigned int total = midiOutGetNumDevs();
		for(unsigned int i = 0;i < total;i++) {
			MIDIOUTCAPS mididev;
			midiOutGetDevCaps(i, &mididev, sizeof(MIDIOUTCAPS));
			base->WriteOut("%2d\t \"%s\"\n",i,mididev.szPname);
		}
	}
};

MidiHandler_win32 Midi_win32;


