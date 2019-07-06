#pragma once
#include "output_handler.h"

#define SIMULTANEOUS_KEYS 6

#define CHECK_JOY_KEY(joy)                                                     \
  if (usedKeys < SIMULTANEOUS_KEYS) {                                          \
    if (config.keys.joy.neg &&                                                 \
        last_controller.joy < -(int)config.threshold_joy) {                    \
      KeyboardReport->KeyCode[usedKeys++] = config.keys.joy.neg;               \
    }                                                                          \
    if (config.keys.joy.pos &&                                                 \
        last_controller.joy > (int)config.threshold_joy) {                     \
      KeyboardReport->KeyCode[usedKeys++] = config.keys.joy.pos;               \
    }                                                                          \
  }

#define CHECK_TRIGGER_KEY(trigger)                                             \
  if (usedKeys < SIMULTANEOUS_KEYS) {                                          \
    if (config.keys.trigger &&                                                 \
        last_controller.trigger > (int)config.threshold_trigger) {             \
      KeyboardReport->KeyCode[usedKeys++] = config.keys.trigger;               \
    }                                                                          \
  }

void keyboard_init(event_pointers *events, const void **const report_descriptor,
                   uint16_t *report_descriptor_size,
                   USB_ClassInfo_HID_Device_t *hid_device,
                   USB_Descriptor_Device_t *DeviceDescriptor);