/**
  * Copyright (C) 2019 fleroviux (Frederic Meyer)
  *
  * This file is part of NanoboyAdvance.
  *
  * NanoboyAdvance is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * (at your option) any later version.
  *
  * NanoboyAdvance is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with NanoboyAdvance. If not, see <http://www.gnu.org/licenses/>.
  */

#include "cpu.hpp"
#include "dma.hpp"
#include "memory/mmio.hpp"

using namespace GameBoyAdvance;

/* TODO: what happens if src_cntl equals DMA::RELOAD? */
static constexpr int g_dma_modify[2][4] = {
  { 2, -2, 0, 2 },
  { 4, -4, 0, 4 }
};

static constexpr int g_dma_none_id = -1;

/* Retrieves DMA with highest priority from a DMA bitset. */
static constexpr int g_dma_from_bitset[] = {
  /* 0b0000 */ g_dma_none_id,
  /* 0b0001 */ 0,
  /* 0b0010 */ 1,
  /* 0b0011 */ 0,
  /* 0b0100 */ 2,
  /* 0b0101 */ 0,
  /* 0b0110 */ 1,
  /* 0b0111 */ 0,
  /* 0b1000 */ 3,
  /* 0b1001 */ 0,
  /* 0b1010 */ 1,
  /* 0b1011 */ 0,
  /* 0b1100 */ 2,
  /* 0b1101 */ 0,
  /* 0b1110 */ 1,
  /* 0b1111 */ 0
};

static constexpr std::uint32_t g_dma_dst_mask[] = { 0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF };
static constexpr std::uint32_t g_dma_src_mask[] = { 0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF };
static constexpr std::uint32_t g_dma_len_mask[] = { 0x3FFF, 0x3FFF, 0x3FFF, 0xFFFF };

void DMAx::Reset() {
  hblank_set.reset();
  vblank_set.reset();
  runnable.reset();
  current = -1;
  interleaved = false;
  
  for (auto& channel : channels) {
    channel.enable = false;
    channel.repeat = false;
    channel.interrupt = false;
    channel.gamepak = false;
    channel.length  = 0;
    channel.dst_addr = 0;
    channel.src_addr = 0;
    channel.latch.length = 0;
    channel.latch.dst_addr = 0;
    channel.latch.src_addr = 0;
    channel.size = Channel::HWORD;
    channel.time = Channel::IMMEDIATE;
    channel.dst_cntl = Channel::INCREMENT;
    channel.src_cntl = Channel::INCREMENT;
  }
}

void DMAx::TryStart(int chan_id) {
  if (runnable.none()) {
    current = chan_id;
  } else if (chan_id < current) {
    current = chan_id;
    interleaved = true;
  }
  
  runnable.set(chan_id, true);
}

void DMAx::Request(Occasion occasion) {
  switch (occasion) {
    case Occasion::HBlank: {
      auto chan_id = g_dma_from_bitset[hblank_set.to_ulong()];
      if (chan_id != g_dma_none_id)
        TryStart(chan_id);
      break;
    }
    case Occasion::VBlank: {
      auto chan_id = g_dma_from_bitset[vblank_set.to_ulong()];
      if (chan_id != g_dma_none_id)
        TryStart(chan_id);
      break;
    }
    case Occasion::FIFO0:
    case Occasion::FIFO1: {
      auto address = (occasion == Occasion::FIFO0) ? FIFO_A : FIFO_B;
      for (int chan_id = 1; chan_id <= 2; chan_id++) {
        auto& channel = channels[chan_id];
        if (channel.enable &&
            channel.time == Channel::SPECIAL &&
            channel.dst_addr == address) {
          channel.fifo_request_count++;
          TryStart(chan_id);
        }
      }
      break;
    }
  }
}

void DMAx::Run(int const& ticks_left) {
  auto& channel = channels[current];
  
  /* TODO: make clear that this checks for audio DMA. */
  if (channel.time == Channel::SPECIAL && (current == 1 || current == 2)) {
    TransferFIFO();
  } else {
    bool completed;
    
    if (channel.size == Channel::WORD) {
      completed = TransferLoop32(ticks_left);
    } else {
      completed = TransferLoop16(ticks_left);
    }
    
    if (completed) {
      if (channel.interrupt) {
        cpu->mmio.irq_if |= (CPU::INT_DMA0 << current);
      }
      
      /* TODO: refactor this beauty. */
      if (channel.repeat) {
        /* Reload the internal length counter. */
        channel.latch.length = channel.length & g_dma_len_mask[current];
        if (channel.latch.length == 0) {
          channel.latch.length = g_dma_len_mask[current] + 1;
        }

        /* Reload destination address if specified. */
        if (channel.dst_cntl == Channel::RELOAD) {
          channel.latch.dst_addr = channel.dst_addr & g_dma_dst_mask[current];
        }

        /* If DMA is specified to be non-immediate, wait for it to be retriggered. */
        if (channel.time != Channel::IMMEDIATE) {
          runnable.set(current, false);
        }
      } else {
        channel.enable = false;
        runnable.set(current, false);
      }
      
      current = g_dma_from_bitset[runnable.to_ulong()];
    }
  }
}

bool DMAx::TransferLoop16(int const& ticks_left) {
  auto& channel   = channels[current];
  auto src_modify = g_dma_modify[Channel::HWORD][channel.src_cntl];
  auto dst_modify = g_dma_modify[Channel::HWORD][channel.dst_cntl];
  
  std::uint32_t word;
  
  while (channel.latch.length != 0) {
    if (ticks_left <= 0) return false; 
    
    /* Stop if DMA was interleaved by higher priority DMA. */
    if (interleaved) {
      interleaved = false;
      return false;
    }
    
    word = cpu->ReadHalf(channel.latch.src_addr & ~1, ARM::ACCESS_SEQ);
    cpu->WriteHalf(channel.latch.dst_addr & ~1, word, ARM::ACCESS_SEQ);
    
    channel.latch.src_addr += src_modify;
    channel.latch.dst_addr += dst_modify;
    channel.latch.length--;
  }
  
  return true;
}

bool DMAx::TransferLoop32(int const& ticks_left) {
  auto& channel   = channels[current];
  auto src_modify = g_dma_modify[Channel::WORD][channel.src_cntl];
  auto dst_modify = g_dma_modify[Channel::WORD][channel.dst_cntl];
  
  std::uint32_t word;
  
  while (channel.latch.length != 0) {
    if (ticks_left <= 0) return false; 
    
    /* Stop if DMA was interleaved by higher priority DMA. */
    if (interleaved) {
      interleaved = false;
      return false;
    }
    
    word = cpu->ReadWord(channel.latch.src_addr & ~3, ARM::ACCESS_SEQ);
    cpu->WriteWord(channel.latch.dst_addr & ~3, word, ARM::ACCESS_SEQ);
    
    channel.latch.src_addr += src_modify;
    channel.latch.dst_addr += dst_modify;
    channel.latch.length--;
  }
  
  return true;
}

void DMAx::TransferFIFO() {
  auto& channel = channels[current];
  
  /* TODO(1): Assert FIFO DMA conditions. 
   *          What happens when the DMA is not configured as in the spec.?
   * TODO(2): Ensure that we do not overshoot 'ticks_left'. 
   */
  std::uint32_t word;
  
  for (int i = 0; i < 4; i++) {
    word = cpu->ReadWord(channel.latch.src_addr & ~3, ARM::ACCESS_SEQ);
    cpu->WriteWord(channel.latch.dst_addr & ~3, word, ARM::ACCESS_SEQ);
    
    channel.latch.src_addr += 4;
  }
    
  if (--channel.fifo_request_count == 0) {
    runnable.set(current, false);
  }
    
  if (channel.interrupt) {
    cpu->mmio.irq_if |= (CPU::INT_DMA0 << current);
  }
  
  current = g_dma_from_bitset[runnable.to_ulong()];
}

auto DMAx::Read(int chan_id, int offset) -> std::uint8_t {
  auto const& channel = channels[chan_id];

  switch (offset) {
    case 10: {
      return (channel.dst_cntl << 5) |
             (channel.src_cntl << 7);
    }
    case 11: {
      return (channel.src_cntl  >> 1) |
             (channel.size      << 2) |
             (channel.time      << 4) |
             (channel.repeat    ? 2   : 0) |
             (channel.gamepak   ? 8   : 0) |
             (channel.interrupt ? 64  : 0) |
             (channel.enable    ? 128 : 0);
    }
    default: return 0;
  }
}

void DMAx::Write(int chan_id, int offset, std::uint8_t value) {
  auto& channel = channels[chan_id];

  switch (offset) {
    /* DMAXSAD */
    case 0: channel.src_addr = (channel.src_addr & 0xFFFFFF00) | (value<<0 ); break;
    case 1: channel.src_addr = (channel.src_addr & 0xFFFF00FF) | (value<<8 ); break;
    case 2: channel.src_addr = (channel.src_addr & 0xFF00FFFF) | (value<<16); break;
    case 3: channel.src_addr = (channel.src_addr & 0x00FFFFFF) | (value<<24); break;

    /* DMAXDAD */
    case 4: channel.dst_addr = (channel.dst_addr & 0xFFFFFF00) | (value<<0 ); break;
    case 5: channel.dst_addr = (channel.dst_addr & 0xFFFF00FF) | (value<<8 ); break;
    case 6: channel.dst_addr = (channel.dst_addr & 0xFF00FFFF) | (value<<16); break;
    case 7: channel.dst_addr = (channel.dst_addr & 0x00FFFFFF) | (value<<24); break;

    /* DMAXCNT_L */
    case 8: channel.length = (channel.length & 0xFF00) | (value<<0); break;
    case 9: channel.length = (channel.length & 0x00FF) | (value<<8); break;

    /* DMAXCNT_H */
    case 10: {
      channel.dst_cntl = Channel::Control((value >> 5) & 3);
      channel.src_cntl = Channel::Control((channel.src_cntl & 0b10) | (value>>7));
      break;
    }
    case 11: {
      bool enable_previous = channel.enable;

      channel.src_cntl  = Channel::Control((channel.src_cntl & 0b01) | ((value & 1)<<1));
      channel.size      = Channel::Size((value>>2) & 1);
      channel.time      = Channel::Timing((value>>4) & 3);
      channel.repeat    = value & 2;
      channel.gamepak   = value & 8;
      channel.interrupt = value & 64;
      channel.enable    = value & 128;

      /* TODO: clean this mess up! */
      
      hblank_set.set(chan_id, false);
      vblank_set.set(chan_id, false);

      if (channel.enable) {
        /* Update HBLANK/VBLANK DMA sets. */
        switch (channel.time) {
          case Channel::HBLANK:
            hblank_set.set(chan_id, true);
            break;
          case Channel::VBLANK:
            vblank_set.set(chan_id, true);
            break;
        }

        /* DMA state is latched on "rising" enable bit. */
        if (!enable_previous) {
          channel.fifo_request_count = 0;

          /* Latch sanitized values into internal DMA state. */
          channel.latch.dst_addr = channel.dst_addr & g_dma_dst_mask[chan_id];
          channel.latch.src_addr = channel.src_addr & g_dma_src_mask[chan_id];
          channel.latch.length   = channel.length   & g_dma_len_mask[chan_id];

          if (channel.latch.length == 0) {
            channel.latch.length = g_dma_len_mask[chan_id] + 1;
          }

          /* Schedule DMA if is setup for immediate execution. */
          if (channel.time == Channel::IMMEDIATE) {
            TryStart(chan_id);
          }
        }
      }
      break;
    }
  }
}
