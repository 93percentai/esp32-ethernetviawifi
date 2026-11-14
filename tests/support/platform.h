#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_TIMEOUT 5
#define ESP_INTR_FLAG_LOWMED 0
#define CONFIG_IDF_TARGET_ESP32S31 1
#define CONFIG_KORVO_USB_CONFIGURATION 1
#ifndef CONFIG_KORVO_USB_AUTO_CONFIGURATION
#define CONFIG_KORVO_USB_AUTO_CONFIGURATION 1
#endif
#define CONFIG_KORVO_USB_TRACE_CONTROL 0
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct { unsigned count, capacity, size, head; uint8_t *bytes; } *QueueHandle_t;
typedef int *SemaphoreHandle_t;
static inline QueueHandle_t xQueueCreate(unsigned capacity, unsigned size) {
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q); q->capacity = capacity; q->size = size; q->bytes = calloc(capacity, size); assert(q->bytes); return q;
}
static inline int xQueueSend(QueueHandle_t q, const void *item, unsigned timeout) {
    (void)timeout; if (q->count == q->capacity) return pdFALSE;
    memcpy(q->bytes + ((q->head + q->count) % q->capacity) * q->size, item, q->size); ++q->count; return pdTRUE;
}
static inline int xQueueReceive(QueueHandle_t q, void *item, unsigned timeout) {
    (void)timeout; if (!q->count) return pdFALSE;
    memcpy(item, q->bytes + q->head * q->size, q->size); q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
static inline unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
static inline int xQueueReset(QueueHandle_t q) { q->head = q->count = 0; return pdTRUE; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(int)); }
static inline int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) { (void)lock; (void)timeout; return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t lock) { (void)lock; return pdTRUE; }
static inline int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *task) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)task; return pdPASS;
}
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
static inline void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
static inline const char *esp_err_to_name(int error) { (void)error; return "test error"; }
int64_t esp_timer_get_time(void);
#define WIFI_IF_STA 0
esp_err_t esp_wifi_internal_tx(int interface, void *data, size_t size);
void esp_wifi_internal_free_rx_buffer(void *buffer);
esp_err_t esp_wifi_internal_reg_rxcb(int interface, esp_err_t (*cb)(void *, uint16_t, void *));
