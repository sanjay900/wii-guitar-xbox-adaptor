#pragma once
#include "controller/controller.h"
#include <stdint.h>

void fillMouseReport(void *ReportData, uint16_t *const ReportSize,
                            Controller_t *controller);