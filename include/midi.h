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


#ifndef DOSBOX_MIDI_H
#define DOSBOX_MIDI_H

#ifndef DOSBOX_PROGRAMS_H
#include "programs.h"
#endif

class MidiHandler {
public:
	MidiHandler();
	virtual bool Open(const char * conf) { return true; };
	virtual bool OpenInput(const char * inconf) {return false;};
	virtual void Close(void) {};
	virtual void PlayMsg(Bit8u * /*msg*/) {};
	virtual void PlaySysex(Bit8u * /*sysex*/,Bitu /*len*/) {};
	virtual const char * GetName(void) { return "none"; };
	virtual void ListAll(Program * base) {};
	virtual ~MidiHandler() { };
	MidiHandler * next;
};


#define SYSEX_SIZE 8192
struct DB_Midi {
	Bit8u rt_buf[8];
		Bitu status[MIDI_DEVS];
		Bitu cmd_r;
	struct {
		Bitu len;
		Bitu pos;
		Bit8u buf[8];
	} cmd[MIDI_DEVS];
	struct {
		Bit8u buf[SYSEX_SIZE];
		Bitu used;
		Bitu delay;
		Bit32u start;
	} sysex[MIDI_DEVS];
	bool available;
	bool in_available;
	MidiHandler * handler;
	MidiHandler * in_handler;
	bool realtime;
	Bitu inputdev;
	bool autoinput;
	bool thruchan;
	bool clockout;
};

extern DB_Midi midi;

#endif
