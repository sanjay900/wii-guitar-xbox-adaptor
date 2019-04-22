#pragma once

/* Includes: */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include "../../../config/config.h"
#include "../lufa/Descriptors.h"
#include "../Controller.h"
#include "HidOutput.h"
#include "Output.h"
#include "Descriptors.h"
#include <LUFA/Drivers/USB/USB.h>
#include <LUFA/Platform/Platform.h>

/** Type define for the gamepad HID report structure, for creating and sending HID reports to the host PC.
 *  This mirrors the layout described to the host in the HID report descriptor, in Descriptors.c.
 */
typedef struct
{
  uint16_t r_x;
  uint16_t r_y;
  uint16_t Button; /**< Bit mask of the currently pressed gamepad buttons */
} USB_GamepadReport_Data_t;