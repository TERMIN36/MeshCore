#pragma once

#include <Arduino.h>

// heltec-eink-modules waits on BUSY by spinning yield(), and it has no
// callback. A same-priority task runs on that yield and samples the buttons.
// The mutex stops that task from calling check() once the wave is over and
// the main loop is sampling the pins again.

static void (*s_eink_poll)() = nullptr;
static volatile bool s_eink_wave = false;
static TaskHandle_t s_eink_poll_task = nullptr;
static SemaphoreHandle_t s_eink_poll_lock = nullptr;

static void einkPollTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (s_eink_wave) {
      if (s_eink_poll_lock) xSemaphoreTake(s_eink_poll_lock, portMAX_DELAY);
      if (s_eink_wave && s_eink_poll) s_eink_poll();
      if (s_eink_poll_lock) xSemaphoreGive(s_eink_poll_lock);
      if (!s_eink_wave) break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

static void einkYieldPollSet(void (*fn)()) {
  s_eink_poll = fn;
  if (!s_eink_poll_lock) s_eink_poll_lock = xSemaphoreCreateMutex();
  if (!s_eink_poll_task && fn) {
    xTaskCreate(einkPollTask, "einkbtn", 3072, nullptr, 1, &s_eink_poll_task);
  }
}

static void einkYieldPollEnter() {
  s_eink_wave = true;
  if (s_eink_poll_task) xTaskNotifyGive(s_eink_poll_task);
}

static void einkYieldPollLeave() {
  s_eink_wave = false;
  if (!s_eink_poll_lock) return;
  xSemaphoreTake(s_eink_poll_lock, portMAX_DELAY);
  xSemaphoreGive(s_eink_poll_lock);
}
