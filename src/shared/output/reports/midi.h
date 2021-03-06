#pragma once
#include "eeprom/eeprom.h"
#include "controller/controller.h"
#include "input/input_handler.h"
#include "midi.h"
#include "output/controller_structs.h"
#include "output/descriptors.h"
#include <stdint.h>

uint8_t lastmidi[XBOX_BTN_COUNT + XBOX_AXIS_COUNT];
MidiConfig_t midiConfig;
void fillMIDIReport(void *ReportData, uint8_t *const ReportSize,
                    Controller_t *controller) {
  USB_MIDI_Data_t *data = ReportData;
  data->rid = REPORT_ID_MIDI;
  uint8_t idx = 0;
  for (int i = 0; i < XBOX_BTN_COUNT + XBOX_AXIS_COUNT; i++) {
    if (midiConfig.type[i] != DISABLED) {
      // Channel 10(percussion)
      uint8_t channel = midiConfig.channel[i];
      uint8_t midipitch = midiConfig.note[i];
      uint8_t midicommand = midiConfig.type[i] == NOTE
                                ? MIDI_COMMAND_NOTE_ON
                                : MIDI_COMMAND_CONTROL_CHANGE;
      uint8_t vel = getVelocity(controller, i) >> 1;
      if (lastmidi[i] == vel) continue;
      lastmidi[i] = vel;
      data->midi[idx].Event = MIDI_EVENT(0, midicommand);
      data->midi[idx].Data1 = midicommand | channel;
      data->midi[idx].Data2 = midipitch;
      data->midi[idx].Data3 = vel;
      idx++;
    }
  }

  *ReportSize = 1 + idx * sizeof(MIDI_EventPacket_t);
}
void initMIDI(Configuration_t* config) {
  midiConfig = config->midi;
}