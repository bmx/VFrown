#include "spu.h"

static SPU_t this;

const float rateCutoff = 44100.0f;

// static const char* registerNames0[] = {
//   "wave address",          // 0x0
//   "audio mode",            // 0x1
//   "loop address",          // 0x2
//   "pan volume",            // 0x3
//   "envelope 0",            // 0x4
//   "envelope data",         // 0x5
//   "envelope 1",            // 0x6
//   "envelope high address", // 0x7
//   "envelope address",      // 0x8
//   "prev wave data",        // 0x9
//   "envelope loop ctrl",    // 0xA
//   "wave data",             // 0xB
//   "adpcm select",          // 0xC
//   "unknown 13",            // 0xD
//   "unknown 14",            // 0xE
//   "unknown 15",            // 0xF
// };
//
// static const char* registerNames2[] = {
//   "phase high",             // 0x0
//   "phase accumulator high", // 0x1
//   "target phase high",      // 0x2
//   "ramp down clock",        // 0x3
//   "phase",                  // 0x4
//   "phase accumulator",      // 0x5
//   "target phase",           // 0x6
//   "phase ctrl",             // 0x7
// };
//
// static const char* registerNames4[] = {
//   "enable",                // 0x00
//   "volume",                // 0x01
//   "FIQ enable",            // 0x02
//   "FIQ stat",              // 0x03
//   "beat base count",       // 0x04
//   "beat count",            // 0x05
//   "envelope clock 0",      // 0x06
//   "envelope clock 0 high", // 0x07
//   "envelope clock 1",      // 0x08
//   "envelope clock 1 high", // 0x09
//   "envelope ramp down",    // 0x0a
//   "stop",                  // 0x0b
//   "zero cross",            // 0x0c
//   "ctrl",                  // 0x0d
//   "compression ctrl",      // 0x0e
//   "stat",                  // 0x0f
//   "L wave in",             // 0x10
//   "R wave in",             // 0x11
//   "L wave out",            // 0x12
//   "R wave out",            // 0x13
//   "channel repeat",        // 0x14
//   "channel env mode",      // 0x15
//   "channel tone release",  // 0x16
//   "channel env irq",       // 0x17
//   "channel pitch bend",    // 0x18
//   "channel soft phase",    // 0x19
//   "attack release",        // 0x1a
//   "eq cutoff 10",          // 0x1b
//   "eq cutoff 32",          // 0x1c
//   "eq gain 10",            // 0x1d
//   "eq gain 32",            // 0x1e
//   "unknown 0x1f"           // 0x1f
// };

static const int16_t adpcmStep[] = {
  16,   17,   19,   21,   23,   25,   28,   31,  34,   37,   41,   45,
  50,   55,   60,   66,   73,   80,   88,   97,  107,  118,  130,  143,
  157,  173,  190,  209,  230,  253,  279,  307, 337,  371,  408,  449,
  494,  544,  598,  658,  724,  796,  876,  963, 1060, 1166, 1282, 1411, 1552
};

static const int8_t adpcmStepShift[] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static const uint32_t rampdownFrameCounts[] = {
  52, 208, 832, 3328, 13312, 53248, 106496, 106496
};

static const int16_t envelopeFrameCounts[] = {
	4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 8192, 8192, 8192, 8192
};

bool SPU_Init() {
  memset(&this, 0, sizeof(SPU_t));

  Backend_InitAudioDevice();

  this.sampleTimer = SPU_SAMPLE_TIMER;

  for (int32_t i = 0; i < 16; i++) {
    this.channels[i].timer = Timer_Init(0, (TimerFunc_t)SPU_TriggerChannelIRQ, i);
  }

  this.beatTimer = Timer_Init(SYSCLOCK / (281250.0f/4.0f), (TimerFunc_t)SPU_TriggerBeatIRQ, 0);
  Timer_Reset(this.beatTimer);

  return true;
}


void SPU_Cleanup() {
  for (int32_t i = 0; i < 16; i++) {
    Timer_Cleanup(this.channels[i].timer);
  }

  Timer_Cleanup(this.beatTimer);
}


void SPU_Reset() {
  // for (int32_t i = 0; i < 16; i++) {
  //
  // }

  Timer_Reset(this.beatTimer);
}


void SPU_Tick(int32_t cycles) {
  Timer_Tick(this.beatTimer, cycles);

  if (this.sampleTimer > 0) {
    this.sampleTimer -= cycles;
    this.accumulatedSamples += cycles;
    return;
  }
  this.sampleTimer += SPU_SAMPLE_TIMER;

  while (this.accumulatedSamples > 0) {
    for (int32_t i = 0; i < 16; i++) {
      Timer_Tick(this.channels[i].timer, SPU_SAMPLE_TIMER);
    }
    this.accumulatedSamples -= SPU_SAMPLE_TIMER;
  }

  if (this.channelIrq) {
    CPU_TriggerFIQ(FIQSRC_SPU);
    this.channelIrq = false;
  }

  int32_t leftSample = 0;
  int32_t rightSample = 0;

  for (int32_t i = 0; i < 16; i++) {
    if (this.chanEnable & (1 << i)) { // Channel is enabled
      int32_t left, right;
      SPU_TickChannel(i, &left, &right);

      leftSample += left;
      rightSample += right;

    } else {
      SDLBackend_PushOscilloscopeSample(i, 0);
    }

  }

  switch((this.volumeSelect >> 6) & 3) { // Volume select
  case 0:
    leftSample >>= 10;
    rightSample >>= 10;
    break;

  case 1: case 2: case 3:
    leftSample >>= 8;
    leftSample >>= 8;
    break;
  }

  Backend_PushAudioSample(leftSample, rightSample);

}


uint16_t SPU_TickSample(uint8_t ch) {
  Channel_t* channel = &this.channels[ch];

  channel->prevWaveData = channel->waveData;

  channel->accum += channel->rate;
  int32_t sampleTicks = 0;
  while (channel->accum >= rateCutoff) {
    channel->accum -= rateCutoff;
    sampleTicks++;
  }

  if (sampleTicks < 1)
    return channel->waveData;

  uint32_t waveAddr = channel->waveAddr | (channel->mode.waveHi << 16);

  switch (channel->mode.pcmMode) {
  case 0: // 8-bit PCM mode
    for (int32_t i = 0; i < sampleTicks; i++) {
      channel->waveData = (uint8_t)(Bus_Load(waveAddr) >> channel->pcmShift) & 0xff;
      channel->waveData <<= 8;
      channel->pcmShift += 8;
      if (channel->pcmShift > 8) {
        channel->pcmShift = 0;
        waveAddr++;
      }

      uint8_t nextSample = (Bus_Load(waveAddr) >> channel->pcmShift) & 0xff;
      if(nextSample == 0xff) {
        if (channel->mode.playMode == 1) { // One shot mode
          SPU_StopChannel(ch);
        } else {
          waveAddr = channel->loopAddr | (channel->mode.loopHi << 16);
          channel->pcmShift = 0;
        }
      }
    }
    break;

  case 1: // 16-bit PCM mode
    for (int32_t i = 0; i < sampleTicks; i++) {
      channel->waveData = Bus_Load(waveAddr);
      waveAddr++;

      if(Bus_Load(waveAddr) == 0xffff) {
        if (channel->mode.playMode == 1) { // One shot mode
          SPU_StopChannel(ch);
        } else {
          waveAddr = channel->loopAddr | (channel->mode.loopHi << 16);
        }
        // SPU_StopChannel(ch); // HACK: prevents stuck intro audio in Alphabet Park Adventure
      }
    }
    break;

  case 2: // ADPCM mode
    for (int32_t i = 0; i < sampleTicks; i++) {
      uint16_t nybble = (Bus_Load(waveAddr) >> channel->pcmShift) & 0xf;
      channel->waveData = SPU_GetADPCMSample(ch, nybble);
      channel->waveData = (channel->waveData << 2) ^ 0x8000;
      channel->pcmShift += 4;
      if (channel->pcmShift >= 16) {
        channel->pcmShift = 0;
        waveAddr++;
      }

      if(Bus_Load(waveAddr) == 0xffff) {
        if (channel->mode.playMode == 1) { // One shot mode
          SPU_StopChannel(ch);
        } else {
          waveAddr = channel->loopAddr | (channel->mode.loopHi << 16);
          channel->pcmShift = 0;
          channel->adpcmLastSample = 0;
          channel->adpcmStepIndex = 0;
        }
      }
    }
    break;

  case 3: // ADPCM36 mode
    for (int32_t i = 0; i < sampleTicks; i++) {
      if (channel->adpcm36Remaining == 0) {
        channel->adpcm36Header.raw = Bus_Load(waveAddr++);
        channel->adpcm36Remaining = 8;
      }

      uint16_t nybble = (Bus_Load(waveAddr) >> channel->pcmShift) & 0xf;
      channel->waveData = SPU_GetADPCM36Sample(ch, nybble);
      channel->pcmShift += 4;
      if (channel->pcmShift >= 16) {
        channel->pcmShift = 0;
        waveAddr++;
        channel->adpcm36Remaining--;
      }

      if(Bus_Load(waveAddr) == 0xffff) {
        if (channel->mode.playMode == 1) { // One shot mode
          SPU_StopChannel(ch);
        } else {
          waveAddr = channel->loopAddr | (channel->mode.loopHi << 16);
          channel->pcmShift = 0;
          channel->adpcmLastSample = 0;
          channel->adpcmStepIndex = 0;
        }
      }
    }
    break;
  }

  channel->waveAddr = waveAddr & 0xffff;
  channel->mode.waveHi = (waveAddr >> 16) & 0x3f;

  return channel->waveData;
}


void SPU_TickChannel(uint8_t ch, int32_t* left, int32_t* right) {
  Channel_t* channel = &this.channels[ch];

  SPU_TickSample(ch);

  int32_t sample = (int16_t)(channel->waveData ^ 0x8000);
  if (!(this.ctrl & 0x0200)) { // Audio CTRL
    int32_t prevSample = (int16_t)(channel->waveData ^ 0x8000);
    int32_t lerp = (int32_t)((channel->rate / 44100.0f) * 256.0f);
    prevSample = (prevSample * (0x100 - lerp)) >> 8;
    sample = (sample * lerp) >> 8;
    sample += prevSample;
  }

  sample = (sample * (int32_t)channel->envData.envelopeData) >> 7;

  SDLBackend_PushOscilloscopeSample(ch, sample);

  int32_t pan = channel->panVol.pan;
  int32_t vol = channel->panVol.vol;

  int32_t panLeft, panRight;
  if (pan < 0x40) {
    panLeft  = 127 * vol;
    panRight = pan * 2 * vol;
  } else {
    panLeft  = (127 - pan) * 2 * vol;
    panRight =  127 * vol;
  }

  (*left)  = (sample * panLeft);
  (*right) = (sample * panRight);

  if (this.envRampDown & (1 << ch)) { // Ramp Down
    if (channel->rampDownFrame > 0)
      channel->rampDownFrame--;

    if (channel->rampDownFrame == 0) {
      uint8_t prevEnvData = channel->envData.envelopeData;
      uint8_t currEnvData = prevEnvData - channel->envLoopCtrl.rampDownOffset;
      if (currEnvData > prevEnvData)
        currEnvData = 0;

      if (currEnvData > 0) {
        channel->envData.envelopeData = currEnvData;
      } else {
        this.chanEnable  &= ~(1 << ch);
        this.chanStat    &= ~(1 << ch);
        this.chanStop    |=  (1 << ch);
        this.envRampDown &= ~(1 << ch);
        this.toneRelease &= ~(1 << ch);
      }

      channel->rampDownFrame = rampdownFrameCounts[channel->rampDownClock];
    }
  }
  else if (!(this.envMode & (1 << ch))) { // Envelope mode
    if (channel->envelopeFrame > 0)
      channel->envelopeFrame--;

    if (channel->envelopeFrame == 0) {
      SPU_TickEnvelope(ch);

      channel->envelopeFrame = envelopeFrameCounts[SPU_GetEnvelopeClock(ch)];
    }
  }
}


int16_t SPU_GetADPCMSample(uint8_t ch, uint8_t nybble) {
  Channel_t* channel = &this.channels[ch];

  int16_t step = adpcmStep[channel->adpcmStepIndex];
  int16_t e = (step >> 3);
  if (nybble & 1) e += (step >> 2);
  if (nybble & 2) e += (step >> 1);
  if (nybble & 4) e += step;
  int16_t offset = (nybble & 8) ? -e : e;
  int16_t sample = channel->adpcmLastSample + offset;

  if (sample >  2047) sample =  2047;
  if (sample < -2048) sample = -2048;

  channel->adpcmLastSample = sample;
  channel->adpcmStepIndex += adpcmStepShift[nybble];
  if (channel->adpcmStepIndex <  0) channel->adpcmStepIndex =  0;
  if (channel->adpcmStepIndex > 48) channel->adpcmStepIndex = 48;

  return sample;
}


int16_t SPU_GetADPCM36Sample(uint8_t ch, uint8_t nybble) {
  Channel_t* channel = &this.channels[ch];

  int32_t f0 = channel->adpcm36Header.filter;
  if (f0 & 0x20) f0 |= ~0x3f;
	int32_t f1 = 1;
  int16_t sample = nybble << 12;
	sample = (sample >> channel->adpcm36Header.shift) + (((channel->adpcm36Prev[0] * f0) + (channel->adpcm36Prev[1] * f1) + 32) >> 12);
	channel->adpcm36Prev[1] = channel->adpcm36Prev[0];
	channel->adpcm36Prev[0] = sample;

  return sample ^ 0x8000;
}


void SPU_TickEnvelope(uint8_t ch) {
  Channel_t* channel = &this.channels[ch];

  uint8_t prevEnvData = channel->envData.envelopeData;

  if (channel->envData.envelopeCount > 0)
    channel->envData.envelopeCount--;

  if (channel->envData.envelopeCount == 0) {
    if (channel->envData.envelopeData != channel->env0.target) {
      if (channel->env0.increment & 0x80) { // Decrement envelope data
        channel->envData.envelopeData -= (channel->env0.increment & 0x7f);

        // Clamp to 0 or target value
        if (channel->envData.envelopeData > prevEnvData)
          channel->envData.envelopeData = 0;
        else if (channel->envData.envelopeData < channel->env0.target)
          channel->envData.envelopeData = channel->env0.target;

        // Stop Channel
        if (channel->envData.envelopeData == 0) {
          SPU_StopChannel(ch);
          return;
        }
      } else { // Increment envelope data
        channel->envData.envelopeData += (channel->env0.increment & 0x7f);

        // Clamp to target value
        if (channel->envData.envelopeData >= channel->env0.target)
          channel->envData.envelopeData = channel->env0.target;
      }
    }

    if (channel->envData.envelopeData == channel->env0.target) {
      uint32_t envAddr = channel->envAddr | (channel->envAddrHigh.envAddrHi << 16);

      if (channel->env1.repeatEnable) {
        channel->env1.repeatCount--;
        if (channel->env1.repeatCount == 0) {
          channel->env0.raw = Bus_Load(envAddr);
          channel->env1.raw = Bus_Load(envAddr+1);
          channel->envLoopCtrl.raw = Bus_Load(envAddr+2);
          envAddr += channel->envLoopCtrl.envAddrOffset;
          // int16_t offset = channel->envLoopCtrl.envAddrOffset;
          // if (offset & 0x100) offset |= 0xfe00;
          // envAddr += offset;
        }
      } else {
        channel->env0.raw = Bus_Load(envAddr);
        channel->env1.raw = Bus_Load(envAddr+1);
        envAddr += 2;
      }

      channel->envAddr = envAddr & 0xffff;
      channel->envAddrHigh.envAddrHi = (envAddr >> 16) & 0x3f;
      channel->envData.envelopeCount = channel->env1.loadVal;
    }
  }
}


uint16_t SPU_GetEnvelopeClock(uint8_t ch) {
  switch (ch) {
  case 0x0 ... 0x3: return (this.envClock0   >> ((ch)    << 2)) & 0xf;
  case 0x4 ... 0x7: return (this.envClock0Hi >> ((ch-4)  << 2)) & 0xf;
  case 0x8 ... 0xb: return (this.envClock1   >> ((ch-8)  << 2)) & 0xf;
  case 0xc ... 0xf: return (this.envClock1Hi >> ((ch-12) << 2)) & 0xf;
  }

  return 0x0000;
}


void SPU_TriggerChannelIRQ(uint8_t ch) {
  // printf("channel IRQ triggered for channel %d\n", ch);

  if (!(this.fiqEnable & (1 << ch))) {
    this.fiqStat |= (1 << ch);
    this.channelIrq = true;
    CPU_ActivatePendingIRQs();
  }

  Timer_Adjust(this.channels[ch].timer, SYSCLOCK / this.channels[ch].rate);
}


uint16_t SPU_Read(uint16_t addr) {
  // switch (addr & 0x0f00) {
  // case 0x0000: {
  //   uint8_t channel = (addr >> 0x4) & 0xf;
  //   uint8_t reg     = (addr &  0xf);
  //   printf("read from channel %d %s (%04x) at %06x\n", channel, registerNames0[reg], addr, CPU_GetCSPC());
  // } break;
  //
  // case 0x0200: {
  //   uint8_t channel = (addr >> 0x4) & 0xf;
  //   uint8_t reg     = (addr &  0x7);
  //   printf("read from channel %d %s (%04x) at %06x\n", channel, registerNames2[reg], addr, CPU_GetCSPC());
  // } break;
  //
  // case 0x0400: {
  //   uint8_t reg = (addr & 0x1f);
  //   printf("read from channel %s (%04x) at %06x\n", registerNames4[reg], addr, CPU_GetCSPC());
  // } break;
  //
  // default:
  //   printf("read from unknown register %04x at %06x\n", addr, CPU_GetCSPC());
  // }

  if ((addr & 0x0f00) == 0x0000) {
    uint8_t channel = (addr >> 0x4) & 0xf;
    uint8_t reg     = (addr &  0xf);
    return this.channels[channel].regs0[reg];
  }

  if ((addr & 0x0f00) == 0x0200) {
    uint8_t channel = (addr >> 0x4) & 0xf;
    uint8_t reg     = (addr &  0x7);
    return this.channels[channel].regs2[reg];
  }

  if ((addr & 0x0f00) == 0x0400) {
    uint8_t reg = (addr & 0x1f);
    return this.regs4[reg];
  }

  return 0x0000;
}


void SPU_Write(uint16_t addr, uint16_t data) {
  // switch (addr & 0x0f00) {
  // case 0x0000: {
  //   uint8_t channel = (addr >> 0x4) & 0xf;
  //   uint8_t reg     = (addr &  0xf);
  //   printf("write to channel %d %s (%04x) with %04x at %06x\n", channel, registerNames0[reg], addr, data, CPU_GetCSPC());
  // } break;
  //
  // case 0x0200: {
  //   uint8_t channel = (addr >> 0x4) & 0xf;
  //   uint8_t reg     = (addr &  0x7);
  //   printf("write to channel %d %s (%04x) with %04x at %06x\n", channel, registerNames2[reg], addr, data, CPU_GetCSPC());
  // } break;
  //
  // case 0x0400: {
  //   uint8_t reg = (addr & 0x1f);
  //   printf("write to %s (%04x) with %04x at %06x\n", registerNames4[reg], addr, data, CPU_GetCSPC());
  // } break;
  //
  // default:
  //   printf("write to unknown register %04x with %04x at %06x\n", addr, data, CPU_GetCSPC());
  // }


  if ((addr & 0x0f00) == 0x0000) {
    Channel_t* channel = &this.channels[(addr >> 0x4) & 0xf];

    uint8_t reg = (addr &  0xf);

    switch (reg) {
    case 0x00:
    case 0x01:
      channel->accum = 0.0f;
      channel->regs0[reg] = data;
      return;

    default: channel->regs0[reg] = data; return;
    }
  }

  if ((addr & 0x0f00) == 0x0200) {
    Channel_t* channel = &this.channels[(addr >> 0x4) & 0xf];

    uint8_t reg = (addr &  0x7);

    switch (reg) {
      case 0x00:   // Phase high
      case 0x04: { // Phase
        channel->regs2[reg] = data;
        float phase = (float)(channel->phase | ((channel->phaseHigh & 3) << 16));
        channel->rate = (phase * 281250.0f) / 524288.0f;
        channel->accum = 0.0f;
        if (channel->isPlaying)
          Timer_Adjust(channel->timer, channel->rate);
        // printf("channel %d rate set to %f\n", (addr >> 0x4) & 0xf, channel->rate);
      } return;

      default: channel->regs2[reg] = data; return;
    }

    return;
  }

  if ((addr & 0x0f00) == 0x0400) {
    uint8_t reg = (addr & 0x1f);

    switch (reg) {
    case 0x00: SPU_EnableChannels(data); return;
    case 0x03:
      this.regs4[reg] &= ~data;
      if (!this.regs4[reg])
        this.channelIrq = false;
      return;
    case 0x05: SPU_WriteBeatCount(data); return;
    case 0x0b: this.regs4[reg] &= ~data; return;
    default:   this.regs4[reg] = data;   return;
    }

    return;
  }

}


void SPU_EnableChannels(uint16_t data) {
  const uint16_t changed = (this.chanEnable ^ data);

  if (changed) {
    for (int32_t i = 0; i < 16; i++) {
      uint16_t mask = (1 << i);

      if (!(changed & mask))
        continue;

      if (data & mask) {
        SPU_StartChannel(i);
      } else {
        SPU_StopChannel(i);
      }

    }

    this.chanEnable = data;
  }
}


void SPU_StartChannel(uint8_t ch) {
  Channel_t* channel = &this.channels[ch];

  this.chanEnable |= (1 << ch); // Channel Enable

  if (this.fiqEnable & (1 << ch)) { // Channel FIQ Status
    Timer_Adjust(channel->timer, SYSCLOCK / channel->rate);
  } else {
    Timer_Stop(channel->timer);
  }

  if (!(this.chanStop & (1 << ch))) { // Channel Stop
    this.chanStat |= (1 << ch); // Channel Status

    channel->adpcm36Remaining = 0;
    channel->adpcm36Header.raw = 0;
    channel->adpcm36Prev[0] = 0;
    channel->adpcm36Prev[1] = 0;
  }

  channel->isPlaying = true;

}


void SPU_StopChannel(uint8_t ch) {
  Channel_t* channel = &this.channels[ch];

  this.chanEnable  &= ~(1 << ch);
  this.envRampDown &= ~(1 << ch);
  this.chanStat    &= ~(1 << ch);
  this.toneRelease &= ~(1 << ch);

  channel->mode.pcmMode = 0;
  channel->pcmShift = 0;
  channel->adpcmLastSample = 0;
  channel->adpcmStepIndex = 0;

  Timer_Stop(channel->timer);

  channel->isPlaying = false;
}


void SPU_WriteBeatCount(uint16_t data) {
  // printf("write to beat count with %04x\n", data);

  this.regs4[0x5] &= ~(data & 0x4000);
  this.regs4[0x5] &= 0x4000;
  this.regs4[0x5] |= (data & ~0x4000);

  if ((this.regs4[0x5] & 0xc000) == 0xc000) {
    this.irq = true;
    CPU_ActivatePendingIRQs();
  } else {
    this.irq = false;
  }

}


void SPU_TriggerBeatIRQ(uint8_t index) {
  if (this.currBeatBase > 0)
    this.currBeatBase--;

  if (this.currBeatBase == 0) {
    this.currBeatBase = this.beatBaseCount;

    if (this.beatCount.count > 0)
      this.beatCount.count--;

    if (this.beatCount.count == 0) {
      if (this.beatCount.irqEn)
        this.beatCount.irq = true;

      if (this.beatCount.irqEn && this.beatCount.irq) {
        this.irq = true;
        CPU_ActivatePendingIRQs();
      } else {
        this.irq = false;
      }
    }
  }

  Timer_Reset(this.beatTimer);
}


uint16_t SPU_GetIRQ() {
  return this.irq;
}


uint16_t SPU_GetChannelIRQ() {
  return this.channelIrq;
}
