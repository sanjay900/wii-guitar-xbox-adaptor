#pragma once
#ifdef __AVR__
#  include "controller/controller.h"
#  define I2C_ADDR 0xa4
#  define I2C_ADDR2 0x52
void tickWiiExtInput(Controller_t *controller);
extern uint16_t wiiExtensionID;
void initWiiInput(void);
#endif
enum WiiExtType {
  WII_NUNCHUK = 0x0000,
  WII_CLASSIC_CONTROLLER = 0x0001,
  WII_CLASSIC_CONTROLLER_PRO = 0x0101,
  WII_THQ_UDRAW_TABLET = 0xFF12,
  WII_UBISOFT_DRAWSOME_TABLET = 0xFF13,
  WII_GUITAR_HERO_GUITAR_CONTROLLER = 0x0003,
  WII_GUITAR_HERO_DRUM_CONTROLLER = 0x0103,
  WII_DJ_HERO_TURNTABLE = 0x0303,
  WII_TAIKO_NO_TATSUJIN_CONTROLLER = 0x0011,
  WII_MOTION_PLUS = 0x0005,
  WII_NO_EXTENSION = 0x180b,
  WII_NOT_INITIALISED = 0xFFFF
};