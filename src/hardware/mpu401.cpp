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


#include <string.h>
#include "dosbox.h"
#include "inout.h"
#include "pic.h"
#include "setup.h"
#include "cpu.h"
#include "support.h"
#include "mixer.h"
#include "midi.h"

SDL_mutex * MPULock;

//metronome sound
#define MPU401_METRONOME 1

static void MPU401_Event(Bitu);
static void MPU401_Reset(void);
static void MPU401_ResetDone(Bitu);
static void MPU401_EOIHandler(Bitu val=0);
static void MPU401_EOIHandlerDispatch(void);
static void MPU401_IntelligentOut(Bitu data);
static void MPU401_WriteCommand(Bitu port,Bitu val,Bitu iolen);
static void MPU401_WriteData(Bitu port,Bitu val,Bitu iolen);
static void MPU401_MetronomeSound(bool accented);
static void MPU401_NotesOff(Bitu chan);
static void MPU401_RecQueueBuffer(Bit8u * buf, Bitu len,bool block);

#define MPU401_VERSION	0x15
#define MPU401_REVISION	0x01
#define MPU401_QUEUE 64
#define MPU401_INPUT_QUEUE 1024
#define MPU401_TIMECONSTANT (60000000/1000.0f)
#define MPU401_RESETBUSY 14.0f

//helpers
#define M_GETKEY key[key/32]&(1<<(key%32))
#define M_SETKEY key[key/32]|=(1<<(key%32))
#define M_DELKEY key[key/32]&=~(1<<(key%32))

enum MpuMode { M_UART,M_INTELLIGENT };
enum MpuDataType {T_OVERFLOW,T_MARK,T_MIDI_SYS,T_MIDI_NORM,T_COMMAND};
enum RecState { M_RECOFF,M_RECSTB,M_RECON };
static Bitu MPUClockBase[8]={48,72,96,120,144,168,192};
static Bit8u cth_data[16]={0,0,0,0,1,0,0,0,1,0,1,0,1,1,1,0};

static void MPU401_WriteData(Bitu port,Bitu val,Bitu iolen);

/* Messages sent to MPU-401 from host */
#define MSG_EOX	                        0xf7
#define MSG_OVERFLOW                    0xf8
#define MSG_MARK                        0xfc

/* Messages sent to host from MPU-401 */
#define MSG_MPU_OVERFLOW                0xf8
#define MSG_MPU_COMMAND_REQ             0xf9
#define MSG_MPU_END                     0xfc
#define MSG_MPU_CLOCK                   0xfd
#define MSG_MPU_ACK                     0xfe

static struct {
	bool intelligent;
	Bitu irq;
	bool midi_thru;
	IO_ReadHandleObject ReadHandler[2];
	IO_WriteHandleObject WriteHandler[2];

#ifdef MPU401_METRONOME
	struct {
		Bit8u emptyBuf[MIXER_BUFSIZE];
		Bit8u majorBeat[MIXER_BUFSIZE];
		Bit8u minorBeat[MIXER_BUFSIZE];
		MixerChannel * mixerChan;
		MixerObject mixObj;
		Bit32s duration;
		bool gen;
		bool accent;
	} metronome;
#endif

} mpuhw;

static struct {
	MpuMode mode;
	Bit8u queue[MPU401_QUEUE];
	Bitu queue_pos,queue_used;

	Bit8u rec_queue[MPU401_INPUT_QUEUE];
	Bitu rec_queue_pos,rec_queue_used;
	struct track {
		Bits counter;
		Bit8u value[3],sys_val;
		Bit8u length;
		MpuDataType type;
	} playbuf[8],condbuf;
	struct {
		bool wsd,wsm,wsd_start;
		bool run_irq,irq_pending;
		bool tx_ready;
		bool conductor,cond_req,cond_set;
		bool track_req;
		bool block_ack;
		bool playing;
		bool send_now;
		bool clock_to_host;
		bool sync_in;
		bool sysex_in_finished;
		bool rec_copy;
		RecState rec;
		bool eoi_scheduled;
		Bits data_onoff;
		Bitu command_byte,cmd_pending;
		Bit8u tmask,cmask,amask;
		Bit16u midi_mask;
		Bit16u req_mask;
		Bitu track,old_track;
		Bit8u last_rtcmd;
	} state;
	struct {
		Bit8u timebase,old_timebase;
		Bit8u tempo,old_tempo;
		Bit8u tempo_rel,old_tempo_rel;
		Bit8u tempo_grad;
		Bit8u cth_rate[4],cth_mode;
		Bit8u midimetro,metromeas;
		Bitu metronome_state;
		Bitu cth_counter,cth_old;
		Bitu rec_counter;
		Bit32s measure_counter,meas_old;
		Bitu metronome_counter;
		Bit32s freq;
		Bits ticks_in;
		float freq_mod;
		bool active;
		Bit8u cth_rate[4],cth_mode;
		Bit8u midimetro,metromeas;
		Bitu metronome_state;
		Bitu cth_counter,cth_old;
		Bitu rec_counter;
		Bit32s measure_counter,meas_old;
		Bitu metronome_counter;
		Bit32s freq;
		Bits ticks_in;
		float freq_mod;
		bool active;
	} clock;
	struct {
		bool all_thru,midi_thru,sysex_thru,commonmsgs_thru;
		bool modemsgs_in, commonmsgs_in, bender_in, sysex_in;
		bool allnotesoff_out;
		bool rt_affection, rt_out,rt_in;
		bool timing_in_stop;
		bool data_in_stop;
		bool rec_measure_end;
		Bit8u prchg_buf[16];
		Bit16u prchg_mask;
	} filter;
	Bitu ch_toref[16];
	struct {
		Bit8u chan;
		Bit32u key[4];
		Bit8u trmask;
		bool on;
	}chanref[5],inputref[16];
} mpu;

static void MPU401_ReCalcClock(void) {
	Bit32s maxtempo=240, mintempo=16;
	if (mpu.clock.timebase>=168) maxtempo=179;
	if (mpu.clock.timebase==144) maxtempo=208;
	if (mpu.clock.timebase>=120) mintempo=8;
	mpu.clock.freq=(Bit32u(mpu.clock.tempo*2*mpu.clock.tempo_rel))>>6;

	mpu.clock.freq=mpu.clock.timebase*
		(mpu.clock.freq<(mintempo*2) ? mintempo : 
		((mpu.clock.freq/2)<maxtempo?(mpu.clock.freq/2):maxtempo) );

	if (mpu.state.sync_in) {
		Bit32s freq= Bit32s(float(mpu.clock.freq)*mpu.clock.freq_mod);
		if (freq>mpu.clock.timebase*mintempo && freq<mpu.clock.timebase*maxtempo) 
			mpu.clock.freq=freq;
	}
}

static INLINE void MPU401_StartClock(void) {
	if (mpu.clock.active) return;
	if (!(mpu.state.clock_to_host || mpu.state.playing || mpu.state.rec==M_RECON)) return;
	mpu.clock.active=true;
	PIC_RemoveEvents(MPU401_Event);
	PIC_AddEvent(MPU401_Event,MPU401_TIMECONSTANT/mpu.clock.freq);
}

static void MPU401_StopClock(void) {
	if (mpu.state.playing || mpu.state.rec!=M_RECON || mpu.state.clock_to_host)  return;
	mpu.clock.active=false;
	PIC_RemoveEvents(MPU401_Event);
}

static INLINE void MPU401_RunClock(void) {
	if (!mpu.clock.active) return;
	PIC_RemoveEvents(MPU401_Event);
	PIC_AddEvent(MPU401_Event,MPU401_TIMECONSTANT/(mpu.clock.freq));
}

static INLINE void MPU401_QueueByte(Bit8u data) {
	if (mpu.state.block_ack) {mpu.state.block_ack=false;return;}
		mpu.state.irq_pending=true;
		PIC_ActivateIRQ(mpuhw.irq);
	}
	if (mpu.queue_used<MPU401_QUEUE) {
		Bitu pos=mpu.queue_used+mpu.queue_pos;
		if (pos>=MPU401_QUEUE) pos-=MPU401_QUEUE;
		mpu.queue_used++;
		mpu.queue[pos]=data;
	}
}

static void MPU401_RecQueueBuffer(Bit8u * buf, Bitu len,bool block) {
	if (block) {
		if (MPULock) SDL_mutexP(MPULock);
		else return;
	}
	Bitu cnt=0;
	while (cnt<len) {
		if (mpu.rec_queue_used<MPU401_INPUT_QUEUE) {
			Bitu pos=mpu.rec_queue_used+mpu.rec_queue_pos;
			if (pos>=MPU401_INPUT_QUEUE) {pos-=MPU401_INPUT_QUEUE;}
			mpu.rec_queue[pos]=buf[cnt];
			mpu.rec_queue_used++;
			if (!mpu.state.sysex_in_finished && buf[cnt]==MSG_EOX) {//finish sysex
				mpu.state.sysex_in_finished=true;
				break;
			}
			cnt++;
		}
	}
	if (mpu.queue_used==0) {
		if (mpu.state.rec_copy || mpu.state.irq_pending) {
			if (block && MPULock) SDL_mutexV(MPULock);
			return;
		}
		mpu.state.rec_copy=true;
		if (mpu.rec_queue_pos>=MPU401_INPUT_QUEUE) mpu.rec_queue_pos-=MPU401_INPUT_QUEUE;
		MPU401_QueueByte(mpu.rec_queue[mpu.rec_queue_pos]);
		mpu.rec_queue_used--;
		mpu.rec_queue_pos++;
	}
	if (block && MPULock) SDL_mutexV(MPULock);
}

static void MPU401_ClrQueue(void) {
	mpu.queue_used=0;
	mpu.queue_pos=0;
	mpu.rec_queue_used=0;
	mpu.rec_queue_pos=0;
	mpu.state.sysex_in_finished=true;
	mpu.state.irq_pending=false;
	SB16_MPU401_IrqToggle(false);
}

static Bitu MPU401_ReadStatus(Bitu port,Bitu iolen) {
	Bit8u ret=0x3f;	/* Bits 6 and 7 clear */
	if (mpu.state.cmd_pending) ret|=0x40;
	return (ret | (mpu.queue_used ? 0: 0x80));
}

static Bitu MPU401_ReadStatusTx(Bitu port,Bitu iolen) {
	return (0x3f | (mpu.queue_used ? 0: 0x80) | (mpu.state.tx_ready ? 0: 0x40));
}

//setup alternative status port handler for throttling
void MPU401_SetupTxHandler(void) {
	mpuhw.ReadHandler[1].Install(0x331,&MPU401_ReadStatusTx,IO_MB);
}

void MPU401_SetTx(bool status) {
	mpu.state.tx_ready=status;
}

static void MPU401_WriteCommand(Bitu port,Bitu val,Bitu iolen) {
	if (mpu.mode==M_UART && val!=0xff) return;
	if (mpu.state.reset) {
		if (mpu.state.cmd_pending || val!=0xff) {
			mpu.state.cmd_pending=val+1;
			return;
		}
		PIC_RemoveEvents(MPU401_ResetDone);
		mpu.state.reset=false;
	}
	//LOG(LOG_MISC,LOG_NORMAL)("MPU401:command %x",val);
	SDL_mutexP(MPULock);

	//hack:enable midi through after the first mpu401 command is written
	mpuhw.midi_thru=true;

	if (val<=0x2f) { /* Sequencer state */
		bool send_prchg=false;
		if ((val&0xf)<0xc) {
			switch (val&3) { /* MIDI realtime messages */
				case 1:
					mpu.state.last_rtcmd=0xfc;
					if (mpu.filter.rt_out) MIDI_RawOutRTByte(0xfc);
					mpu.clock.meas_old=mpu.clock.measure_counter;
					mpu.clock.cth_old=mpu.clock.cth_counter;
					break;
				case 2: 
					mpu.state.last_rtcmd=0xfa;
					if (mpu.filter.rt_out) MIDI_RawOutRTByte(0xfb);
					mpu.clock.measure_counter=mpu.clock.meas_old=0;
					mpu.clock.cth_counter=mpu.clock.cth_old=0;
					break;
				case 3:
					mpu.state.last_rtcmd=0xfc;
					if (mpu.filter.rt_out) MIDI_RawOutRTByte(0xfa);
					mpu.clock.measure_counter=mpu.clock.meas_old;
					mpu.clock.cth_counter=mpu.clock.cth_old;
					break;
			}
			switch (val&0xc) { /* Playing */
				case  0x4:	/* Stop */
					mpu.state.playing=false;
					MPU401_StopClock();
					for (Bitu i=0;i<16;i++) MPU401_NotesOff(i);
					mpu.filter.prchg_mask=0;
					break;
				case 0x8:	/* Start */
					LOG(LOG_MISC,LOG_NORMAL)("MPU-401:Intelligent mode playback");
					mpu.state.playing=true;
					MPU401_StartClock();
			}
			switch (val&0x30) { /* Recording */
				case 0: //check if it waited for MIDI RT command
					//if (val&8 && mpu.state.rec!=M_RECON) mpu.clock.rec_counter=0;
					if ((val&3)<2 || !mpu.filter.rt_affection || mpu.state.rec!=M_RECSTB) break;
					mpu.state.rec=M_RECON;
					MPU401_StartClock();
					if (mpu.filter.prchg_mask) send_prchg=true;
					break;
				case 0x10:  /* Stop */
					//if (val&8 && mpu.state.rec!=M_RECON) mpu.clock.rec_counter=0;
					mpu.state.rec=M_RECOFF;
					MPU401_StopClock();	
					MPU401_QueueByte(MSG_MPU_ACK);
					MPU401_QueueByte(mpu.clock.rec_counter);
					MPU401_QueueByte(MSG_MPU_END);
					mpu.filter.prchg_mask=0;
					mpu.clock.rec_counter=0;
					SDL_mutexV(MPULock);
					return;					
				case 0x20:  /* Start */
					LOG(LOG_MISC,LOG_NORMAL)("MPU-401: intelligent mode recording");
					if (!(mpu.state.rec==M_RECON)) {
						mpu.clock.rec_counter=0;
						mpu.state.rec=M_RECSTB;
					}
					if (mpu.state.last_rtcmd==0xfa || mpu.state.last_rtcmd==0xfb) {
						mpu.clock.rec_counter=0;
						mpu.state.rec=M_RECON;
						if (mpu.filter.prchg_mask) send_prchg=true;
						MPU401_StartClock();
					}
			}
 		}
		MPU401_QueueByte(MSG_MPU_ACK);
		//record counter hack: needed by Prism, but sent only on cmd 0x20/0x26 (or breaks Ballade)
		Bit8u rec_cnt=mpu.clock.rec_counter;
		if ((val==0x20 || val==0x26) && mpu.state.rec==M_RECON) 
			MPU401_RecQueueBuffer(&rec_cnt,1,false);

		if (send_prchg) for (Bitu i=0;i<16;i++)
			if (mpu.filter.prchg_mask&(1<<i)) {
				Bit8u recmsg[3]={mpu.clock.rec_counter,0xc0|i,mpu.filter.prchg_buf[i]};
				MPU401_RecQueueBuffer(recmsg,3,false);
				mpu.filter.prchg_mask&=~(1<<i);
			}
		SDL_mutexV(MPULock);
		return;
    }
	else if (val>=0xa0 && val<=0xa7) {	/* Request play counter */
		//if (mpu.state.cmask&(1<<(val&7)))
		MPU401_QueueByte(mpu.playbuf[val&7].counter);
	}
	else if (val>=0xd0 && val<=0xd7) {	/* Send data */
		mpu.state.old_track=mpu.state.track;
		mpu.state.track=val&7;
		mpu.state.wsd=true;
		mpu.state.wsm=false;
		mpu.state.wsd_start=true;
	}
	else if (val<0x80 && val>=0x40) { /* Set reference table channel */
		mpu.chanref[(val>>4)-4].on=true;
		mpu.chanref[(val>>4)-4].chan=val&0x0f;
		mpu.chanref[(val>>4)-4].trmask=0;
		for (Bitu i=0;i<4;i++) mpu.chanref[(val>>4)-4].key[i]=0;
		for (Bitu i=0;i<16;i++) if (mpu.ch_toref[i]==((val>>4)-4)) mpu.ch_toref[i]=4;
		mpu.ch_toref[val&0x0f]=(val>>4)-4;
	}
	else
	switch (val) {
		case 0x30: /* Configuration 0x30 - 0x39 */
			mpu.filter.allnotesoff_out=false;
			break;
		case 0x32:
			mpu.filter.rt_out=false;
			break;
		case 0x33:
			mpu.filter.all_thru=false;
			mpu.filter.commonmsgs_thru=false;
			mpu.filter.midi_thru=false;
			for (Bitu i=0;i<16;i++) {
				mpu.inputref[i].on=false;
				for (Bitu j=0;j<4;j++) mpu.inputref[i].key[j]=0;
			}
			break;
		case 0x34:
			mpu.filter.timing_in_stop=true;
			break;
		case 0x35:
			mpu.filter.modemsgs_in=true;
			break;
		case 0x37:
			mpu.filter.sysex_thru=true;
			break;
		case 0x38:
			mpu.filter.commonmsgs_in=true;
			break;
		case 0x39:
			mpu.filter.rt_in=true;
			break;
		case 0x3f:	/* UART mode */
			LOG(LOG_MISC,LOG_NORMAL)("MPU-401:Set UART mode %X",val);
			mpu.mode=M_UART;
			mpuhw.midi_thru=false;
			break;


		case 0x80:  /* Internal clock */
			if (mpu.clock.active && mpu.state.sync_in) {
				PIC_AddEvent(MPU401_Event,MPU401_TIMECONSTANT/(mpu.clock.freq));
				mpu.clock.freq_mod=1.0;
			}
			mpu.state.sync_in=false;
			break;
//		case 0x81:  /* Sync to tape signal */
		case 0x82:  /* Sync to MIDI */
			mpu.clock.ticks_in=0;
			mpu.state.sync_in=true;
			break;
		case 0x83: /* Metronome on without accents */
			mpu.clock.metronome_state=1;
			break;
		case 0x84: /* Metronome off */
			mpu.clock.metronome_state=0;
			break;
		case 0x85: /* Metronome on with accents */
			mpu.clock.metronome_state=2;
			break;
		case 0x86: case 0x87: /* Bender */
			mpu.filter.bender_in=bool(val&1);
			break;
		case 0x88: case 0x89:/* MIDI through */
			mpu.filter.midi_thru=bool(val&1);
			for (Bitu i=0;i<16;i++) {
				mpu.inputref[i].on=mpu.filter.midi_thru;
				if (!(val&1)) for (Bitu j=0;j<4;j++) mpu.inputref[i].key[j]=0;
			}
			break;
		case 0x8a: case 0x8b: /* Data in stop */
			mpu.filter.data_in_stop=bool(val&1);
			break;
		case 0x8c: case 0x8d: /* Send measure end */
			mpu.filter.rec_measure_end=bool(val&1);
			break;
		case 0x8e: case 0x8f: /* Conductor */
			mpu.state.cond_set=bool(val&1);
			break;
		case 0x90: case 0x91: /* Realtime affection */
			mpu.filter.rt_affection=bool(val&1);
			break;
//		case 0x92: /* Tape */
//		case 0x93:
		case 0x94: /* Clock to host */
			mpu.state.clock_to_host=false;
			MPU401_StopClock();
			break;
		case 0x95:
			mpu.state.clock_to_host=true;
			MPU401_StartClock();
			break;
		case 0x96: case 0x97: /* Sysex input allow */
			mpu.filter.sysex_in=bool(val&1);
			if (val&1) mpu.filter.sysex_thru=false;
			break;
		case 0x98:case 0x99:case 0x9a: case 0x9b: /* Reference tables on/off */ 
		case 0x9c:case 0x9d:case 0x9e: case 0x9f:
			mpu.chanref[(val-0x98)/2].on=bool(val&1);
			break;
		/* Commands 0xa# returning data */
		case 0xab:	/* Request and clear recording counter */
			MPU401_QueueByte(MSG_MPU_ACK);
			MPU401_QueueByte(0);
			SDL_mutexV(MPULock);
			return;
		case 0xac:	/* Request version */
			MPU401_QueueByte(MSG_MPU_ACK);
			MPU401_QueueByte(MPU401_VERSION);
			SDL_mutexV(MPULock);
			return;
		case 0xad:	/* Request revision */
			MPU401_QueueByte(MSG_MPU_ACK);
			MPU401_QueueByte(MPU401_REVISION);
			SDL_mutexV(MPULock);
			return;
		case 0xaf:	/* Request tempo */
			MPU401_QueueByte(MSG_MPU_ACK);
			MPU401_QueueByte(mpu.clock.tempo);
			SDL_mutexV(MPULock);
			return;
		case 0xb1:	/* Reset relative tempo */
			mpu.clock.tempo_rel=0x40;
			break;
		case 0xb8:	/* Clear play counters */
			mpu.state.last_rtcmd=0;
			for (Bitu i=0;i<8;i++) {
				mpu.playbuf[i].counter=0;
				mpu.playbuf[i].type=T_OVERFLOW;
			}
			mpu.condbuf.counter=0;
			mpu.condbuf.type=T_OVERFLOW;

			mpu.state.amask=mpu.state.tmask;
			mpu.state.conductor=mpu.state.cond_set;
			mpu.clock.cth_counter=mpu.clock.cth_old=0;
			mpu.clock.measure_counter=mpu.clock.meas_old=0;
			break;
		case 0xb9:	/* Clear play map */
			for (Bitu i=0;i<16;i++) MPU401_NotesOff(i);
			for (Bitu i=0;i<8;i++) {
				mpu.playbuf[i].counter=0;
				mpu.playbuf[i].type=T_OVERFLOW;
			}
			mpu.state.last_rtcmd=0;
			mpu.clock.cth_counter=mpu.clock.cth_old=0;
			mpu.clock.measure_counter=mpu.clock.meas_old=0;
			break;
		case 0xba: /* Clear record counter */
			mpu.clock.rec_counter=0;
			break;
		case 0xc2: case 0xc3: case 0xc4: /* Internal timebase */
		case 0xc5: case 0xc6: case 0xc7: case 0xc8:
			mpu.clock.timebase=MPUClockBase[val-0xc2];
			MPU401_ReCalcClock();
			break;
		case 0xdf:	/* Send system message */
			mpu.state.wsd=false;
			mpu.state.wsm=true;
			mpu.state.wsd_start=true;
			break;
		case 0xe0: case 0xe1: case 0xe2: case 0xe4: case 0xe6: /* Commands with data byte */
		case 0xe7: case 0xec: case 0xed: case 0xee: case 0xef:
			mpu.state.command_byte=val;
			break;
		case 0xff:	/* Reset MPU-401 */
			LOG(LOG_MISC,LOG_NORMAL)("MPU-401:Reset");
			if (CPU_Cycles > 5) { //It came from the desert wants a fast irq
				CPU_CycleLeft += CPU_Cycles;
				CPU_Cycles = 5;
			}
			MPU401_Reset();
			break;
		default:;
			//LOG(LOG_MISC,LOG_WARN)("MPU-401:Unhandled command %X",val);
	}
	MPU401_QueueByte(MSG_MPU_ACK);
	SDL_mutexV(MPULock);
}

static Bitu MPU401_ReadData(Bitu port,Bitu iolen) {
	Bit8u ret=MSG_MPU_ACK;
	SDL_mutexP(MPULock);
	if (mpu.queue_used) {
		if (mpu.queue_pos>=MPU401_QUEUE) mpu.queue_pos-=MPU401_QUEUE;
		ret=mpu.queue[mpu.queue_pos];
		mpu.queue_pos++;mpu.queue_used--;
	}

	if (mpu.mode==M_UART) {
		if (!mpu.queue_used) SB16_MPU401_IrqToggle(false);
		//else SB16_MPU401_IrqToggle(true);
		if (mpuhw.intelligent && !mpu.queue_used) PIC_DeActivateIRQ(mpuhw.irq);
		SDL_mutexV(MPULock);
		return ret;
	}
	if (mpu.state.rec_copy && !mpu.rec_queue_used) {
		mpu.state.rec_copy=false;
		MPU401_EOIHandler();
		SDL_mutexV(MPULock);
		//LOG(LOG_MISC,LOG_NORMAL)("MPU401:read data eoi %x", ret);
		return ret;
	}

	//copy from recording buffer
	if (!mpu.queue_used && mpu.rec_queue_used) {
		mpu.state.rec_copy=true;
		if (mpu.rec_queue_pos>=MPU401_INPUT_QUEUE) mpu.rec_queue_pos-=MPU401_INPUT_QUEUE;
		MPU401_QueueByte(mpu.rec_queue[mpu.rec_queue_pos]);
		mpu.rec_queue_pos++;mpu.rec_queue_used--;
	}
	if (!mpu.queue_used) PIC_DeActivateIRQ(mpuhw.irq);
	if (ret>=0xf0 && ret<=0xf7) { /* MIDI data request */
		mpu.state.track=ret&7;
		mpu.state.data_onoff=0;
		mpu.state.cond_req=false;
		mpu.state.track_req=true;
	}
	if (ret==MSG_MPU_COMMAND_REQ) {
		mpu.state.data_onoff=0;
		mpu.state.cond_req=true;
		if (mpu.condbuf.type!=T_OVERFLOW) {
			mpu.state.block_ack=true;
			MPU401_WriteCommand(0x331,mpu.condbuf.value[0],1);
			if (mpu.state.command_byte) MPU401_WriteData(0x330,mpu.condbuf.value[1],1);
            mpu.condbuf.type=T_OVERFLOW;
		}
	}
	if (ret==MSG_MPU_END || ret==MSG_MPU_CLOCK || ret==MSG_MPU_ACK || ret==MSG_MPU_OVERFLOW) {
		MPU401_EOIHandlerDispatch();
	}

	SDL_mutexV(MPULock);
	//LOG(LOG_MISC,LOG_NORMAL)("MPU401:read data %x", ret);
	return ret;
}

static void MPU401_WriteData(Bitu port,Bitu val,Bitu iolen) {
	if (mpu.mode==M_UART) {MIDI_RawOutByte(val);return;}
	switch (mpu.state.command_byte) {	/* 0xe# command data */
		case 0x00:
			break;
		case 0xe0:	/* Set tempo */
			mpu.state.command_byte=0;
			mpu.clock.tempo=val;
			return;
		case 0xe1:	/* Set relative tempo */
			mpu.state.command_byte=0;
			if (val!=0x40) //default value
				LOG(LOG_MISC,LOG_ERROR)("MPU-401:Relative tempo change not implemented");
			return;
		case 0xe7:	/* Set internal clock to host interval */
			mpu.state.command_byte=0;
			mpu.clock.cth_rate=val>>2;
			return;
		case 0xec:	/* Set active track mask */
			mpu.state.command_byte=0;
			mpu.state.tmask=val;
			return;
		case 0xed: /* Set play counter mask */
			mpu.state.command_byte=0;
			mpu.state.cmask=val;
			return;
		case 0xee: /* Set 1-8 MIDI channel mask */
			mpu.state.command_byte=0;
			mpu.state.midi_mask&=0xff00;
			mpu.state.midi_mask|=val;
			return;
		case 0xef: /* Set 9-16 MIDI channel mask */
			mpu.state.command_byte=0;
			mpu.state.midi_mask&=0x00ff;
			mpu.state.midi_mask|=((Bit16u)val)<<8;
			return;
		//case 0xe2:	/* Set graduation for relative tempo */
		//case 0xe4:	/* Set metronome */
		//case 0xe6:	/* Set metronome measure length */
		default:
			mpu.state.command_byte=0;
			return;
	}
	static Bitu length,cnt,posd;
	if (mpu.state.wsd) {	/* Directly send MIDI message */
		if (mpu.state.wsd_start) {
			mpu.state.wsd_start=0;
			cnt=0;
				switch (val&0xf0) {
					case 0xc0:case 0xd0:
						mpu.playbuf[mpu.state.channel].value[0]=val;
						length=2;
						break;
					case 0x80:case 0x90:case 0xa0:case 0xb0:case 0xe0:
						mpu.playbuf[mpu.state.channel].value[0]=val;
						length=3;
						break;
					case 0xf0:
						LOG(LOG_MISC,LOG_ERROR)("MPU-401:Illegal WSD byte");
						mpu.state.wsd=0;
						mpu.state.channel=mpu.state.old_chan;
						return;
					default: /* MIDI with running status */
						cnt++;
						MIDI_RawOutByte(mpu.playbuf[mpu.state.channel].value[0]);
				}
		}
		if (cnt<length) {MIDI_RawOutByte(val);cnt++;}
		if (cnt==length) {
			mpu.state.wsd=0;
			mpu.state.channel=mpu.state.old_chan;
		}
		return;
	}
	if (mpu.state.wsm) {	/* Directly send system message */
		if (val==MSG_EOX) {MIDI_RawOutByte(MSG_EOX);mpu.state.wsm=0;return;}
		if (mpu.state.wsd_start) {
			mpu.state.wsd_start=0;
			cnt=0;
			switch (val) {
				case 0xf2:{ length=3; break;}
				case 0xf3:{ length=2; break;}
				case 0xf6:{ length=1; break;}
				case 0xf0:{ length=0; break;}
				default:
					length=0;
			}
		}
		if (!length || cnt<length) {MIDI_RawOutByte(val);cnt++;}
		if (cnt==length) mpu.state.wsm=0;
		return;
	}
	if (mpu.state.cond_req) { /* Command */
		switch (mpu.state.data_onoff) {
			case -1:
				return;
			case  0: /* Timing byte */
				mpu.condbuf.vlength=0;
				if (val<0xf0) mpu.state.data_onoff++;
				else {
					mpu.state.data_onoff=-1;
					MPU401_EOIHandlerDispatch();
					return;
				}
				if (val==0) mpu.state.send_now=true;
				else mpu.state.send_now=false;
				mpu.condbuf.counter=val;
				break;
			case  1: /* Command byte #1 */
				mpu.condbuf.type=T_COMMAND;
				if (val==0xf8 || val==0xf9) mpu.condbuf.type=T_OVERFLOW;
				mpu.condbuf.value[mpu.condbuf.vlength]=val;
				mpu.condbuf.vlength++;
				if ((val&0xf0)!=0xe0) MPU401_EOIHandlerDispatch();
				else mpu.state.data_onoff++;
				break;
			case  2:/* Command byte #2 */
				mpu.condbuf.value[mpu.condbuf.vlength]=val;
				mpu.condbuf.vlength++;
				MPU401_EOIHandlerDispatch();
				break;
		}
		return;
	}
	switch (mpu.state.data_onoff) { /* Data */
		case   -1:
			return;
		case    0: /* Timing byte */
			if (val<0xf0) mpu.state.data_onoff=1;
			else {
				mpu.state.data_onoff=-1;
				MPU401_EOIHandlerDispatch();
				return;
			}
			if (val==0) mpu.state.send_now=true;
			else mpu.state.send_now=false;
			mpu.playbuf[mpu.state.channel].counter=val;
			break;
		case    1: /* MIDI */
			mpu.playbuf[mpu.state.channel].vlength++;
			posd=mpu.playbuf[mpu.state.channel].vlength;
			if (posd==1) {
				switch (val&0xf0) {
					case 0xf0: /* System message or mark */
						if (val>0xf7) {
							mpu.playbuf[mpu.state.channel].type=T_MARK;
							mpu.playbuf[mpu.state.channel].sys_val=val;
							length=1;
						} else {
							LOG(LOG_MISC,LOG_ERROR)("MPU-401:Illegal message");
							mpu.playbuf[mpu.state.channel].type=T_MIDI_SYS;
							mpu.playbuf[mpu.state.channel].sys_val=val;
							length=1;
						}
						break;
					case 0xc0: case 0xd0: /* MIDI Message */
						mpu.playbuf[mpu.state.channel].type=T_MIDI_NORM;
						length=mpu.playbuf[mpu.state.channel].length=2;
						break;
					case 0x80: case 0x90: case 0xa0:  case 0xb0: case 0xe0:
						mpu.playbuf[mpu.state.channel].type=T_MIDI_NORM;
						length=mpu.playbuf[mpu.state.channel].length=3;
						break;
					default: /* MIDI data with running status */
						posd++;
						mpu.playbuf[mpu.state.channel].vlength++;
						mpu.playbuf[mpu.state.channel].type=T_MIDI_NORM;
						length=mpu.playbuf[mpu.state.channel].length;
						break;
				}
			}
			if (!(posd==1 && val>=0xf0)) mpu.playbuf[mpu.state.channel].value[posd-1]=val;
			if (posd==length) MPU401_EOIHandlerDispatch();
	}
}

static void MPU401_IntelligentOut(Bit8u chan) {
	Bitu val;
	switch (mpu.playbuf[chan].type) {
		case T_OVERFLOW:
			break;
		case T_MARK:
			val=mpu.playbuf[chan].sys_val;
			if (val==0xfc) {
				MIDI_RawOutByte(val);
				mpu.state.amask&=~(1<<chan);
				mpu.state.req_mask&=~(1<<chan);
			}
			break;
		case T_MIDI_NORM:
			for (Bitu i=0;i<mpu.playbuf[chan].vlength;i++)
				MIDI_RawOutByte(mpu.playbuf[chan].value[i]);
			break;
		default:
			break;
	}
}

static void UpdateTrack(Bit8u chan) {
	MPU401_IntelligentOut(chan);
	if (mpu.state.amask&(1<<chan)) {
		mpu.playbuf[chan].vlength=0;
		mpu.playbuf[chan].type=T_OVERFLOW;
		mpu.playbuf[chan].counter=0xf0;
		mpu.state.req_mask|=(1<<chan);
	} else {
		if (mpu.state.amask==0 && !mpu.state.conductor) mpu.state.req_mask|=(1<<12);
	}
}

static void UpdateConductor(void) {
	if (mpu.condbuf.value[0]==0xfc) {
		mpu.condbuf.value[0]=0;
		mpu.state.conductor=false;
		mpu.state.req_mask&=~(1<<9);
		if (mpu.state.amask==0) mpu.state.req_mask|=(1<<12);
		return;
	}
	mpu.condbuf.vlength=0;
	mpu.condbuf.counter=0xf0;
	mpu.state.req_mask|=(1<<9);
}

static void MPU401_Event(Bitu val) {
	if (mpu.mode==M_UART) return;
	if (mpu.state.irq_pending) goto next_event;
	for (Bitu i=0;i<8;i++) { /* Decrease counters */
		if (mpu.state.amask&(1<<i)) {
			mpu.playbuf[i].counter--;
			if (mpu.playbuf[i].counter<=0) UpdateTrack(i);
		}
	}
	if (mpu.state.conductor) {
		mpu.condbuf.counter--;
		if (mpu.condbuf.counter<=0) UpdateConductor();
	}
	if (mpu.clock.clock_to_host) {
		mpu.clock.cth_counter++;
		if (mpu.clock.cth_counter >= mpu.clock.cth_rate) {
			mpu.clock.cth_counter=0;
			mpu.state.req_mask|=(1<<13);
		}
	}
	if (!mpu.state.irq_pending && mpu.state.req_mask) MPU401_EOIHandler();
next_event:
	Bitu new_time;
	if ((new_time=mpu.clock.tempo*mpu.clock.timebase)==0) return;
	PIC_AddEvent(MPU401_Event,MPU401_TIMECONSTANT/new_time);
}


static void MPU401_EOIHandlerDispatch(void) {
	if (mpu.state.send_now) {
		mpu.state.eoi_scheduled=true;
		PIC_AddEvent(MPU401_EOIHandler,0.06f); //Possible a bit longer
	}
	else if (!mpu.state.eoi_scheduled) MPU401_EOIHandler();
}

//Updates counters and requests new data on "End of Input"
static void MPU401_EOIHandler(Bitu val) {
	mpu.state.eoi_scheduled=false;
	if (mpu.state.send_now) {
		mpu.state.send_now=false;
		if (mpu.state.cond_req) UpdateConductor();
		else UpdateTrack(mpu.state.channel);
	}
	mpu.state.irq_pending=false;
	if (!mpu.state.playing || !mpu.state.req_mask) return;
	Bitu i=0;
	do {
		if (mpu.state.req_mask&(1<<i)) {
			QueueByte(0xf0+i);
			mpu.state.req_mask&=~(1<<i);
			break;
		}
	} while ((i++)<16);
}

static void MPU401_ResetDone(Bitu) {
	mpu.state.reset=false;
	if (mpu.state.cmd_pending) {
		MPU401_WriteCommand(0x331,mpu.state.cmd_pending-1,1);
		mpu.state.cmd_pending=0;
	}
}
static void MPU401_Reset(void) {
	PIC_DeActivateIRQ(mpu.irq);
	mpu.mode=(mpu.intelligent ? M_INTELLIGENT : M_UART);
	PIC_RemoveEvents(MPU401_EOIHandler);
	mpu.state.eoi_scheduled=false;
	mpu.state.wsd=false;
	mpu.state.wsm=false;
	mpu.state.conductor=false;
	mpu.state.cond_req=false;
	mpu.state.cond_set=false;
	mpu.state.playing=false;
	mpu.state.run_irq=false;
	mpu.state.irq_pending=false;
	mpu.state.cmask=0xff;
	mpu.state.amask=mpu.state.tmask=0;
	mpu.state.midi_mask=0xffff;
	mpu.state.data_onoff=-1;
	mpu.state.command_byte=0;
	mpu.state.block_ack=false;
	mpu.clock.tempo=mpu.clock.old_tempo=100;
	mpu.clock.timebase=mpu.clock.old_timebase=120;
	mpu.clock.tempo_rel=mpu.clock.old_tempo_rel=40;
	mpu.clock.tempo_grad=0;
	mpu.clock.clock_to_host=false;
	mpu.clock.cth_rate=60;
	mpu.clock.cth_counter=0;
	ClrQueue();
	mpu.state.req_mask=0;
	mpu.condbuf.counter=0;
	mpu.condbuf.type=T_OVERFLOW;
	for (Bitu i=0;i<8;i++) {mpu.playbuf[i].type=T_OVERFLOW;mpu.playbuf[i].counter=0;}
}

class MPU401:public Module_base{
private:
	IO_ReadHandleObject ReadHandler[2];
	IO_WriteHandleObject WriteHandler[2];
	bool installed; /*as it can fail to install by 2 ways (config and no midi)*/
public:
	MPU401(Section* configuration):Module_base(configuration){
		installed = false;
		Section_prop * section=static_cast<Section_prop *>(configuration);
		const char* s_mpu = section->Get_string("mpu401");
		if(strcasecmp(s_mpu,"none") == 0) return;
		if(strcasecmp(s_mpu,"off") == 0) return;
		if(strcasecmp(s_mpu,"false") == 0) return;
		if (!MIDI_Available()) return;
		/*Enabled and there is a Midi */
		installed = true;

		WriteHandler[0].Install(0x330,&MPU401_WriteData,IO_MB);
		WriteHandler[1].Install(0x331,&MPU401_WriteCommand,IO_MB);
		ReadHandler[0].Install(0x330,&MPU401_ReadData,IO_MB);
		ReadHandler[1].Install(0x331,&MPU401_ReadStatus,IO_MB);

		mpu.queue_used=0;
		mpu.queue_pos=0;
		mpu.mode=M_UART;
		mpu.irq=9;	/* Princess Maker 2 wants it on irq 9 */

		mpu.intelligent = true;	//Default is on
		if(strcasecmp(s_mpu,"uart") == 0) mpu.intelligent = false;
		if (!mpu.intelligent) return;
		/*Set IRQ and unmask it(for timequest/princess maker 2) */
		PIC_SetIRQMask(mpu.irq,false);
		MPU401_Reset();
	}
	~MPU401(){
		if(!installed) return;
		Section_prop * section=static_cast<Section_prop *>(m_configuration);
		if(strcasecmp(section->Get_string("mpu401"),"intelligent")) return;
		PIC_SetIRQMask(mpu.irq,true);
		}
};

static MPU401* test;

void MPU401_Destroy(Section* sec){
	delete test;
}

void MPU401_Init(Section* sec) {
	test = new MPU401(sec);
	sec->AddDestroyFunction(&MPU401_Destroy,true);
}
