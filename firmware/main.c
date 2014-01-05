/* 
 * Project: Micronucleus -  v1.11
 * 
 * Original author               (c) 2012 Jenna Fox
 *
 * Optimizations v1.10/v1.11     (c) 2013 Tim Bo"scke - cpldcpu@gmail.com
 *                     v1.11     (c) 2013 Shay Green
 *
 * Based on USBaspLoader-tiny85  (c) 2012 Louis Beaudoin
 * Based on USBaspLoader         (c) 2007 by OBJECTIVE DEVELOPMENT Software GmbH
 *
 * License: GNU GPL v2 (see License.txt)
 */
 
#define MICRONUCLEUS_VERSION_MAJOR 1
#define MICRONUCLEUS_VERSION_MINOR 99
// how many milliseconds should host wait till it sends another erase or write?
// needs to be above 4.5 (and a whole integer) as avr freezes for 4.5ms
#define MICRONUCLEUS_WRITE_SLEEP 5
// Use the old delay routines without NOP padding. This saves memory.
#define __DELAY_BACKWARD_COMPATIBLE__     

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/boot.h>
#include <util/delay.h>

#include "bootloaderconfig.h"
#include "usbdrv/usbdrv.c"

// verify the bootloader address aligns with page size
#if BOOTLOADER_ADDRESS % SPM_PAGESIZE != 0
  #error "BOOTLOADER_ADDRESS in makefile must be a multiple of chip's pagesize"
#endif

#if SPM_PAGESIZE>256
  #error "Micronucleus only supports pagesizes up to 256 bytes"
#endif

// command system schedules functions to run in the main loop
register uint8_t        command         asm("r3");  // bind command to r3 
register uint16_union_t currentAddress  asm("r4");  // r4/r5 current progmem address, used for erasing and writing 
register uint16_union_t idlePolls       asm("r6");  // r6/r7 idlecounter

#if OSCCAL_RESTORE
  register uint8_t      osccal_default  asm("r2");
#endif 

static uint16_t vectorTemp; // remember data to create tinyVector table before BOOTLOADER_ADDRESS

enum {
  cmd_local_nop=0, // also: get device info
  cmd_device_info=0,
  cmd_transfer_page=1,
  cmd_erase_application=2,
  cmd_exit=4,
  cmd_write_page=64,  // internal commands start at 64
};

// Definition of sei and cli without memory barrier keyword to prevent reloading of memory variables
#define sei() asm volatile("sei")
#define cli() asm volatile("cli")
#define nop() asm volatile("nop")

/* ------------------------------------------------------------------------ */
static inline void eraseApplication(void);
static void writeFlashPage(void);
static void writeWordToPageBuffer(uint16_t data);
static uint8_t usbFunctionSetup(uint8_t data[8]);
static uint8_t usbFunctionWrite(uint8_t *data, uint8_t length);
static inline void leaveBootloader(void);

// erase any existing application and write in jumps for usb interrupt and reset to bootloader
//  - Because flash can be erased once and programmed several times, we can write the bootloader
//  - vectors in now, and write in the application stuff around them later.
//  - if vectors weren't written back in immediately, usb would fail.
static inline void eraseApplication(void) {
  // erase all pages until bootloader, in reverse order (so our vectors stay in place for as long as possible)
  // while the vectors don't matter for usb comms as interrupts are disabled during erase, it's important
  // to minimise the chance of leaving the device in a state where the bootloader wont run, if there's power failure
  // during upload
  
  uint8_t i;
  uint16_t ptr = BOOTLOADER_ADDRESS;

  while (ptr) {
    ptr -= SPM_PAGESIZE;        
    boot_page_erase(ptr);
  }
    
	currentAddress.w = 0;
  for (i=0; i<8; i++) writeWordToPageBuffer(0xFFFF);  // Write first 8 words to fill in vectors.
  writeFlashPage();  
  }
  

// simply write currently stored page in to already erased flash memory
static inline void writeFlashPage(void) {

  boot_page_write(currentAddress.w - 2);   // will halt CPU, no waiting required

}

// clear memory which stores data to be written by next writeFlashPage call
#define __boot_page_fill_clear()             \
(__extension__({                             \
  __asm__ __volatile__                       \
  (                                          \
    "sts %0, %1\n\t"                         \
    "spm\n\t"                                \
    :                                        \
    : "i" (_SFR_MEM_ADDR(__SPM_REG)),        \
      "r" ((uint8_t)(__BOOT_PAGE_FILL | (1 << CTPB)))     \
  );                                           \
}))

// write a word in to the page buffer, doing interrupt table modifications where they're required
static void writeWordToPageBuffer(uint16_t data) {
  
  // first two interrupt vectors get replaced with a jump to the bootloader's vector table
  // remember vectors or the tinyvector table 
    if (currentAddress.w == RESET_VECTOR_OFFSET * 2) {
      vectorTemp = data;
      data = 0xC000 + (BOOTLOADER_ADDRESS/2) - 1;
    }

    // at end of page just before bootloader, write in tinyVector table
  // see http://embedded-creations.com/projects/attiny85-usb-bootloader-overview/avr-jtag-programmer/
  // for info on how the tiny vector table works
  if (currentAddress.w == BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET) {
      data = vectorTemp + ((FLASHEND + 1) - BOOTLOADER_ADDRESS)/2 + 2 + RESET_VECTOR_OFFSET;

#if (!OSCCAL_RESTORE) && OSCCAL_16_5MHz   
  } else if (currentAddress.w == BOOTLOADER_ADDRESS - TINYVECTOR_OSCCAL_OFFSET) {
      data = OSCCAL;
#endif		
  }

 // previous_sreg=SREG;    
 // cli(); // ensure interrupts are disabled
  
  boot_page_fill(currentAddress.w, data);
  
  // increment progmem address by one word
  currentAddress.w += 2;
 // SREG=previous_sreg;
}

// This function is never called, it is just here to suppress a compiler warning.
USB_PUBLIC usbMsgLen_t usbFunctionDescriptor(struct usbRequest *rq) { return 0; }

/* ------------------------------------------------------------------------ */
static uint8_t usbFunctionSetup(uint8_t data[8]) {
  usbRequest_t *rq = (void *)data;

  static uint8_t replyBuffer[4] = {
    (((uint16_t)PROGMEM_SIZE) >> 8) & 0xff,
    ((uint16_t)PROGMEM_SIZE) & 0xff,
    SPM_PAGESIZE,
    MICRONUCLEUS_WRITE_SLEEP
  };

  idlePolls.b[1]=0; // reset idle polls when we get usb traffic

  if (rq->bRequest == cmd_device_info) { // get device info
    usbMsgPtr = replyBuffer;
    return 4;      
  } else if (rq->bRequest == cmd_transfer_page) { // transfer page  
    // clear page buffer as a precaution before filling the buffer in case 
    // a previous write operation failed and there is still something in the buffer.
    __boot_page_fill_clear();
    currentAddress.w = rq->wIndex.word;        
    return USB_NO_MSG; // hands off work to usbFunctionWrite
  } else {
    // Handle cmd_erase_application and cmd_exit
    command=rq->bRequest;
    return 0;
  }
}

// read in a page over usb, and write it in to the flash write buffer
static uint8_t usbFunctionWrite(uint8_t *data, uint8_t length) {
  do {     
    // make sure we don't write over the bootloader!
    if (currentAddress.w >= BOOTLOADER_ADDRESS) break;
    
    writeWordToPageBuffer(*(uint16_t *) data);
    data += 2; // advance data pointer
    length -= 2;
  } while(length);
  
  // if we have now reached another page boundary, we're done
  uint8_t isLast = ((currentAddress.b[0] % SPM_PAGESIZE) == 0);
  if (isLast) command=cmd_write_page; // ask runloop to write our page
  
  return isLast; // let V-USB know we're done with this request
}

static void initHardware (void)
{
  // Disable watchdog and set timeout to maximum in case the WDT is fused on 
  MCUSR=0;    
  WDTCR = 1<<WDCE | 1<<WDE;
  WDTCR = 1<<WDP2 | 1<<WDP1 | 1<<WDP0; 

  /* initialize  */
  #if OSCCAL_RESTORE
    osccal_default = OSCCAL;
  #endif
    
  usbDeviceDisconnect();  /* do this while interrupts are disabled */
  _delay_ms(300);  
  usbDeviceConnect();
  
  // Todo: timeout if no reset is found
  calibrateOscillatorASM();
  usbInit();    // Initialize INT settings after reconnect
  
 // sei();        
}

/* ------------------------------------------------------------------------ */
// reset system to a normal state and launch user program
static void leaveBootloader(void) __attribute__((__noreturn__));
static inline void leaveBootloader(void) {
 
  bootLoaderExit();

  _delay_ms(10); // Bus needs to see a few more SOFs before it can be disconnected
	usbDeviceDisconnect();  /* Disconnect micronucleus */

  USB_INTR_ENABLE = 0;
  USB_INTR_CFG = 0;       /* also reset config bits */

  #if OSCCAL_RESTORE
    OSCCAL=osccal_default;
    nop(); // NOP to avoid CPU hickup during oscillator stabilization
  #elif OSCCAL_16_5MHz   
    // adjust clock to previous calibration value, so user program always starts with same calibration
    // as when it was uploaded originally
    unsigned char stored_osc_calibration = pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_OSCCAL_OFFSET);
    if (stored_osc_calibration != 0xFF && stored_osc_calibration != 0x00) {
      OSCCAL=stored_osc_calibration;
      nop();
    }
  #endif
  
  asm volatile ("rjmp __vectors - 2"); // jump to application reset vector at end of flash
  
  for (;;); // Make sure function does not return to help compiler optimize
}

void USB_INTR_VECTOR(void);

int main(void) {
  uint8_t ackSent=0;  
  bootLoaderInit();
	
  DDRB|=3;
  
  if (bootLoaderStartCondition()||(pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET + 1)==0xff)) {
  
    initHardware();        
    LED_INIT();

    if (AUTO_EXIT_NO_USB_MS>0) {
      idlePolls.b[1]=((AUTO_EXIT_MS-AUTO_EXIT_NO_USB_MS) * 10UL)>>8;
    } else {
      idlePolls.b[1]=0;
    }
    
    do {
     
    USB_INTR_PENDING = 1<<USB_INTR_PENDING_BIT; 

  while ( !(USB_INTR_PENDING & (1<<USB_INTR_PENDING_BIT)) );
           USB_INTR_VECTOR();
          
          
  command=cmd_local_nop;     
  PORTB|=_BV(PB1);
  USB_INTR_PENDING = 1<<USB_INTR_PENDING_BIT; 
  usbPoll();
  
   // Test whether another interrupt occured during the processing of USBpoll.
   // If yes, we missed a data packet on the bus. This is not a big issue, since
   // USB seems to allow timeout of up the two packets. (On my machine an USB
   // error is triggered after the third missed packet.) 
   // The most critical situation occurs when a PID IN packet is missed due to
   // it's short length. Each packet + timeout takes around 45µs, meaning that
   // usbpoll must take less than 90µs or resyncing is not possible.
   // To avoid synchronizing of the interrupt routine, we must not call it while
   // a packet is transmitted. Therefore we have to wait until the bus is idle again.
   //
   // Just waiting for EOP (SE0) or no activity for 6 bus cycles is not enough,
   // as the host may have been sending a multi-packet transmission (eg. OUT or SETUP)
   // In that case we may resynch within a transmission, causing errors.
   //
   // A safer way is to wait until the bus was idle for the time it takes to send
   // an ACK packet by the client (10.5µs on D+) but not as long as bus
   // time out (12µs)
   //
   // TODO: Fix usb receiver to discard DATA1/0 packets without preceding OUT or SETUP
        
   if (USB_INTR_PENDING & (1<<USB_INTR_PENDING_BIT))  // Usbpoll intersected with data packe
   {        
     PORTB|=_BV(PB0);
      uint8_t ctr;
      uint8_t timeout=(uint8_t)(10.0f*(F_CPU/1.0e6f)/5.0f+0.5);
     
      // loop takes 5 cycles
      asm volatile(      
      "         ldi  %0,%1 \n\t"        
      "loop%=:  sbic %2,%3  \n\t"        
      "         ldi  %0,%1  \n\t"
      "         subi %0,1   \n\t"        
      "         brne loop%= \n\t"   
      : "=&d" (ctr)
      :  "M" ((uint8_t)(10.0f*(F_CPU/1.0e6f)/5.0f+0.5)), "I" (_SFR_IO_ADDR(USBIN)), "M" (USB_CFG_DPLUS_BIT)
      );       
            
     
     // loop takes 9 cycles
     /*
     while (--tx) {
        uint8_t usbin=USBIN;
        
        if (usbin&(1<<USB_CFG_DPLUS_BIT)) {tx=timeout;}
        
      }
      */
     PORTB&=~_BV(PB0);
   }     
  PORTB&=~_BV(PB1);

  if (command == cmd_local_nop) continue;
/*  if (!ackSent) {ackSent=1;continue;}
  ackSent=0;*/
  
  USB_INTR_PENDING = 1<<USB_INTR_PENDING_BIT;
  while ( !(USB_INTR_PENDING & (1<<USB_INTR_PENDING_BIT)) );
           USB_INTR_VECTOR();  

      idlePolls.w++;
      
      // Try to execute program if bootloader exit condition is met
  //    if (AUTO_EXIT_MS&&(idlePolls.w==AUTO_EXIT_MS*10L)) command=cmd_exit;
 
      LED_MACRO( idlePolls.b[1] );

      if (command==cmd_erase_application) 
        eraseApplication();
      else if (command==cmd_write_page) 
        writeFlashPage();
       
      /* main event loop runs as long as no problem is uploaded or existing program is not executed */                           
    } while((command!=cmd_exit)||(pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET + 1)==0xff));  

    LED_EXIT();
  }
   
  leaveBootloader();
}
/* ------------------------------------------------------------------------ */
