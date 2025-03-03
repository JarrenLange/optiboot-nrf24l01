#define LED_START_FLASHES 0
#define RADIO_UART  1
#define SUPPORT_EEPROM 1
#define F_CPU 8000000
#define __AVR_ATmega168__


#define OPTIBOOT_MAJVER 5
#define OPTIBOOT_MINVER 0

#define MAKESTR(a) #a
#define MAKEVER(a, b) MAKESTR(a*256+b)

asm("  .section .version\n"
    "optiboot_version:  .word " MAKEVER(OPTIBOOT_MAJVER, OPTIBOOT_MINVER) "\n"
    "  .section .text\n");

#include <inttypes.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>

// <avr/boot.h> uses sts instructions, but this version uses out instructions
// This saves cycles and program memory.
#include "boot.h"


// We don't use <avr/wdt.h> as those routines have interrupt overhead we don't need.

#include "pin_defs.h"
#include "stk500.h"

#ifndef LED_START_FLASHES
#define LED_START_FLASHES 0
#endif

#ifdef LUDICROUS_SPEED
#define BAUD_RATE 230400L
#endif

/* set the UART baud rate defaults */
#ifndef BAUD_RATE
#if F_CPU >= 8000000L
#define BAUD_RATE   115200L // Highest rate Avrdude win32 will support
#elif F_CPU >= 1000000L
#define BAUD_RATE   9600L   // 19200 also supported, but with significant error
#elif F_CPU >= 128000L
#define BAUD_RATE   4800L   // Good for 128kHz internal RC
#else
#define BAUD_RATE 1200L     // Good even at 32768Hz
#endif
#endif

#ifndef UART
#define UART 0
#endif

#define BAUD_SETTING (( (F_CPU + BAUD_RATE * 4L) / ((BAUD_RATE * 8L))) - 1 )
#define BAUD_ACTUAL (F_CPU/(8 * ((BAUD_SETTING)+1)))
#define BAUD_ERROR (( 100*(BAUD_RATE - BAUD_ACTUAL) ) / BAUD_RATE)

#if BAUD_ERROR >= 5
#error BAUD_RATE error greater than 5%
#elif BAUD_ERROR <= -5
#error BAUD_RATE error greater than -5%
#elif BAUD_ERROR >= 2
#warning BAUD_RATE error greater than 2%
#elif BAUD_ERROR <= -2
#warning BAUD_RATE error greater than -2%
#endif

#if 0
/* Switch in soft UART for hard baud rates */
/*
 * I don't understand what this was supposed to accomplish, where the
 * constant "280" came from, or why automatically (and perhaps unexpectedly)
 * switching to a soft uart is a good thing, so I'm undoing this in favor
 * of a range check using the same calc used to config the BRG...
 */
#if (F_CPU/BAUD_RATE) > 280 // > 57600 for 16MHz
#ifndef SOFT_UART
#define SOFT_UART
#endif
#endif
#else // 0
#if (F_CPU + BAUD_RATE * 4L) / (BAUD_RATE * 8L) - 1 > 250
#error Unachievable baud rate (too slow) BAUD_RATE 
#endif // baud rate slow check
#if (F_CPU + BAUD_RATE * 4L) / (BAUD_RATE * 8L) - 1 < 3
#error Unachievable baud rate (too fast) BAUD_RATE 
#endif // baud rate fastn check
#endif

/* Watchdog settings */
#define WATCHDOG_OFF    (0)
#define WATCHDOG_16MS   (_BV(WDE))
#define WATCHDOG_32MS   (_BV(WDP0) | _BV(WDE))
#define WATCHDOG_64MS   (_BV(WDP1) | _BV(WDE))
#define WATCHDOG_125MS  (_BV(WDP1) | _BV(WDP0) | _BV(WDE))
#define WATCHDOG_250MS  (_BV(WDP2) | _BV(WDE))
#define WATCHDOG_500MS  (_BV(WDP2) | _BV(WDP0) | _BV(WDE))
#define WATCHDOG_1S     (_BV(WDP2) | _BV(WDP1) | _BV(WDE))
#define WATCHDOG_2S     (_BV(WDP2) | _BV(WDP1) | _BV(WDP0) | _BV(WDE))
#ifndef __AVR_ATmega8__
#define WATCHDOG_4S     (_BV(WDP3) | _BV(WDE))
#define WATCHDOG_8S     (_BV(WDP3) | _BV(WDP0) | _BV(WDE))
#endif

/* Function Prototypes */
/* The main function is in init9, which removes the interrupt vector table */
/* we don't need. It is also 'naked', which means the compiler does not    */
/* generate any entry or exit code itself. */
int main(void) __attribute__ ((OS_main)) __attribute__ ((section (".init9"))) __attribute__ ((__noreturn__));
void putch(char);
uint8_t getch(void);
static inline void getNch(uint8_t); /* "static inline" is a compiler hint to reduce code size */
void verifySpace();
static inline void flash_led(uint8_t);
uint8_t getLen();
static inline void watchdogReset();
void watchdogConfig(uint8_t x);
#ifdef SOFT_UART
void uartDelay() __attribute__ ((naked));
#endif
void wait_timeout(void) __attribute__ ((__noreturn__));
void appStart(uint8_t rstFlags) __attribute__ ((naked))  __attribute__ ((__noreturn__));
#ifdef RADIO_UART
static void radio_init(void);
#endif

/*
 * NRWW memory
 * Addresses below NRWW (Non-Read-While-Write) can be programmed while
 * continuing to run code from flash, slightly speeding up programming
 * time.  Beware that Atmel data sheets specify this as a WORD address,
 * while optiboot will be comparing against a 16-bit byte address.  This
 * means that on a part with 128kB of memory, the upper part of the lower
 * 64k will get NRWW processing as well, even though it doesn't need it.
 * That's OK.  In fact, you can disable the overlapping processing for
 * a part entirely by setting NRWWSTART to zero.  This reduces code
 * space a bit, at the expense of being slightly slower, overall.
 *
 * RAMSTART should be self-explanatory.  It's bigger on parts with a
 * lot of peripheral registers.
 */
#if defined(__AVR_ATmega168__)|| defined(__AVR_ATmega168p__)
#define RAMSTART (0x100)
#define NRWWSTART (0x3800)
#elif defined(__AVR_ATmega328P__) || defined(__AVR_ATmega32__)
#define RAMSTART (0x100)
#define NRWWSTART (0x7000)
#elif defined (__AVR_ATmega644P__)
#define RAMSTART (0x100)
#define NRWWSTART (0xE000)
// correct for a bug in avr-libc
#undef SIGNATURE_2
#define SIGNATURE_2 0x0A
#elif defined (__AVR_ATmega1284P__)
#define RAMSTART (0x100)
#define NRWWSTART (0xE000)
#elif defined(__AVR_ATtiny84__)
#define RAMSTART (0x100)
#define NRWWSTART (0x0000)
#elif defined(__AVR_ATmega1280__)
#define RAMSTART (0x200)
#define NRWWSTART (0xE000)
#elif defined(__AVR_ATmega8__) || defined(__AVR_ATmega88__)
#define RAMSTART (0x100)
#define NRWWSTART (0x1800)
#endif

// TODO: get actual .bss+.data size from GCC
#ifdef RADIO_UART
#define BSS_SIZE	0x80
#else
#define BSS_SIZE	0
#endif

/* C zero initialises all global variables. However, that requires */
/* These definitions are NOT zero initialised, but that doesn't matter */
/* This allows us to drop the zero init code, saving us memory */
#define buff    ((uint8_t*)(RAMSTART+BSS_SIZE))
#ifdef VIRTUAL_BOOT_PARTITION
#define rstVect (*(uint16_t*)(RAMSTART+BSS_SIZE+SPM_PAGESIZE*2+4))
#define wdtVect (*(uint16_t*)(RAMSTART+BSS_SIZE+SPM_PAGESIZE*2+6))
#endif

/*
 * Handle devices with up to 4 uarts (eg m1280.)  Rather inelegantly.
 * Note that mega8/m32 still needs special handling, because ubrr is handled
 * differently.
 */
#if UART == 0
# define UART_SRA UCSR0A
# define UART_SRB UCSR0B
# define UART_SRC UCSR0C
# define UART_SRL UBRR0L
# define UART_UDR UDR0
#elif UART == 1
#if !defined(UDR1)
#error UART == 1, but no UART1 on device
#endif
# define UART_SRA UCSR1A
# define UART_SRB UCSR1B
# define UART_SRC UCSR1C
# define UART_SRL UBRR1L
# define UART_UDR UDR1
#elif UART == 2
#if !defined(UDR2)
#error UART == 2, but no UART2 on device
#endif
# define UART_SRA UCSR2A
# define UART_SRB UCSR2B
# define UART_SRC UCSR2C
# define UART_SRL UBRR2L
# define UART_UDR UDR2
#elif UART == 3
#if !defined(UDR1)
#error UART == 3, but no UART3 on device
#endif
# define UART_SRA UCSR3A
# define UART_SRB UCSR3B
# define UART_SRC UCSR3C
# define UART_SRL UBRR3L
# define UART_UDR UDR3
#endif

static void eeprom_write(uint16_t addr, uint8_t val) {
  while (!eeprom_is_ready());

  EEAR = addr;
  EEDR = val;
  EECR |= 1 << EEMPE;	/* Write logical one to EEMPE */
  EECR |= 1 << EEPE;	/* Start eeprom write by setting EEPE */
}

static uint8_t eeprom_read(uint16_t addr) {
  while (!eeprom_is_ready());

  EEAR = addr;
  EECR |= 1 << EERE;	/* Start eeprom read by writing EERE */

  return EEDR;
}

/* main program starts here */
int main(void) {
  uint8_t ch;

  /*
   * Making these local and in registers prevents the need for initializing
   * them, and also saves space because code no longer stores to memory.
   * (initializing address keeps the compiler happy, but isn't really
   *  necessary, and uses 4 bytes of flash.)
   */
  register uint16_t address = 0;
  register uint8_t  length;

  // After the zero init loop, this is the first code to run.
  //
  // This code makes the following assumptions:
  //  No interrupts will execute
  //  SP points to RAMEND
  //  r1 contains zero
  //
  // If not, uncomment the following instructions:
  // cli();
  asm volatile ("cli");
  asm volatile ("clr __zero_reg__");
#if defined(__AVR_ATmega8__) || defined (__AVR_ATmega32__)
  SP=RAMEND;  // This is done by hardware reset
#endif

  /*
   * With wireless flashing it's possible that this is a remote
   * board that's hard to reset manually.  In this case optiboot can
   * force the watchdog to run before jumping to userspace, so that if
   * a buggy program is uploaded, the board resets automatically.  We
   * still use the watchdog to reset the bootloader too.
   */
#ifdef FORCE_WATCHDOG
  SP = RAMEND - 32;
#define reset_cause (*(uint8_t *) (RAMEND - 16 - 4))
#define marker (*(uint32_t *) (RAMEND - 16 - 3))

  /* GCC does loads Y with SP at the beginning, repeat it with the new SP */
  asm volatile ("in r28, 0x3d");
  asm volatile ("in r29, 0x3e");

  ch = MCUSR;
  MCUSR = 0;
  if ((ch & _BV(WDRF)) && marker == 0xdeadbeef) {
    marker = 0;
    appStart(reset_cause);
  }
  /* Save the original reset reason to pass on to the applicatoin */
  reset_cause = ch;
  marker = 0xdeadbeef;
#else
  // Adaboot no-wait mod
  ch = MCUSR;
  MCUSR = 0;
  if (ch & (_BV(WDRF) | _BV(PORF) | _BV(BORF)))
    appStart(ch);
#endif

#if BSS_SIZE > 0
  // Prepare .data
  asm volatile (
	"	ldi	r17, hi8(__data_end)\n"
	"	ldi	r26, lo8(__data_start)\n"
	"	ldi	r27, hi8(__data_start)\n"
	"	ldi	r30, lo8(__data_load_start)\n"
	"	ldi	r31, hi8(__data_load_start)\n"
	"	rjmp	cpchk\n"
	"copy:	lpm	__tmp_reg__, Z+\n"
	"	st	X+, __tmp_reg__\n"
	"cpchk:	cpi	r26, lo8(__data_end)\n"
	"	cpc	r27, r17\n"
	"	brne	copy\n");
  // Prepare .bss
  asm volatile (
	"	ldi	r17, hi8(__bss_end)\n"
	"	ldi	r26, lo8(__bss_start)\n"
	"	ldi	r27, hi8(__bss_start)\n"
	"	rjmp	clchk\n"
	"clear:	st	X+, __zero_reg__\n"
	"clchk:	cpi	r26, lo8(__bss_end)\n"
	"	cpc	r27, r17\n"
	"	brne	clear\n");
#endif

#if LED_START_FLASHES > 0
  // Set up Timer 1 for timeout counter
  TCCR1B = _BV(CS12) | _BV(CS10); // div 1024
#endif
  /*
   * Disable pullups that may have been enabled by a user program.
   * Somehow a pullup on RXD screws up everything unless RXD is externally
   * driven high.
   */
  DDRD |= 3;
  PORTD &= ~3;
#ifndef SOFT_UART
#if defined(__AVR_ATmega8__) || defined (__AVR_ATmega32__)
  UCSRA = _BV(U2X); //Double speed mode USART
  UCSRB = _BV(RXEN) | _BV(TXEN);  // enable Rx & Tx
  UCSRC = _BV(URSEL) | _BV(UCSZ1) | _BV(UCSZ0);  // config USART; 8N1
  UBRRL = (uint8_t)( (F_CPU + BAUD_RATE * 4L) / (BAUD_RATE * 8L) - 1 );
#else
  UART_SRA = _BV(U2X0); //Double speed mode USART0
  UART_SRB = _BV(RXEN0) | _BV(TXEN0);
  UART_SRC = _BV(UCSZ00) | _BV(UCSZ01);
  UART_SRL = (uint8_t)( (F_CPU + BAUD_RATE * 4L) / (BAUD_RATE * 8L) - 1 );
#endif
#endif
#ifdef RADIO_UART
  radio_init();
#endif

  // Set up watchdog to trigger after 500ms
  watchdogConfig(WATCHDOG_1S);

#if (LED_START_FLASHES > 0) || defined(LED_DATA_FLASH)
  /* Set LED pin as output */
  LED_DDR |= _BV(LED);
#endif

#ifdef SOFT_UART
  /* Set TX pin as output */
  UART_DDR |= _BV(UART_TX_BIT);
#endif

#if LED_START_FLASHES > 0
  /* Flash onboard LED to signal entering of bootloader */
  flash_led(LED_START_FLASHES * 2);
#endif

  /* Forever loop */
  for (;;) {
    /* get character from UART */
    ch = getch();

    if(ch == STK_GET_PARAMETER) {
      unsigned char which = getch();
      verifySpace();
      if (which == 0x82) {
	/*
	 * Send optiboot version as "minor SW version"
	 */
	putch(OPTIBOOT_MINVER);
      } else if (which == 0x81) {
	  putch(OPTIBOOT_MAJVER);
      } else {
	/*
	 * GET PARAMETER returns a generic 0x03 reply for
         * other parameters - enough to keep Avrdude happy
	 */
	putch(0x03);
      }
    }
    else if(ch == STK_SET_DEVICE) {
      // SET DEVICE is ignored
      getNch(20);
    }
    else if(ch == STK_SET_DEVICE_EXT) {
      // SET DEVICE EXT is ignored
      getNch(5);
    }
    else if(ch == STK_LOAD_ADDRESS) {
      // LOAD ADDRESS
      uint16_t newAddress;
      newAddress = getch();
      newAddress |= getch() << 8;
#ifdef RAMPZ
      // Transfer top bit to RAMPZ
      RAMPZ = (newAddress & 0x8000) ? 1 : 0;
#endif
      newAddress <<= 1; // Convert from word address to byte address
      address = newAddress;
      verifySpace();
    }
    else if(ch == STK_UNIVERSAL) {
      // UNIVERSAL command is ignored
      getNch(4);
      putch(0x00);
    }
    /* Write memory, length is big endian and is in bytes */
    else if(ch == STK_PROG_PAGE) {
      // PROGRAM PAGE - we support flash and EEPROM programming
      uint8_t *bufPtr;
      uint16_t addrPtr;
      uint8_t type;

      getch();			/* getlen() */
      length = getch();
      type = getch();

#ifdef SUPPORT_EEPROM
      if (type == 'F')		/* Flash */
#endif
        // If we are in RWW section, immediately start page erase
        if (address < NRWWSTART) __boot_page_erase_short((uint16_t)(void*)address);

      // While that is going on, read in page contents
      bufPtr = buff;
      do *bufPtr++ = getch();
      while (--length);

#ifdef SUPPORT_EEPROM
      if (type == 'F') {	/* Flash */
#endif
        // If we are in NRWW section, page erase has to be delayed until now.
        // Todo: Take RAMPZ into account (not doing so just means that we will
        //  treat the top of both "pages" of flash as NRWW, for a slight speed
        //  decrease, so fixing this is not urgent.)
        if (address >= NRWWSTART) __boot_page_erase_short((uint16_t)(void*)address);

        // Read command terminator, start reply
        verifySpace();

        // If only a partial page is to be programmed, the erase might not be complete.
        // So check that here
        boot_spm_busy_wait();

#ifdef VIRTUAL_BOOT_PARTITION
        if ((uint16_t)(void*)address == 0) {
          // This is the reset vector page. We need to live-patch the code so the
          // bootloader runs.
          //
          // Move RESET vector to WDT vector
          uint16_t vect = buff[0] | (buff[1]<<8);
          rstVect = vect;
          wdtVect = buff[8] | (buff[9]<<8);
          vect -= 4; // Instruction is a relative jump (rjmp), so recalculate.
          buff[8] = vect & 0xff;
          buff[9] = vect >> 8;

          // Add jump to bootloader at RESET vector
          buff[0] = 0x7f;
          buff[1] = 0xce; // rjmp 0x1d00 instruction
        }
#endif

        // Copy buffer into programming buffer
        bufPtr = buff;
        addrPtr = (uint16_t)(void*)address;
        ch = SPM_PAGESIZE / 2;
        do {
          uint16_t a;
          a = *bufPtr++;
          a |= (*bufPtr++) << 8;
          __boot_page_fill_short((uint16_t)(void*)addrPtr,a);
          addrPtr += 2;
        } while (--ch);

        // Write from programming buffer
        __boot_page_write_short((uint16_t)(void*)address);
        boot_spm_busy_wait();

#if defined(RWWSRE)
        // Reenable read access to flash
        boot_rww_enable();
#endif
#ifdef SUPPORT_EEPROM
      } else if (type == 'E') {	/* EEPROM */
        // Read command terminator, start reply
        verifySpace();

        length = bufPtr - buff;
        addrPtr = address;
        bufPtr = buff;
        while (length--) {
          watchdogReset();
          eeprom_write(addrPtr++, *bufPtr++);
        }
      }
#endif
    }
    /* Read memory block mode, length is big endian.  */
    else if(ch == STK_READ_PAGE) {
      // READ PAGE - we only read flash and EEPROM
      uint8_t type;

      getch();			/* getlen() */
      length = getch();
      type = getch();

      verifySpace();
      /* TODO: putNch */
#ifdef SUPPORT_EEPROM
      if (type == 'F')
#endif
        do {
#ifdef VIRTUAL_BOOT_PARTITION
          // Undo vector patch in bottom page so verify passes
          if (address == 0)       ch=rstVect & 0xff;
          else if (address == 1)  ch=rstVect >> 8;
          else if (address == 8)  ch=wdtVect & 0xff;
          else if (address == 9) ch=wdtVect >> 8;
          else ch = pgm_read_byte_near(address);
          address++;
#elif defined(RAMPZ)
          // Since RAMPZ should already be set, we need to use EPLM directly.
          // Also, we can use the autoincrement version of lpm to update "address"
          //      do putch(pgm_read_byte_near(address++));
          //      while (--length);
          // read a Flash and increment the address (may increment RAMPZ)
          __asm__ ("elpm %0,Z+\n" : "=r" (ch), "=z" (address): "1" (address));
#else
          // read a Flash byte and increment the address
          __asm__ ("lpm %0,Z+\n" : "=r" (ch), "=z" (address): "1" (address));
#endif
          putch(ch);
        } while (--length);
#ifdef SUPPORT_EEPROM
      else if (type == 'E')
        while (length--)
          putch(eeprom_read(address++));
#endif
    }

    /* Get device signature bytes  */
    else if(ch == STK_READ_SIGN) {
      // READ SIGN - return what Avrdude wants to hear
      verifySpace();
      putch(SIGNATURE_0);
      putch(SIGNATURE_1);
      putch(SIGNATURE_2);
    }
    else if (ch == STK_LEAVE_PROGMODE) { /* 'Q' */
      // Adaboot no-wait mod
      watchdogConfig(WATCHDOG_16MS);
      verifySpace();
    }
    else {
      // This covers the response to commands like STK_ENTER_PROGMODE
      verifySpace();
    }
    putch(STK_OK);
  }
}

#ifdef RADIO_UART
/*
 * Radio mode gets set the moment we receive any command over the radio chip.
 * From that point our responses will also be sent through the radio instead
 * of through the UART.  Otherwise all communication goes through the UART
 * as normal.
 *
 * TODO: require a challenge-response negotiation at least to start the
 * radio mode, for security -- the keys need to be stored in EEPROM.  Ideally
 * we'd encrypt all communication but that would be an overkill for the
 * bootloader.
 */
static uint8_t radio_mode = 0;
static uint8_t radio_present = 0;
static uint8_t pkt_max_len = 32;

#warning Make sure pin config matches hardware setup.
#warning Here CE  = PIN9  (PORTB1)
#warning Here CSN = PIN10 (PORTB2)
#define CE_DDR		DDRB
#define CE_PORT		PORTB
#define CSN_DDR		DDRB
#define CSN_PORT	PORTB
#define CE_PIN		(1 << 1)
#define CSN_PIN		(1 << 2)

#include "spi.h"
#include "nrf24.h"

#define SEQN

static void radio_init(void) {
  uint8_t addr[3];

  spi_init();

  if (nrf24_init())
    return;

  radio_present = 1;
  /*
   * Set our own address.
   *
   * The remote end's address will be set according to the contents
   * of the first packet we receive from the master.
   */
  /*
  Since addresses defined by me, have a first value between 0x20 - 0x40 exclusive
  */
   addr[0] = eeprom_read(0);
	if(addr[0] > 0x40||addr[0] < 0x20){
	   eeprom_write_byte(0,0x30);
	   eeprom_write_byte(1,0x30);
	   eeprom_write_byte(2,0x31);
	   addr[0] = 0x30;
	}
   addr[1] = eeprom_read(1);
   addr[2] = eeprom_read(2);
  nrf24_set_rx_addr(addr);

  nrf24_rx_mode();
}
#endif

void putch(char ch) {
#ifdef RADIO_UART
  if (radio_mode) {
    static uint8_t pkt_len = 0;
    static uint8_t pkt_buf[32];

    pkt_buf[pkt_len++] = ch;

    if (ch == STK_OK || pkt_len == pkt_max_len) {
#ifdef SEQN
      uint8_t cnt = 128;

      while (--cnt) {
        /* Wait 4ms to allow the remote end to switch to Rx mode */
        my_delay(4000);

        nrf24_tx(pkt_buf, pkt_len);
        if (!nrf24_tx_result_wait())
          break;

        /*
	 * TODO: also check if there's anything in the Rx FIFO - that
	 * would indicate that the other side has actually received our
	 * packet but the ACK may have been lost instead.  In any case
	 * the other side is not listening for what we're re-sending,
	 * maybe has given up and is resending the full command which
	 * is ok.
	 */
      }

      pkt_len = 1;
      pkt_buf[0] ++;
#else
      /* Wait 4ms to allow the remote end to switch to Rx mode */
      my_delay(4000);

      nrf24_tx(pkt_buf, pkt_len);
      nrf24_tx_result_wait();

      pkt_len = 0;
#endif
    }

    return;
  }
#endif
#ifndef SOFT_UART
  while (!(UART_SRA & _BV(UDRE0)));
  UART_UDR = ch;
#else
  __asm__ __volatile__ (
    "   com %[ch]\n" // ones complement, carry set
    "   sec\n"
    "1: brcc 2f\n"
    "   cbi %[uartPort],%[uartBit]\n"
    "   rjmp 3f\n"
    "2: sbi %[uartPort],%[uartBit]\n"
    "   nop\n"
    "3: rcall uartDelay\n"
    "   rcall uartDelay\n"
    "   lsr %[ch]\n"
    "   dec %[bitcnt]\n"
    "   brne 1b\n"
    :
    :
      [bitcnt] "d" (10),
      [ch] "r" (ch),
      [uartPort] "I" (_SFR_IO_ADDR(UART_PORT)),
      [uartBit] "I" (UART_TX_BIT)
    :
      "r25"
  );
#endif
}

uint8_t getch(void) {
  uint8_t ch;
#ifdef RADIO_UART
  static uint8_t pkt_len = 0, pkt_start = 0;
  static uint8_t pkt_buf[32];
#endif

#ifdef LED_DATA_FLASH
#if defined(__AVR_ATmega8__) || defined (__AVR_ATmega32__)
  LED_PORT ^= _BV(LED);
#else
  LED_PIN |= _BV(LED);
#endif
#endif

#ifdef SOFT_UART
  __asm__ __volatile__ (
    "1: sbic  %[uartPin],%[uartBit]\n"  // Wait for start edge
    "   rjmp  1b\n"
    "   rcall uartDelay\n"          // Get to middle of start bit
    "2: rcall uartDelay\n"              // Wait 1 bit period
    "   rcall uartDelay\n"              // Wait 1 bit period
    "   clc\n"
    "   sbic  %[uartPin],%[uartBit]\n"
    "   sec\n"
    "   dec   %[bitCnt]\n"
    "   breq  3f\n"
    "   ror   %[ch]\n"
    "   rjmp  2b\n"
    "3:\n"
    :
      [ch] "=r" (ch)
    :
      [bitCnt] "d" (9),
      [uartPin] "I" (_SFR_IO_ADDR(UART_PIN)),
      [uartBit] "I" (UART_RX_BIT)
    :
      "r25"
);
#else
  while(1) {
	  
#ifndef RADIO_UART
    if (UART_SRA & _BV(RXC0)) {
      if (!(UART_SRA & _BV(FE0))) {
        /*
         * A Framing Error indicates (probably) that something is talking
         * to us at the wrong bit rate.  Assume that this is because it
         * expects to be talking to the application, and DON'T reset the
         * watchdog.  This should cause the bootloader to abort and run
         * the application "soon", if it keeps happening.  (Note that we
         * don't care that an invalid char is returned...)
         */
        watchdogReset();
      }

      ch = UART_UDR;
      break;
    }
#endif

#ifdef RADIO_UART
    if (radio_present && (pkt_len || nrf24_rx_fifo_data())) {
      watchdogReset();

      if (!pkt_len) {
#ifdef SEQN
        static uint8_t seqn = 0xff;
#define START 1
#else
#define START 0
#endif
        nrf24_rx_read(pkt_buf, &pkt_len);
        pkt_start = START;

        if (!radio_mode && pkt_len >= 4) {
          /*
           * If this is the first packet we receive, the first three bytes
           * should contain the sender's address.
           */
          nrf24_set_tx_addr(pkt_buf);
          pkt_max_len = pkt_buf[3];
          pkt_len -= 4;
          pkt_start += 4;

          radio_mode = 1;
        } else if (!radio_mode)
          pkt_len = 0;

        if (!pkt_len)
          continue;

#ifdef SEQN
        if (pkt_buf[0] == seqn) {
          pkt_len = 0;
          continue;
        }

        seqn = pkt_buf[0];
        pkt_len--;
#endif
      }

      ch = pkt_buf[pkt_start ++];
      pkt_len --;
      break;
    }
#endif
  }
#endif

#ifdef LED_DATA_FLASH
#if defined(__AVR_ATmega8__) || defined (__AVR_ATmega32__)
  LED_PORT ^= _BV(LED);
#else
  LED_PIN |= _BV(LED);
#endif
#endif

  return ch;
}

#ifdef SOFT_UART
// AVR305 equation: #define UART_B_VALUE (((F_CPU/BAUD_RATE)-23)/6)
// Adding 3 to numerator simulates nearest rounding for more accurate baud rates
#define UART_B_VALUE (((F_CPU/BAUD_RATE)-20)/6)
#if UART_B_VALUE > 255
#error Baud rate too slow for soft UART
#endif

void uartDelay() {
  __asm__ __volatile__ (
    "ldi r25,%[count]\n"
    "1:dec r25\n"
    "brne 1b\n"
    "ret\n"
    ::[count] "M" (UART_B_VALUE)
  );
}
#endif

void getNch(uint8_t count) {
  do getch(); while (--count);
  verifySpace();
}

void wait_timeout(void) {
#ifdef RADIO_UART
  nrf24_idle_mode(0);		      // power the radio off
#endif
  watchdogConfig(WATCHDOG_16MS);      // shorten WD timeout
  while (1)			      // and busy-loop so that WD causes
    ;				      //  a reset and app start.
}

void verifySpace(void) {
  char ch;
  ch = getch();
  if (ch!= CRC_EOP)
    wait_timeout();
  putch(STK_INSYNC);
}

#if LED_START_FLASHES > 0
void flash_led(uint8_t count) {
  do {
    TCNT1 = -(F_CPU/(1024*16));
    TIFR1 = _BV(TOV1);
    while(!(TIFR1 & _BV(TOV1)));
#if defined(__AVR_ATmega8__)  || defined (__AVR_ATmega32__)
    LED_PORT ^= _BV(LED);
#else
    LED_PIN |= _BV(LED);
#endif
    watchdogReset();
  } while (--count);
}
#endif

// Watchdog functions. These are only safe with interrupts turned off.
void watchdogReset() {
  __asm__ __volatile__ (
    "wdr\n"
  );
}

void watchdogConfig(uint8_t x) {
  WDTCSR = _BV(WDCE) | _BV(WDE);
  WDTCSR = x;
}

void appStart(uint8_t rstFlags) {
#ifdef FORCE_WATCHDOG
  watchdogConfig(WATCHDOG_4S);
#else
  watchdogConfig(WATCHDOG_OFF);
#endif

  // save the reset flags in the designated register
  //  This can be saved in a main program by putting code in .init0 (which
  //  executes before normal c init code) to save R2 to a global variable.
  __asm__ __volatile__ ("mov r2, %0\n" :: "r" (rstFlags));

  __asm__ __volatile__ (
#ifdef VIRTUAL_BOOT_PARTITION
    // Jump to WDT vector
    "ldi r30,4\n"
    "clr r31\n"
#else
    // Jump to RST vector
    "clr r30\n"
    "clr r31\n"
#endif
    "ijmp\n"
  );
}
