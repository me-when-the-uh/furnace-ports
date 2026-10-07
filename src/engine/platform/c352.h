/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FURNACE_PLATFORM_C352_H
#define FURNACE_PLATFORM_C352_H

#include "../dispatch.h"
#include "sound/c352.h"
#include <deque>

class DivPlatformC352: public DivDispatch {
  struct Channel: public SharedChannel {
    int sample, audPos, macroVolMul, macroPanMul;
    int pan[4];
    bool setPos, volChanged;
    Channel(bool linear=true): SharedChannel(255,linear), sample(-1), audPos(0),
      macroVolMul(64), macroPanMul(127), pan{255,255,0,0}, setPos(false), volChanged(true) {}
  };
  struct SampleRegion {
    unsigned int base, length, loopStart, loopEnd, link;
    bool loaded, loop, pingPong, mulaw;
    SampleRegion(): base(0), length(0), loopStart(0), loopEnd(0), link(0),
      loaded(false), loop(false), pingPong(false), mulaw(false) {}
  };
  struct Write { unsigned int address; unsigned short value; };
  Channel chan[32];
  DivDispatchOscBuffer* oscBuf[32];
  bool isMuted[32];
  C352Core core;
  DivPitchTableManager samplePitchTable;
  unsigned char mulawByte[65536];
  std::vector<unsigned char> sampleMem;
  std::vector<SampleRegion> regions;
  size_t sampleMemLen;
  DivMemoryComposition memCompo;
  unsigned short regPool[0x203];
  std::deque<Write> writes;
  bool quadOutput;
  void write(unsigned int address, unsigned short value);
  void flushWrites();

public:
  DivPlatformC352();
  ~DivPlatformC352();
  void acquire(short** buf, size_t len) override;
  int dispatch(DivCommand c) override;
  void tick(bool sysTick=true) override;
  void reset() override;
  void forceIns() override;
  void muteChannel(int ch, bool mute) override;
  int getOutputCount() override;
  bool hasSoftPan(int ch) override;
  SharedChannel* getChanState(int ch) override;
  DivMacroInt* getChanMacroInt(int ch) override;
  unsigned short getPan(int ch) override;
  DivDispatchOscBuffer* getOscBuffer(int ch) override;
  unsigned char* getRegisterPool() override;
  int getRegisterPoolSize() override;
  int getRegisterPoolDepth() override;
  const char** getRegisterSheet() override;
  float getPostAmp() override;
  void notifyInsChange(int ins) override;
  void notifyInsDeletion(void* ins) override;
  void notifyPitchTable(int sample=-1) override;
  unsigned int getMaxFreq(int ch) override;
  void poke(unsigned int addr, unsigned short value) override;
  void poke(std::vector<DivRegWrite>& list) override;
  const void* getSampleMem(int index=0) override;
  size_t getSampleMemCapacity(int index=0) override;
  size_t getSampleMemUsage(int index=0) override;
  bool isSampleLoaded(int index, int sample) override;
  const DivMemoryComposition* getMemCompo(int index) override;
  void renderSamples(int chipID) override;
  int getClockRangeMin() override;
  int getClockRangeMax() override;
  void setFlags(const DivConfig& flags) override;
  int init(DivEngine* parent, int channels, int sugRate, const DivConfig& flags) override;
  void quit() override;
};
#endif
