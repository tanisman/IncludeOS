// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//#define DEBUG
//#define DEBUG2
#include <hw/ioport.hpp>
#include <hw/pit.hpp>
#include <os>
#include <hw/cpu_freq_sampling.hpp>
#include <kernel/irq_manager.hpp>

// Used for cpu frequency sampling
extern double _CPUFreq_;
extern const uint16_t _cpu_sampling_freq_divider_;

namespace hw {

  // Bit 0-3: Mode 0 - "Interrupt on terminal count"
  // Bit 4-5: Both set, access mode "Lobyte / Hibyte"
  const uint8_t PIT_mode_register = 0x43;
  const uint8_t PIT_chan0 = 0x40;

  // PIT state
  PIT::Mode PIT::current_mode_ = NONE;
  PIT::Mode PIT::temp_mode_ = NONE;
  uint16_t PIT::current_freq_divider_ = 0;
  uint16_t PIT::temp_freq_divider_ = 0;

  uint64_t PIT::IRQ_counter_ = 0;

  // Time keeping
  uint64_t PIT::millisec_counter = 0;

  // The default recurring timer condition
  std::function<bool()> PIT::forever = []{ return true; };

  // Timer ID's
  uint32_t PIT::Timer::timers_count_ = 0;

  using namespace std::chrono;



  PIT::Timer::Timer(Type t, timeout_handler handler, std::chrono::milliseconds ms, repeat_condition cond)
    : type_{t}, id_{++timers_count_}, handler_{handler}, interval_{ms}, cond_{cond} {};


  void PIT::disable_regular_interrupts()
  {
    oneshot(1);
  }

  PIT::PIT() {
    debug("<PIT> Instantiating. \n");
    // register irq handler
    auto handler(IRQ_manager::irq_delegate::from<PIT,&PIT::irq_handler>(this));
    bsp_idt.subscribe(0, handler);
  }
  PIT::~PIT() {}

  void PIT::estimateCPUFrequency(){

    debug("<PIT EstimateCPUFreq> Saving state: curr_freq_div %i \n",current_freq_divider_);
    // Save PIT-state
    temp_mode_ = current_mode_;
    temp_freq_divider_ = current_freq_divider_;

    auto prev_irq_handler = bsp_idt.get_handler(32);

    debug("<PIT EstimateCPUFreq> Sampling\n");
    bsp_idt.set_handler(32, cpu_sampling_irq_entry);

    // GO!
    set_mode(RATE_GEN);
    set_freq_divider(_cpu_sampling_freq_divider_);

    // BLOCKING call to external measurment.
    calculate_cpu_frequency();

    debug("<PIT EstimateCPUFreq> Done. Result: %f \n", _CPUFreq_);

    set_mode(temp_mode_);
    set_freq_divider(temp_freq_divider_);

    bsp_idt.set_handler(32, prev_irq_handler);
  }

  MHz PIT::CPUFrequency(){
    if (! _CPUFreq_)
      estimateCPUFrequency();

    return MHz(_CPUFreq_);
  }


  PIT::Timer_iterator PIT::start_timer(Timer t, std::chrono::milliseconds in_msecs){
    if (in_msecs < 1ms) panic("Can't wait less than 1 ms. ");

    if (current_mode_ != RATE_GEN)
      set_mode(RATE_GEN);

    if (current_freq_divider_ != millisec_interval)
      set_freq_divider(millisec_interval);
    
    auto cycles_pr_millisec = KHz(CPUFrequency());
    //debug("<PIT start_timer> CPU KHz: %f Cycles to wait: %f \n",cycles_pr_millisec.count(), cycles_pr_millisec.count() * in_msecs);

    auto ticks = in_msecs / KHz(current_frequency()).count();
    //debug("<PIT start_timer> PIT KHz: %f * %i = %f ms. \n",
    //KHz(current_frequency()).count(), (uint32_t)ticks.count(), ((uint32_t)ticks.count() * KHz(current_frequency()).count()));

    t.setStart(OS::cycles_since_boot());
    t.setEnd(t.start() + uint64_t(cycles_pr_millisec.count() * in_msecs.count()));

    auto key = millisec_counter + ticks.count();

    // We could emplace, but the timer exists allready, and might be a reused one
    auto it = timers_.insert(std::make_pair(key, t));

    debug("<PIT start_timer> Key: %i id: %i, t.cond()(): %s There are %i timers. \n",
          (uint32_t)key, t.id(), t.cond()() ? "true" : "false", timers_.size());

    return it;
  }

  PIT::Timer_iterator PIT::onRepeatedTimeout(std::chrono::milliseconds ms, timeout_handler handler, repeat_condition cond){
    debug("<PIT repeated> setting a %i ms. repeating timer \n", (uint32_t)ms.count());

    Timer t(Timer::REPEAT_WHILE, handler, ms, cond);
    return start_timer(t, ms);
  };


  PIT::Timer_iterator PIT::onTimeout(std::chrono::milliseconds msec, timeout_handler handler){
    Timer t(Timer::ONE_SHOT, handler, msec);

    debug("<PIT timeout> setting a %i ms. one-shot timer. Id: %i \n",
          (uint32_t)msec.count(), t.id());

    return start_timer(t, msec);

  };

  void PIT::stop_timer(PIT::Timer_iterator it) {
    timers_.erase(it);
  }


  uint8_t PIT::read_back(uint8_t){
    const uint8_t READ_BACK_CMD = 0xc2;

    hw::outb(PIT_mode_register, READ_BACK_CMD );

    auto res = hw::inb(PIT_chan0);

    debug("STATUS: %#x \n", res);

    return res;

  }

  void PIT::irq_handler(){
    // All IRQ-handlers has to send EOI
    IRQ_manager::eoi(0);

    IRQ_counter_ ++;

    if (current_freq_divider_ == millisec_interval)
      millisec_counter++;

#ifdef DEBUG
    if (millisec_counter % 100 == 0)
      OS::rsprint(".");
#endif

    std::vector<Timer_iterator> garbage {};
    std::vector<Timer_iterator> restart {};

    // Iterate over expired timers (we break on the first non-expired)
    for (auto it = timers_.begin(); it != timers_.end(); it++) {

      // Map-keys are sorted. If this timer isn't expired, neither are the rest
      if (it->first > millisec_counter)
        break;

      debug2 ("\n**** Timer type %i, id: %i expired. Running handler **** \n",
              it->second.type(), it->second.id());

      // Execute the handler
      it->second.handler()();

      // Re-queue repeating timers
      if (it->second.type() == Timer::REPEAT) {
        debug2 ("<Timer IRQ> REPEAT: Requeuing the timer \n");
        restart.push_back(it);

      }else if (it->second.type() == Timer::REPEAT_WHILE and it->second.cond()()) {
        debug2 ("<Timer IRQ> REPEAT_WHILE: Requeuing the timer COND \n");
        restart.push_back(it);
      }

      debug2 ("Timer done. Erasing.  \n");

      // Queue this timer for deletion (Escape iterator death by not deleting it now)
      garbage.push_back(it);

    }

    if (not garbage.empty()) {
      debug2 ("Timers left: %i \n", timers_.size());

#ifdef DEBUG2
      for (auto t : timers_)
        debug2("Key: %i , id: %i, Type: %i \n", (uint32_t)t.first, t.second.id(), t.second.type());
#endif

      debug2("\n---------------------------\n\n");
    }

    for (auto t : restart) {
      start_timer(t->second, t->second.interval());
    }

    for (auto t : garbage) {
      debug2("ERASE: Key: %i , id: %i, Type: %i \n", (uint32_t)t->first, t->second.id(), t->second.type());
      timers_.erase(t);
    }

    if (timers_.empty())
      oneshot(1);

  }


  void PIT::init(){
    debug("<PIT> Initializing @ frequency: %16.16f MHz. Assigning myself to all timer interrupts.\n ", frequency());
    PIT::disable_regular_interrupts();
    // must be done to program IOAPIC to redirect to BSP LAPIC
    bsp_idt.enable_irq(0);
  }

  void PIT::set_mode(Mode mode){
    // Channel is the last two bits in the PIT mode register
    // ...we always use channel 0
    auto channel = 0x00;
    uint8_t config = mode | LO_HI | channel;
    debug("<PIT::set_mode> Setting mode %#x, config: %#x \n", mode, config);

    hw::outb(PIT_mode_register, config);
    current_mode_ = mode;

  }

  void PIT::set_freq_divider(uint16_t freq_divider){
    union {
      uint16_t whole;
      uint8_t part[2];
    } data{freq_divider};

    // Send frequency hi/lo to PIT
    hw::outb(PIT_chan0, data.part[0]);
    hw::outb(PIT_chan0, data.part[1]);

    current_freq_divider_ = freq_divider;
  }

  void PIT::oneshot(uint16_t t){

    // Enable 1-shot mode
    set_mode(ONE_SHOT);

    // Set a frequency for shot
    set_freq_divider(t);
  }

} //< namespace hw
