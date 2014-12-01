//#define DEBUG
#include <os>
#include <stdio.h>
#include <assert.h>

#include <class_dev.hpp>
#include <class_service.hpp>

// A private class to handle IRQ
#include "class_irq_handler.hpp"
#include <class_pci_manager.hpp>

bool OS::_power = true;
float OS::_CPU_mhz = 2399.928; //For Trident3, reported by /proc/cpuinfo

// The heap starts @ 1MB
caddr_t OS::_heap_start = (caddr_t)0x100000;//&_end;//

void OS::start()
{
  // Set heap to an appropriate location
  if (&_end > _heap_start)
    _heap_start = &_end;

  rsprint(">>> OS class started\n");
  srand(time(NULL));
  
  // Disable the timer interrupt completely
  disable_PIT();

  timeval t;
  gettimeofday(&t,0);
  printf("<OS> TimeOfDay: %li.%li Uptime: %f \n",t.tv_sec,t.tv_usec,uptime());

  __asm__("cli");  
  IRQ_handler::init();
  Dev::init();  
  
  //Everything is ready
  Service::start();
  
  __asm__("sti");
  halt();
};

void OS::disable_PIT(){
  
#define PIT_one_shot 0x30
#define PIT_mode_chan 0x43
#define PIT_chan0 0x40
  
  // Enable 1-shot mode
  OS::outb(PIT_mode_chan,PIT_one_shot);
  
  // Set a frequency for "first shot"
  OS::outb(PIT_chan0,1);
  OS::outb(PIT_chan0,0);
  debug("<PIT> Switching to 1-shot mode (0x%x) \n",PIT_one_shot);
  
  
};


extern "C" void halt_loop(){
  __asm__ volatile("hlt; jmp halt_loop;");
}

  
union intstr{
  int i;
  char part[4];
};

void OS::halt(){
  //intstr eof{EOF};
  

  OS::rsprint("\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
  OS::rsprint(">>> System idle - waiting for interrupts \n");
  OS::rsprint("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
  //OS::rsprint(eof.part);
  while(_power){        
    
    IRQ_handler::notify(); 
    
    debug("<OS> Woke up @ t = %li \n",uptime());
  }
  
  //Cleanup
  //Service::stop();
}

int OS::rsprint(const char* str){
  char* ptr=(char*)str;
  while(*ptr)
    rswrite(*(ptr++));  
  return ptr-str;
}


/* STEAL: Read byte from I/O address space */
uint8_t OS::inb(int port) {  
  int ret;

  __asm__ volatile ("xorl %eax,%eax");
  __asm__ volatile ("inb %%dx,%%al":"=a" (ret):"d"(port));

  return ret;
}


/*  Write byte to I/O address space */
void OS::outb(int port, uint8_t data) {
  __asm__ volatile ("outb %%al,%%dx"::"a" (data), "d"(port));
}



/* 
 * STEAL: Print to serial port 0x3F8
 */
int OS::rswrite(char c) {
  /* Wait for the previous character to be sent */
  while ((inb(0x3FD) & 0x20) != 0x20);

  /* Send the character */
  outb(0x3F8, c);

  return 1;
}






