/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "c352.h"
#include "../engine.h"
#include "../../ta-log.h"
#include <algorithm>
#include <utility>

namespace {
const char* regSheet[]={
  "CHx_FrontVolume", "000+x*8", "CHx_RearVolume", "001+x*8",
  "CHx_Frequency", "002+x*8", "CHx_Flags", "003+x*8",
  "CHx_Bank", "004+x*8", "CHx_Start", "005+x*8",
  "CHx_End", "006+x*8", "CHx_Loop", "007+x*8",
  "Control (unknown)", "200", "Execute keys", "202", NULL
};
}

DivPlatformC352::DivPlatformC352(): sampleMem(C352Core::ROM_SIZE,0),
  sampleMemLen(0), quadOutput(false) {
  std::fill(oscBuf,oscBuf+32,nullptr);
  std::fill(isMuted,isMuted+32,false);
  std::fill(regPool,regPool+0x203,0);
  core.setROM(sampleMem.data(),sampleMem.size());
  for (unsigned b=0; b<256; b++) mulawByte[uint16_t(core.mulaw[b])]=b;
}

DivPlatformC352::~DivPlatformC352() {
  samplePitchTable.destroy<Channel>(chan,32);
}

void DivPlatformC352::write(unsigned int address, unsigned short value) {
  if (skipRegisterWrites) return;
  writes.push_back({address,value});
  if (dumpWrites) addWrite(address,value);
}

void DivPlatformC352::flushWrites() {
  while (!writes.empty()) {
    const Write& w=writes.front();
    core.write(w.address,w.value);
    writes.pop_front();
  }
}

void DivPlatformC352::acquire(short** buf, size_t len) {
  for (auto* osc: oscBuf) osc->begin(len);
  for (size_t h=0; h<len; h++) {
    flushWrites();
    core.tick();
    if (quadOutput) {
      for (int o=0; o<4; o++) buf[o][h]=CLAMP(core.output[o],-32768,32767);
    } else {
      buf[0][h]=CLAMP(int(core.output[0])+core.output[2],-32768,32767);
      buf[1][h]=CLAMP(int(core.output[1])+core.output[3],-32768,32767);
    }
    for (int i=0; i<32; i++) {
      int value=0;
      for (int o=0; o<4; o++) value+=core.voiceOutput[i][o];
      oscBuf[i]->putSample(h,CLAMP(value/4,-32768,32767));
    }
  }
  for (auto* osc: oscBuf) osc->end(len);
}

void DivPlatformC352::tick(bool sysTick) {
  bool execute=false;
  for (int i=0; i<32; i++) {
    Channel& c=chan[i];
    c.std.next();
    if (c.std.vol.had) {
      c.outVol=(c.vol*CLAMP(c.std.vol.val,0,c.macroVolMul))/c.macroVolMul;
      c.volChanged=true;
    }
    if (NEW_ARP_STRAT) c.handleArp();
    else if (c.std.arp.had && !c.rawFreq) {
      if (!c.inPorta) c.baseFreq=c.calcBaseFreq(parent->calcArp(c.note,c.std.arp.val));
      c.freqChanged=true;
    }
    if (c.std.pitch.had) {
      c.pitch2=c.std.pitch.mode ? CLAMP(c.pitch2+c.std.pitch.val,-32768,32767) : c.std.pitch.val;
      c.freqChanged=true;
    }
    if (c.std.panL.had) {
      c.pan[0]=255*CLAMP(c.std.panL.val,0,c.macroPanMul)/c.macroPanMul;
      c.volChanged=true;
    }
    if (c.std.panR.had) {
      c.pan[1]=255*CLAMP(c.std.panR.val,0,c.macroPanMul)/c.macroPanMul;
      c.volChanged=true;
    }
    if (c.std.phaseReset.had && c.std.phaseReset.val==1 && c.active) {
      c.audPos=0;
      c.setPos=true;
    }
    if (c.setPos && c.active) c.keyOn=true;
    if (c.volChanged || c.keyOn) {
      int v[4];
      for (int o=0; o<4; o++) v[o]=CLAMP(c.outVol,0,255)*c.pan[o]/255;
      write(i*8,(v[0]<<8)|v[1]);
      write(i*8+1,(v[2]<<8)|v[3]);
      c.volChanged=false;
    }
    if (c.freqChanged || c.keyOn) {
      c.freq=CLAMP(c.calcFreq(),0,65535);
      write(i*8+2,c.freq);
      c.freqChanged=false;
    }
    if (c.keyOn) {
      const SampleRegion* s=(c.sample>=0 && size_t(c.sample)<regions.size()) ? &regions[c.sample] : nullptr;
      if (s && s->loaded && c.audPos>=0 && unsigned(c.audPos)<(s->loop ? s->loopEnd : s->length)) {
        const unsigned base=s->base;
        write(i*8+4,base>>16);
        write(i*8+5,(base+c.audPos)&0xffff);
        if (s->link) {
          write(i*8+6,(base+s->loopStart-1)&0xffff);
          write(i*8+7,s->link&0xffff);
        } else {
          write(i*8+6,(base+(s->loop ? s->loopEnd-1 : s->length))&0xffff);
          write(i*8+7,(base+s->loopStart)&0xffff);
        }
        const unsigned flags=C352Core::KEYON|(s->mulaw ? C352Core::MULAW : 0)|
          (s->loop ? C352Core::LOOP : 0)|(s->pingPong ? C352Core::REVERSE : 0)|
          (s->link ? C352Core::LINK : 0);
        write(i*8+3,flags);
        c.keyOff=false;
      } else {
        c.active=false;
        c.keyOff=true;
      }
      execute=true;
      c.keyOn=false;
    }
    if (c.keyOff) {
      write(i*8+3,C352Core::KEYOFF);
      execute=true;
      c.keyOff=false;
    }
    c.setPos=false;
    c.audPos=0;
  }
  if (execute) write(0x202,0);
}

void DivPlatformC352::renderSamples(int chipID) {
  std::fill(sampleMem.begin(),sampleMem.end(),0);
  regions.assign(parent->song.sampleLen,SampleRegion());
  sampleMemLen=0;
  memCompo=DivMemoryComposition();
  memCompo.name="Sample ROM";

  // 16-bit bodies hold the mu-law-decoded values.
  auto emit=[&](unsigned char* dst, DivSample* s, unsigned from, unsigned count) {
    if (s->depth==DIV_SAMPLE_DEPTH_16BIT) {
      for (unsigned j=0; j<count; j++) dst[j]=mulawByte[uint16_t(s->data16[from+j])];
    } else if (s->depth==DIV_SAMPLE_DEPTH_MULAW) {
      std::copy(s->dataMuLaw+from,s->dataMuLaw+from+count,dst);
    } else if (s->depth==DIV_SAMPLE_DEPTH_C219) {
      std::copy(s->dataC219+from,s->dataC219+from+count,dst);
    } else {
      std::copy(s->data8+from,s->data8+from+count,dst);
    }
  };

  std::vector<std::pair<size_t,size_t>> used;
  auto overlaps=[&](size_t s, size_t e) {
    for (const auto& u: used) if (s<u.second && e>u.first) return true;
    return false;
  };

  for (int i=0; i<parent->song.sampleLen; i++) {
    DivSample* s=parent->song.sample[i];
    if (!s->renderOn[0][chipID]) continue;
    unsigned length;
    if (s->depth==DIV_SAMPLE_DEPTH_16BIT) length=s->samples;
    else if (s->depth==DIV_SAMPLE_DEPTH_MULAW) length=s->lengthMuLaw;
    else if (s->depth==DIV_SAMPLE_DEPTH_C219) length=s->lengthC219;
    else length=s->length8;
    SampleRegion r;
    r.length=length;
    r.mulaw=s->depth==DIV_SAMPLE_DEPTH_16BIT||s->depth==DIV_SAMPLE_DEPTH_MULAW||s->depth==DIV_SAMPLE_DEPTH_C219;
    r.loop=s->loop;
    if (!length || (r.loop && (s->loopStart<0 || s->loopEnd<=s->loopStart || unsigned(s->loopEnd)>length))) {
      logW("C352: empty sample or invalid loop in sample %d",i);
      continue;
    }
    if (r.loop && s->loopMode==DIV_SAMPLE_LOOP_BACKWARD) {
      logW("C352: backward sample loops are not supported (%d)",i);
      continue;
    }
    if (r.loop && length>65536 && s->loopStart>0) {
      // intro and loop region in separate banks, both ending on the same low
      // 16 bits. the LINK jump then lands on the loop region.
      const unsigned L1=s->loopStart, L2=length-L1;
      bool placed=false;
      for (unsigned b=1; b<=0xff && !placed; b++) {
        const size_t s2=(size_t(b)<<16)|((b+L1-L2)&0xffff);
        if (s2+L2>sampleMem.size() || overlaps(s2,s2+L2)) continue;
        for (size_t base=0; base+L1<=sampleMem.size(); base+=0x10000) {
          const size_t s1=base+b;
          if (s1+L1>sampleMem.size()) break;
          if (overlaps(s1,s1+L1) || (s1<s2+L2 && s2<s1+L1)) continue;
          emit(sampleMem.data()+s1,s,0,L1);
          emit(sampleMem.data()+s2,s,L1,L2);
          used.emplace_back(s1,s1+L1);
          used.emplace_back(s2,s2+L2);
          r.base=s1;
          r.link=s2;
          r.length=L1;
          r.loopStart=L1;
          r.loopEnd=L1;
          r.loaded=true;
          regions[i]=r;
          sampleMemLen=std::max(sampleMemLen,std::max(s1+L1,s2+L2));
          memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample",i,s1,s1+L1));
          memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample (loop)",i,s2,s2+L2));
          placed=true;
          break;
        }
      }
      if (!placed) logW("C352: could not place sample %d",i);
      continue;
    }
    const unsigned size=length+(r.loop ? 0 : 1);
    if (length>65536 || size>65536) {
      logW("C352: sample %d does not fit a 64 KiB bank",i);
      continue;
    }
    size_t pos=sampleMemLen;
    if ((pos&0xffff)+size>65536) pos=(pos+65535)&~size_t(65535);
    if (pos+size>sampleMem.size()) {
      logW("C352: not enough ROM space for sample %d",i);
      continue;
    }
    r.base=pos;
    r.loopStart=r.loop ? s->loopStart : 0;
    r.loopEnd=r.loop ? s->loopEnd : length;
    r.pingPong=r.loop && s->loopMode==DIV_SAMPLE_LOOP_PINGPONG && r.loopEnd-r.loopStart>1;
    r.loaded=true;
    emit(sampleMem.data()+pos,s,0,length);
    used.emplace_back(pos,pos+length);
    regions[i]=r;
    sampleMemLen=pos+size;
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample",i,pos,sampleMemLen));
  }
  memCompo.used=sampleMemLen;
  memCompo.capacity=sampleMem.size();
}

void DivPlatformC352::reset() {
  writes.clear();
  core.reset();
  std::fill(regPool,regPool+0x203,0);
  for (int i=0; i<32; i++) {
    chan[i]=Channel(parent->song.compatFlags.linearPitch);
    chan[i].pitchTable=samplePitchTable.get(-1);
    chan[i].std.setEngine(parent);
    core.mute(i,isMuted[i]);
  }
}

void DivPlatformC352::forceIns() {
  for (auto& c: chan) { c.insChanged=true; c.freqChanged=true; c.volChanged=true; }
}
void DivPlatformC352::muteChannel(int ch, bool mute) {
  if (ch<0 || ch>=32) return;
  isMuted[ch]=mute;
  core.mute(ch,mute);
}
int DivPlatformC352::getOutputCount() { return quadOutput ? 4 : 2; }
bool DivPlatformC352::hasSoftPan(int ch) { return true; }
SharedChannel* DivPlatformC352::getChanState(int ch) { return &chan[ch]; }
DivMacroInt* DivPlatformC352::getChanMacroInt(int ch) { return &chan[ch].std; }
unsigned short DivPlatformC352::getPan(int ch) { return (chan[ch].pan[0]<<8)|chan[ch].pan[1]; }
DivDispatchOscBuffer* DivPlatformC352::getOscBuffer(int ch) { return oscBuf[ch]; }
unsigned char* DivPlatformC352::getRegisterPool() {
  for (unsigned i=0; i<0x203; i++) regPool[i]=core.read(i);
  return reinterpret_cast<unsigned char*>(regPool);
}
int DivPlatformC352::getRegisterPoolSize() { return 0x203; }
int DivPlatformC352::getRegisterPoolDepth() { return 16; }
const char** DivPlatformC352::getRegisterSheet() { return regSheet; }
float DivPlatformC352::getPostAmp() { return 6.0f; }
void DivPlatformC352::notifyInsChange(int ins) {
  for (auto& c: chan) if (c.ins==ins) c.insChanged=true;
}
void DivPlatformC352::notifyInsDeletion(void* ins) {
  for (auto& c: chan) c.std.notifyInsDeletion(static_cast<DivInstrument*>(ins));
}
void DivPlatformC352::notifyPitchTable(int sample) {
  samplePitchTable.update<Channel>(chan,32,parent->song.tuning,rate,65536.0,0xffff,false,parent->song.compatFlags.linearPitch,sample);
}
unsigned int DivPlatformC352::getMaxFreq(int ch) { return 0xffff; }
void DivPlatformC352::poke(unsigned int addr, unsigned short value) { write(addr,value); }
void DivPlatformC352::poke(std::vector<DivRegWrite>& list) {
  for (auto& w: list) write(w.addr,w.val);
}
const void* DivPlatformC352::getSampleMem(int index) { return index==0 ? sampleMem.data() : nullptr; }
size_t DivPlatformC352::getSampleMemCapacity(int index) { return index==0 ? sampleMem.size() : 0; }
size_t DivPlatformC352::getSampleMemUsage(int index) { return index==0 ? sampleMemLen : 0; }
bool DivPlatformC352::isSampleLoaded(int index, int sample) {
  return index==0 && sample>=0 && size_t(sample)<regions.size() && regions[sample].loaded;
}
const DivMemoryComposition* DivPlatformC352::getMemCompo(int index) { return index==0 ? &memCompo : nullptr; }
int DivPlatformC352::getClockRangeMin() { return MIN_CUSTOM_CLOCK; }
int DivPlatformC352::getClockRangeMax() { return MAX_CUSTOM_CLOCK; }
void DivPlatformC352::setFlags(const DivConfig& flags) {
  chipClock=25401600;
  CHECK_CUSTOM_CLOCK;
  rate=chipClock/288;
  quadOutput=flags.getBool("quadOutput",false);
  for (auto* osc: oscBuf) if (osc) osc->setRate(rate);
  notifyPitchTable();
}
int DivPlatformC352::init(DivEngine* p, int channels, int sugRate, const DivConfig& flags) {
  parent=p;
  samplePitchTable.init(parent);
  dumpWrites=false;
  skipRegisterWrites=false;
  for (int i=0; i<32; i++) {
    isMuted[i]=false;
    oscBuf[i]=new DivDispatchOscBuffer;
  }
  setFlags(flags);
  reset();
  return 32;
}
void DivPlatformC352::quit() {
  for (auto*& osc: oscBuf) { delete osc; osc=nullptr; }
}
int DivPlatformC352::dispatch(DivCommand c) {
  if (c.chan>=32) return 0;
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,DIV_INS_C352);
      chan[c.chan].macroVolMul=ins->type==DIV_INS_AMIGA?64:255;
      chan[c.chan].macroPanMul=ins->type==DIV_INS_AMIGA?127:255;
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].sample=ins->amiga.getSample(c.value);
        chan[c.chan].pitchTable=samplePitchTable.get(chan[c.chan].sample);
        chan[c.chan].sampleNote=c.value;
        c.value=ins->amiga.getFreq(c.value);
        chan[c.chan].sampleNoteDelta=c.value-chan[c.chan].sampleNote;
      }
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(c.value);
      }
      if (chan[c.chan].sample<0 || chan[c.chan].sample>=parent->song.sampleLen) {
        chan[c.chan].sample=-1;
        chan[c.chan].pitchTable=samplePitchTable.get(chan[c.chan].sample);
      }
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      chan[c.chan].active=true;
      chan[c.chan].keyOn=true;
      chan[c.chan].keyOff=false;
      chan[c.chan].macroInit(ins);
      if (!parent->song.compatFlags.brokenOutVol && !chan[c.chan].std.vol.will) {
        chan[c.chan].outVol=chan[c.chan].vol;
        chan[c.chan].volChanged=true;
      }
      break;
    }
    case DIV_CMD_NOTE_OFF:
      chan[c.chan].sample=-1;
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].keyOn=false;
      chan[c.chan].macroInit(NULL);
      break;
    case DIV_CMD_NOTE_OFF_ENV:
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].std.release();
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
      }
      break;
    case DIV_CMD_VOLUME:
      chan[c.chan].vol=CLAMP(c.value,0,255);
      if (!chan[c.chan].std.vol.has) {
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      chan[c.chan].volChanged=true;
      break;
    case DIV_CMD_GET_VOLUME:
      if (chan[c.chan].std.vol.has) {
        return chan[c.chan].vol;
      }
      return chan[c.chan].outVol;
      break;
    case DIV_CMD_PANNING:
      chan[c.chan].pan[0]=CLAMP(c.value,0,255);
      chan[c.chan].pan[1]=CLAMP(c.value2,0,255);
      chan[c.chan].volChanged=true;
      break;
    case DIV_CMD_SURROUND_PANNING:
      if (c.value>=0 && c.value<4) {
        chan[c.chan].pan[c.value]=CLAMP(c.value2,0,255);
        chan[c.chan].volChanged=true;
      }
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_NOTE_PORTA: {
      int destFreq=chan[c.chan].calcBaseFreq(c.value2+chan[c.chan].sampleNoteDelta);
      bool return2=false;
      if (destFreq>chan[c.chan].baseFreq) {
        chan[c.chan].baseFreq+=c.value;
        if (chan[c.chan].baseFreq>=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      } else {
        chan[c.chan].baseFreq-=c.value;
        if (chan[c.chan].baseFreq<=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      }
      chan[c.chan].freqChanged=true;
      if (return2) {
        chan[c.chan].inPorta=false;
        return 2;
      }
      break;
    }
    case DIV_CMD_LEGATO: {
      chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(c.value+chan[c.chan].sampleNoteDelta+((HACKY_LEGATO_MESS)?(chan[c.chan].std.arp.val-12):(0)));
      chan[c.chan].freqChanged=true;
      chan[c.chan].note=c.value;
      break;
    }
    case DIV_CMD_PRE_PORTA:
      if (chan[c.chan].active && c.value2) {
        if (parent->song.compatFlags.resetMacroOnPorta) chan[c.chan].macroInit(parent->getIns(chan[c.chan].ins,DIV_INS_C352));
      }
      if (!chan[c.chan].inPorta && c.value && !parent->song.compatFlags.brokenPortaArp && chan[c.chan].std.arp.will && !NEW_ARP_STRAT) chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(chan[c.chan].note);
      chan[c.chan].inPorta=c.value;
      break;
    case DIV_CMD_SAMPLE_POS:
      chan[c.chan].audPos=MAX(c.value,0);
      chan[c.chan].setPos=true;
      break;
    case DIV_CMD_GET_VOLMAX:
      return 255;
      break;
    case DIV_CMD_MACRO_OFF:
      chan[c.chan].std.mask(c.value,true);
      break;
    case DIV_CMD_MACRO_ON:
      chan[c.chan].std.mask(c.value,false);
      break;
    case DIV_CMD_MACRO_RESTART:
      chan[c.chan].std.restart(c.value);
      break;
    default:
      break;
  }
  return 1;
}
