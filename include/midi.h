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

#include "SDL_thread.h"

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
#define MIDI_DEVS 4
enum {MOUT_MPU,MOUT_SBUART,MOUT_GUS,MOUT_THRU};

enum {MDEV_MPU,MDEV_SBUART,MDEV_GUS,MDEV_SB16,MDEV_NONE};

void MIDI_RawOutByte(Bit8u data, Bit8u slot);
void MIDI_RawOutRTByte(Bit8u data);
void MIDI_RawOutThruRTByte(Bit8u data);
void MIDI_ClearBuffer(Bit8u slot);
bool MIDI_Available(void);

void MPU401_InputMsg(Bit8u msg[4]);
void SB_UART_InputMsg(Bit8u msg[4]);
void GUS_UART_InputMsg(Bit8u msg[4]);

Bits MPU401_InputSysex(Bit8u* buffer,Bitu len,bool abort);
Bits SB_UART_InputSysex(Bit8u* buffer,Bitu len,bool abort);
Bits GUS_UART_InputSysex(Bit8u* buffer,Bitu len,bool abort);

void MPU401_SetupTxHandler(void);
void MPU401_SetTx(bool status);

void SB16_MPU401_IrqToggle(bool status);

Bit32s MIDI_ToggleInputDevice(Bit32u device,bool status);

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
