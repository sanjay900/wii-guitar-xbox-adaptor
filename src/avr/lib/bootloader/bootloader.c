#include "bootloader/bootloader.h"
#include "util/util.h"
#include <avr/interrupt.h>
#include <avr/wdt.h>
#define MAGIC_KEY_POS 0x0800
#define MAGIC_KEY 0x7777
volatile uint16_t *const bootKeyPtr = (volatile uint16_t *)MAGIC_KEY_POS;
uint16_t bootKeyVal;
void reboot(void) {
  cli();
  wdt_enable(WDTO_15MS);
  for (;;) {}
}
void bootloader(void) {
  // write magic key to ram
  *bootKeyPtr = MAGIC_KEY;

  reboot();
}