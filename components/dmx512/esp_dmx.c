#include "esp_dmx.h"

#include <string.h>

#include "dmx_types.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "endian.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rdm.h"
#include "esp_task_wdt.h"
#include "private/dmx_hal.h"
#include "private/driver.h"
#include "private/rdm_encode/types.h"
#include "rdm_types.h"

#if ESP_IDF_VERSION_MAJOR >= 5
#include "driver/gptimer.h"
#include "esp_private/periph_ctrl.h"
#include "esp_timer.h"
#else
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#endif

#ifdef CONFIG_DMX_ISR_IN_IRAM
#define DMX_ISR_ATTR IRAM_ATTR
#else
#define DMX_ISR_ATTR
#endif

// Used for argument checking at the beginning of each function.
#define DMX_CHECK(a, err_code, format, ...) \
  ESP_RETURN_ON_FALSE(a, err_code, TAG, format, ##__VA_ARGS__)

DRAM_ATTR dmx_driver_t *dmx_driver[DMX_NUM_MAX] = {0};
DRAM_ATTR spinlock_t dmx_spinlock[DMX_NUM_MAX] = {portMUX_INITIALIZER_UNLOCKED,
                                                  portMUX_INITIALIZER_UNLOCKED,
#if DMX_NUM_MAX > 2
                                                  portMUX_INITIALIZER_UNLOCKED
#endif
};

enum dmx_default_interrupt_values_t {
  DMX_UART_FULL_DEFAULT = 1,   // RX FIFO full default interrupt threshold.
  DMX_UART_EMPTY_DEFAULT = 8,  // TX FIFO empty default interrupt threshold.
};

enum dmx_interrupt_mask_t {
  DMX_INTR_RX_FIFO_OVERFLOW = UART_INTR_RXFIFO_OVF,
  DMX_INTR_RX_FRAMING_ERR = UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR,
  DMX_INTR_RX_ERR = DMX_INTR_RX_FIFO_OVERFLOW | DMX_INTR_RX_FRAMING_ERR,

  DMX_INTR_RX_BREAK = UART_INTR_BRK_DET,
  DMX_INTR_RX_DATA = UART_INTR_RXFIFO_FULL,
  DMX_INTR_RX_ALL = DMX_INTR_RX_DATA | DMX_INTR_RX_BREAK | DMX_INTR_RX_ERR,

  DMX_INTR_TX_DATA = UART_INTR_TXFIFO_EMPTY,
  DMX_INTR_TX_DONE = UART_INTR_TX_DONE,
  DMX_INTR_TX_ALL = DMX_INTR_TX_DATA | DMX_INTR_TX_DONE,

  DMX_ALL_INTR_MASK = -1
};

enum rdm_packet_timing_t {
  RDM_DISCOVERY_NO_RESPONSE_PACKET_SPACING = 5800,
  RDM_REQUEST_NO_RESPONSE_PACKET_SPACING = 3000,
  RDM_BROADCAST_PACKET_SPACING = 176,
  RDM_RESPOND_TO_REQUEST_PACKET_SPACING = 176,

  RDM_CONTROLLER_RESPONSE_LOST_TIMEOUT = 2800,
  RDM_RESPONDER_RESPONSE_LOST_TIMEOUT = 2000
};

enum rdm_packet_type_t {
  RDM_PACKET_TYPE_NON_RDM = 0,
  RDM_PACKET_TYPE_DISCOVERY = BIT(0),
  RDM_PACKET_TYPE_DISCOVERY_RESPONSE = BIT(1),
  RDM_PACKET_TYPE_REQUEST = BIT(2),
  RDM_PACKET_TYPE_RESPONSE = BIT(3),
  RDM_PACKET_TYPE_BROADCAST = BIT(4),

  RDM_PACKET_TYPE_EARLY_TIMEOUT =
      (RDM_PACKET_TYPE_REQUEST | RDM_PACKET_TYPE_DISCOVERY)
};

static void DMX_ISR_ATTR dmx_uart_isr(void *arg) {
  const int64_t now = esp_timer_get_time();
  dmx_driver_t *const driver = arg;
  spinlock_t *const restrict spinlock = &dmx_spinlock[driver->dmx_num];
  uart_dev_t *const restrict uart = driver->uart;
  int task_awoken = false;

  while (true) {
    const uint32_t intr_flags = dmx_uart_get_interrupt_status(uart);
    if (intr_flags == 0) break;

    // DMX Receive ####################################################
    if (intr_flags & DMX_INTR_RX_ALL) {
      // Stop the RDM receive alarm (which may or may not be running)
#if ESP_IDF_VERSION_MAJOR >= 5
      gptimer_stop(driver->gptimer_handle);
#else
      timer_group_set_counter_enable_in_isr(driver->timer_group,
                                            driver->timer_idx, 0);
#endif

      // Read data into the DMX buffer if there is enough space
      const bool is_in_break = intr_flags & DMX_INTR_RX_BREAK;
      if (driver->data.head >= 0 && driver->data.head < DMX_MAX_PACKET_SIZE) {
        int read_len = DMX_MAX_PACKET_SIZE - driver->data.head;
        if (is_in_break) --read_len;  // Ignore DMX breaks

        // Read the UART data into the DMX buffer and increment the head
        uint8_t *current_slot = &driver->data.buffer[driver->data.head];
        dmx_uart_read_rxfifo(uart, current_slot, &read_len);
        driver->data.head += read_len;

        // Handle receiving a valid packet with larger than expected size
        if (driver->data.head > driver->data.rx_size) {
          driver->data.rx_size = driver->data.head;
        }

      } else {
        // Record the number of slots received for error reporting
        if (driver->data.head > 0) {
          driver->data.head += dmx_uart_get_rxfifo_len(uart);
        }
        dmx_uart_rxfifo_reset(uart);
      }

      // Handle DMX break condition
      if (is_in_break) {
        // Handle receiveing a valid packet with smaller than expected size
        if (!driver->end_of_packet && driver->data.head > 0 &&
            driver->data.head < DMX_MAX_PACKET_SIZE) {
          driver->data.rx_size = driver->data.head - 1;
        }

        // Set driver flags
        taskENTER_CRITICAL_ISR(spinlock);
        driver->end_of_packet = false;  // A new packet is being received
        driver->data.head = 0;          // Driver is ready for data
        taskEXIT_CRITICAL_ISR(spinlock);
      }

      // Set data timestamp, DMX break flag, and clear interrupts
      taskENTER_CRITICAL_ISR(spinlock);
      driver->data.timestamp = now;
      driver->is_in_break = is_in_break;
      taskEXIT_CRITICAL_ISR(spinlock);
      dmx_uart_clear_interrupt(uart, DMX_INTR_RX_ALL);

      // Don't process data if end-of-packet condition already reached
      if (driver->end_of_packet) {
        continue;
      }

      // Handle DMX errors or process DMX data
      esp_err_t packet_err = ESP_OK;
      rdm_response_type_t packet_type = RDM_PACKET_TYPE_NON_RDM;
      if (intr_flags & DMX_INTR_RX_ERR) {
        packet_err = intr_flags & DMX_INTR_RX_FRAMING_ERR
                         ? ESP_FAIL               // Missing stop bits
                         : ESP_ERR_NOT_FINISHED;  // UART overflow
      } else if (intr_flags & DMX_INTR_RX_DATA) {
        // Determine if a complete packet has been received
        const rdm_data_t *const rdm = (rdm_data_t *)driver->data.buffer;

        if (rdm->sc == RDM_SC && rdm->sub_sc == RDM_SUB_SC) {
          // The packet is a standard RDM packet
          if (driver->data.head < RDM_BASE_PACKET_SIZE ||
              driver->data.head < rdm->message_len + 2) {
            continue;  // Guard against base packet size too small
          }

          // Determine the RDM packet type
          if (rdm->cc == RDM_CC_DISC_COMMAND &&
              rdm->pid == bswap16(RDM_PID_DISC_UNIQUE_BRANCH)) {
            packet_type = RDM_PACKET_TYPE_DISCOVERY;
          } else if (rdm_uid_is_broadcast(buf_to_uid(rdm->destination_uid))) {
            packet_type = RDM_PACKET_TYPE_BROADCAST;
          } else if (rdm->cc == RDM_CC_GET_COMMAND ||
                     rdm->cc == RDM_CC_SET_COMMAND ||
                     rdm->cc == RDM_CC_DISC_COMMAND) {
            packet_type = RDM_PACKET_TYPE_REQUEST;
          } else {
            packet_type = RDM_PACKET_TYPE_RESPONSE;
          }

        } else if (rdm->sc == RDM_PREAMBLE || rdm->sc == RDM_DELIMITER) {
          // The packet is a DISC_UNIQUE_BRANCH response
          if (driver->data.head < 17) {
            continue;  // Guard against base packet size too small
          }

          // Find the length of the preamble (0 to 7 bytes)
          size_t preamble_len = 0;
          for (; preamble_len < 7; ++preamble_len) {
            if (driver->data.buffer[preamble_len] == RDM_DELIMITER) {
              break;
            }
          }
          if (driver->data.buffer[preamble_len] != RDM_DELIMITER ||
              driver->data.head < preamble_len + 17) {
            continue;  // Delimiter not found or packet not complete yet
          }
          packet_type = RDM_PACKET_TYPE_DISCOVERY_RESPONSE;

        } else if (driver->data.head < driver->data.rx_size) {
          continue;  // Guard against smaller DMX packets than expected
        }
      } else {
        // This code should never run, but can prevent crashes just in case!
        dmx_uart_disable_interrupt(uart, ~(DMX_INTR_RX_ALL | DMX_INTR_TX_ALL));
        dmx_uart_clear_interrupt(uart, ~(DMX_INTR_RX_ALL | DMX_INTR_TX_ALL));
        continue;
      }

      // Set driver flags and notify task
      taskENTER_CRITICAL_ISR(spinlock);
      driver->new_packet = true;
      driver->end_of_packet = true;
      driver->data.sent_last = false;
      driver->data.type = packet_type;
      driver->data.err = packet_err;
      if (driver->task_waiting) {
        xTaskNotifyFromISR(driver->task_waiting, driver->data.head,
                           eSetValueWithOverwrite, &task_awoken);
      }
      taskEXIT_CRITICAL_ISR(spinlock);
    }

    // DMX Transmit #####################################################
    else if (intr_flags & DMX_INTR_TX_DATA) {
      // Write data to the UART and clear the interrupt
      size_t write_size = driver->data.tx_size - driver->data.head;
      const uint8_t *src = &driver->data.buffer[driver->data.head];
      dmx_uart_write_txfifo(uart, src, &write_size);
      driver->data.head += write_size;
      dmx_uart_clear_interrupt(uart, DMX_INTR_TX_DATA);

      // Allow FIFO to empty when done writing data
      if (driver->data.head == driver->data.tx_size) {
        dmx_uart_disable_interrupt(uart, DMX_INTR_TX_DATA);
      }
    }

    else if (intr_flags & DMX_INTR_TX_DONE) {
      // Disable write interrupts and clear the interrupt
      dmx_uart_disable_interrupt(uart, DMX_INTR_TX_ALL);
      dmx_uart_clear_interrupt(uart, DMX_INTR_TX_DONE);

      // Record timestamp, unset sending flag, and notify task
      taskENTER_CRITICAL_ISR(spinlock);
      driver->is_sending = false;
      driver->data.timestamp = now;
      if (driver->task_waiting) {
        xTaskNotifyFromISR(driver->task_waiting, 0, eNoAction, &task_awoken);
      }
      taskEXIT_CRITICAL_ISR(spinlock);

      // Turn DMX bus around quickly if expecting an RDM response
      bool expecting_response = false;
      if (driver->data.type == RDM_PACKET_TYPE_DISCOVERY) {
        expecting_response = true;
        driver->data.head = 0;  // Not expecting a DMX break
      } else if (driver->data.type == RDM_PACKET_TYPE_REQUEST) {
        expecting_response = true;
        driver->data.head = -1;  // Expecting a DMX break
      }
      if (expecting_response) {
        taskENTER_CRITICAL_ISR(spinlock);
        driver->end_of_packet = false;
        driver->new_packet = false;
        dmx_uart_rxfifo_reset(uart);
        dmx_uart_set_rts(uart, 1);
        taskEXIT_CRITICAL_ISR(spinlock);
      }
    }
  }

  if (task_awoken) portYIELD_FROM_ISR();
}

static void DMX_ISR_ATTR dmx_gpio_isr(void *arg) {
  const int64_t now = esp_timer_get_time();
  dmx_driver_t *const driver = (dmx_driver_t *)arg;
  int task_awoken = false;

  if (dmx_uart_get_rx_level(driver->uart)) {
    /* If this ISR is called on a positive edge and the current DMX frame is in
    a break and a negative edge timestamp has been recorded then a break has
    just finished. Therefore the DMX break length is able to be recorded. It can
    also be deduced that the driver is now in a DMX mark-after-break. */

    if (driver->is_in_break && driver->sniffer.last_neg_edge_ts > -1) {
      driver->sniffer.data.break_len = now - driver->sniffer.last_neg_edge_ts;
      driver->sniffer.is_in_mab = true;
      driver->is_in_break = false;
    }
    driver->sniffer.last_pos_edge_ts = now;
  } else {
    /* If this ISR is called on a negative edge in a DMX mark-after-break then
    the DMX mark-after-break has just finished. It can be recorded. Sniffer data
    is now available to be read by the user. */

    if (driver->sniffer.is_in_mab) {
      driver->sniffer.data.mab_len = now - driver->sniffer.last_pos_edge_ts;
      driver->sniffer.is_in_mab = false;

      // Send the sniffer data to the queue
      xQueueOverwriteFromISR(driver->sniffer.queue, &driver->sniffer.data,
                             &task_awoken);
    }
    driver->sniffer.last_neg_edge_ts = now;
  }

  if (task_awoken) portYIELD_FROM_ISR();
}

static bool DMX_ISR_ATTR dmx_timer_isr(
#if ESP_IDF_VERSION_MAJOR >= 5
    gptimer_handle_t gptimer_handle,
    const gptimer_alarm_event_data_t *event_data,
#endif
    void *arg) {
  dmx_driver_t *const restrict driver = (dmx_driver_t *)arg;
  int task_awoken = false;

  if (driver->is_sending) {
    if (driver->is_in_break) {
      dmx_uart_invert_tx(driver->uart, 0);
      driver->is_in_break = false;

      // Reset the alarm for the end of the DMX mark-after-break
#if ESP_IDF_VERSION_MAJOR >= 5
      const gptimer_alarm_config_t alarm_config = {
          .alarm_count = driver->mab_len,
          .flags.auto_reload_on_alarm = false};
      gptimer_set_alarm_action(gptimer_handle, &alarm_config);
#else
      timer_group_set_alarm_value_in_isr(driver->timer_group, driver->timer_idx,
                                         driver->mab_len);
#endif
    } else {
      // Write data to the UART
      size_t write_size = driver->data.tx_size;
      dmx_uart_write_txfifo(driver->uart, driver->data.buffer, &write_size);
      driver->data.head += write_size;

      // Pause MAB timer alarm
#if ESP_IDF_VERSION_MAJOR >= 5
      gptimer_stop(gptimer_handle);
      gptimer_set_raw_count(gptimer_handle, 0);
#else
      timer_group_set_counter_enable_in_isr(driver->timer_group,
                                            driver->timer_idx, 0);
#endif
      // Enable DMX write interrupts
      dmx_uart_enable_interrupt(driver->uart, DMX_INTR_TX_ALL);
    }
  } else if (driver->task_waiting) {
    // Notify the task
    xTaskNotifyFromISR(driver->task_waiting, driver->data.head,
                       eSetValueWithOverwrite, &task_awoken);

    // Pause the receive timer alarm
#if ESP_IDF_VERSION_MAJOR >= 5
    gptimer_stop(gptimer_handle);
    gptimer_set_raw_count(gptimer_handle, 0);
#else
    timer_group_set_counter_enable_in_isr(driver->timer_group,
                                          driver->timer_idx, 0);
#endif
  }

  return task_awoken;
}

static const char *TAG = "dmx";  // The log tagline for the file.

esp_err_t dmx_driver_install(dmx_port_t dmx_num, int intr_flags) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(!dmx_driver_is_installed(dmx_num), ESP_ERR_INVALID_STATE,
            "driver is already installed");

#ifdef CONFIG_DMX_ISR_IN_IRAM
  // Driver ISR is in IRAM so interrupt flags must include IRAM flag
  if (!(intr_flags & ESP_INTR_FLAG_IRAM)) {
    ESP_LOGI(TAG, "ESP_INTR_FLAG_IRAM flag not set, flag updated");
    intr_flags |= ESP_INTR_FLAG_IRAM;
  }
#endif

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *restrict driver;

  // Allocate the DMX driver dynamically
  driver = heap_caps_malloc(sizeof(dmx_driver_t), MALLOC_CAP_32BIT);
  if (driver == NULL) {
    ESP_LOGE(TAG, "DMX driver malloc error");
    return ESP_ERR_NO_MEM;
  }
  dmx_driver[dmx_num] = driver;

  // Buffer must be allocated in unaligned memory
  driver->data.buffer = heap_caps_malloc(DMX_PACKET_SIZE, MALLOC_CAP_8BIT);
  if (driver->data.buffer == NULL) {
    ESP_LOGE(TAG, "DMX driver buffer malloc error");
    dmx_driver_delete(dmx_num);
    return ESP_ERR_NO_MEM;
  }

  // Allocate semaphore
  driver->mux = xSemaphoreCreateRecursiveMutex();
  if (driver->mux == NULL) {
    ESP_LOGE(TAG, "DMX driver mutex malloc error");
    dmx_driver_delete(dmx_num);
    return ESP_ERR_NO_MEM;
  }

  // Initialize driver state
  driver->dmx_num = dmx_num;
  driver->task_waiting = NULL;
  driver->uart = UART_LL_GET_HW(dmx_num);

  // Initialize driver flags
  driver->is_in_break = false;
  driver->end_of_packet = true;
  driver->is_sending = false;
  driver->new_packet = false;
  driver->is_enabled = true;

  driver->rdm.uid = 0;
  driver->rdm.discovery_is_muted = false;
  driver->rdm.tn = 0;

  // Initialize the driver buffer
  bzero(driver->data.buffer, DMX_MAX_PACKET_SIZE);
  driver->data.sent_last = false;
  driver->data.type = RDM_PACKET_TYPE_NON_RDM;
  driver->data.timestamp = 0;
  driver->data.head = -1;  // Wait for DMX break before reading data
  driver->data.rx_size = DMX_MAX_PACKET_SIZE;

  // Initialize DMX transmit settings
  driver->break_len = RDM_BREAK_LEN_US;
  driver->mab_len = RDM_MAB_LEN_US;

  // Initialize sniffer in the disabled state
  driver->sniffer.queue = NULL;

  // Initialize the UART peripheral
  uart_dev_t *const restrict uart = driver->uart;
  taskENTER_CRITICAL(spinlock);
  periph_module_enable(uart_periph_signal[dmx_num].module);
  if (dmx_num != CONFIG_ESP_CONSOLE_UART_NUM) {
#if SOC_UART_REQUIRE_CORE_RESET
    // ESP32-C3 workaround to prevent UART outputting garbage data.
    uart_ll_set_reset_core(uart, true);
    periph_module_reset(uart_periph_signal[dmx_num].module);
    uart_ll_set_reset_core(uart, false);
#else
    periph_module_reset(uart_periph_signal[dmx_num].module);
#endif
  }
  taskEXIT_CRITICAL(spinlock);

  // Initialize and flush the UART
  dmx_uart_init(uart);
  dmx_uart_rxfifo_reset(uart);
  dmx_uart_txfifo_reset(uart);

  // Install UART interrupt
  dmx_uart_disable_interrupt(uart, DMX_ALL_INTR_MASK);
  dmx_uart_clear_interrupt(uart, DMX_ALL_INTR_MASK);
  dmx_uart_set_txfifo_empty(uart, DMX_UART_EMPTY_DEFAULT);
  dmx_uart_set_rxfifo_full(uart, DMX_UART_FULL_DEFAULT);
  esp_intr_alloc(uart_periph_signal[dmx_num].irq, intr_flags, &dmx_uart_isr,
                 driver, &driver->uart_isr_handle);

  // Initialize hardware timer
#if ESP_IDF_VERSION_MAJOR >= 5
  const gptimer_config_t timer_config = {
      .clk_src = GPTIMER_CLK_SRC_APB,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,  // 1MHz resolution timer
  };
  esp_err_t err = gptimer_new_timer(&timer_config, &driver->gptimer_handle);
  if (err) {
    ESP_LOGE(TAG, "DMX driver gptimer error");
    dmx_driver_delete(dmx_num);
    return err;
  }
  const gptimer_event_callbacks_t gptimer_cb = {.on_alarm = dmx_timer_isr};
  gptimer_register_event_callbacks(driver->gptimer_handle, &gptimer_cb, driver);
  gptimer_enable(driver->gptimer_handle);
#else
  driver->timer_group = dmx_num / 2;
  driver->timer_idx = dmx_num % 2;
  const timer_config_t timer_config = {
      .divider = 80,  // (80MHz / 80) == 1MHz resolution timer
      .counter_dir = TIMER_COUNT_UP,
      .counter_en = false,
      .alarm_en = true,
      .auto_reload = true,
  };
  timer_init(driver->timer_group, driver->timer_idx, &timer_config);
  timer_isr_callback_add(driver->timer_group, driver->timer_idx, dmx_timer_isr,
                         driver, intr_flags);
#endif

  // Enable UART read interrupt and set RTS low
  taskENTER_CRITICAL(spinlock);
  xTaskNotifyStateClear(xTaskGetCurrentTaskHandle());
  dmx_uart_enable_interrupt(uart, DMX_INTR_RX_ALL);
  dmx_uart_set_rts(uart, 1);
  taskEXIT_CRITICAL(spinlock);

  // Give the mutex and return
  xSemaphoreGiveRecursive(driver->mux);
  return ESP_OK;
}

esp_err_t dmx_driver_delete(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), ESP_ERR_INVALID_STATE,
            "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Free driver mutex
  if (!xSemaphoreTakeRecursive(driver->mux, 0)) {
    return ESP_FAIL;
  }
  xSemaphoreGiveRecursive(driver->mux);
  vSemaphoreDelete(driver->mux);

  // Uninstall UART ISR
  if (driver->uart_isr_handle != NULL) {
    esp_intr_free(driver->uart_isr_handle);
  }

  // Uninstall sniffer ISR
  if (dmx_sniffer_is_enabled(dmx_num)) {
    dmx_sniffer_disable(dmx_num);
  }

  // Free driver data buffer
  if (driver->data.buffer != NULL) {
    heap_caps_free(driver->data.buffer);
  }

  // Free hardware timer ISR
#if ESP_IDF_VERSION_MAJOR >= 5
  gptimer_disable(driver->gptimer_handle);
  gptimer_del_timer(driver->gptimer_handle);
#else
  timer_isr_callback_remove(driver->timer_group, driver->timer_idx);
  timer_deinit(driver->timer_group, driver->timer_idx);
#endif

  // Free driver
  heap_caps_free(driver);
  dmx_driver[dmx_num] = NULL;

  // Disable UART module
  taskENTER_CRITICAL(spinlock);
  if (dmx_num != CONFIG_ESP_CONSOLE_UART_NUM) {
    periph_module_disable(uart_periph_signal[dmx_num].module);
  }
  taskEXIT_CRITICAL(spinlock);

  return ESP_OK;
}

bool dmx_driver_is_installed(dmx_port_t dmx_num) {
  return dmx_num < DMX_NUM_MAX && dmx_driver[dmx_num] != NULL;
}
bool dmx_driver_disable(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, false, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), false, "driver is not installed");
  DMX_CHECK(dmx_driver_is_enabled(dmx_num), false,
            "driver is already disabled");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  bool success = false;

  // Disable receive interrupts
  taskENTER_CRITICAL(spinlock);
  if (!driver->is_sending) {
    dmx_uart_disable_interrupt(driver->uart, DMX_INTR_RX_ALL);
    dmx_uart_clear_interrupt(driver->uart, DMX_INTR_RX_ALL);
    driver->is_enabled = false;
    success = true;
  }
  taskEXIT_CRITICAL(spinlock);

  return success;
}

bool dmx_driver_enable(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, false, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), false, "driver is not installed");
  DMX_CHECK(!dmx_driver_is_enabled(dmx_num), false,
            "driver is already enabled");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Initialize driver flags and reenable interrupts
  taskENTER_CRITICAL(spinlock);
  driver->is_in_break = false;
  driver->end_of_packet = true;
  driver->new_packet = false;
  driver->data.head = -1;  // Wait for DMX break before reading data
  dmx_uart_rxfifo_reset(driver->uart);
  dmx_uart_txfifo_reset(driver->uart);
  dmx_uart_enable_interrupt(driver->uart, DMX_INTR_RX_ALL);
  dmx_uart_clear_interrupt(driver->uart, DMX_INTR_RX_ALL);
  driver->is_enabled = true;
  taskEXIT_CRITICAL(spinlock);

  return true;
}

bool dmx_driver_is_enabled(dmx_port_t dmx_num) {
  bool is_enabled = false;

  if(dmx_driver_is_installed(dmx_num)) {
    spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
    taskENTER_CRITICAL(spinlock);
    is_enabled = dmx_driver[dmx_num]->is_enabled;
    taskEXIT_CRITICAL(spinlock);
  }

  return is_enabled;
}

esp_err_t dmx_set_pin(dmx_port_t dmx_num, int tx_pin, int rx_pin, int rts_pin) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(tx_pin < 0 || GPIO_IS_VALID_OUTPUT_GPIO(tx_pin),
            ESP_ERR_INVALID_ARG, "tx_pin error");
  DMX_CHECK(rx_pin < 0 || GPIO_IS_VALID_GPIO(rx_pin), ESP_ERR_INVALID_ARG,
            "rx_pin error");
  DMX_CHECK(rts_pin < 0 || GPIO_IS_VALID_OUTPUT_GPIO(rts_pin),
            ESP_ERR_INVALID_ARG, "rts_pin error");

  return uart_set_pin(dmx_num, tx_pin, rx_pin, rts_pin, DMX_PIN_NO_CHANGE);
}

esp_err_t dmx_sniffer_enable(dmx_port_t dmx_num, int intr_pin) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(intr_pin > 0 && GPIO_IS_VALID_GPIO(intr_pin), ESP_ERR_INVALID_ARG,
            "intr_pin error");
  DMX_CHECK(!dmx_sniffer_is_enabled(dmx_num), ESP_ERR_INVALID_STATE,
            "sniffer is already enabled");

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Allocate the sniffer queue
  driver->sniffer.queue = xQueueCreate(1, sizeof(dmx_metadata_t));
  if (driver->sniffer.queue == NULL) {
    ESP_LOGE(TAG, "DMX sniffer queue malloc error");
    return ESP_ERR_NO_MEM;
  }

  // Add the GPIO interrupt handler
  esp_err_t err = gpio_isr_handler_add(intr_pin, dmx_gpio_isr, driver);
  if (err) {
    ESP_LOGE(TAG, "DMX sniffer ISR handler error");
    vQueueDelete(driver->sniffer.queue);
    driver->sniffer.queue = NULL;
    return ESP_FAIL;
  }
  driver->sniffer.intr_pin = intr_pin;

  // Set sniffer default values
  driver->sniffer.last_neg_edge_ts = -1;  // Negative edge hasn't been seen yet
  driver->sniffer.is_in_mab = false;

  // Enable the interrupt
  gpio_set_intr_type(intr_pin, GPIO_INTR_ANYEDGE);

  return ESP_OK;
}

esp_err_t dmx_sniffer_disable(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(dmx_sniffer_is_enabled(dmx_num), ESP_ERR_INVALID_STATE,
            "sniffer is not enabled");

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Disable the interrupt and remove the interrupt handler
  gpio_set_intr_type(driver->sniffer.intr_pin, GPIO_INTR_DISABLE);
  esp_err_t err = gpio_isr_handler_remove(driver->sniffer.intr_pin);
  if (err) {
    ESP_LOGE(TAG, "DMX sniffer ISR handler error");
    return ESP_FAIL;
  }

  // Deallocate the sniffer queue
  vQueueDelete(driver->sniffer.queue);
  driver->sniffer.queue = NULL;

  return ESP_OK;
}

bool dmx_sniffer_is_enabled(dmx_port_t dmx_num) {
  return dmx_driver_is_installed(dmx_num) &&
         dmx_driver[dmx_num]->sniffer.queue != NULL;
}

bool dmx_sniffer_get_data(dmx_port_t dmx_num, dmx_metadata_t *metadata,
                          TickType_t wait_ticks) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, ESP_ERR_INVALID_ARG,
            "dmx_num error");
  DMX_CHECK(metadata, ESP_ERR_INVALID_ARG, "metadata is null");
  DMX_CHECK(dmx_sniffer_is_enabled(dmx_num), ESP_ERR_INVALID_STATE,
            "sniffer is not enabled");

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  return xQueueReceive(driver->sniffer.queue, metadata, wait_ticks);
}

uint32_t dmx_set_baud_rate(dmx_port_t dmx_num, uint32_t baud_rate) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");

  // Clamp the baud rate to within DMX specification
  if (baud_rate < DMX_MIN_BAUD_RATE) {
    baud_rate = DMX_MIN_BAUD_RATE;
  } else if (baud_rate > DMX_MAX_BAUD_RATE) {
    baud_rate = DMX_MAX_BAUD_RATE;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  dmx_uart_set_baud_rate(dmx_driver[dmx_num]->uart, baud_rate);
  taskEXIT_CRITICAL(spinlock);

  return baud_rate;
}

uint32_t dmx_get_baud_rate(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  const uint32_t baud_rate = dmx_uart_get_baud_rate(dmx_driver[dmx_num]->uart);
  taskEXIT_CRITICAL(spinlock);

  return baud_rate;
}

uint32_t dmx_set_break_len(dmx_port_t dmx_num, uint32_t break_len) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp the break length to within DMX specification
  if (break_len < DMX_MIN_BREAK_LEN_US) {
    break_len = DMX_MIN_BREAK_LEN_US;
  } else if (break_len > DMX_MAX_BREAK_LEN_US) {
    break_len = DMX_MAX_BREAK_LEN_US;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  dmx_driver[dmx_num]->break_len = break_len;
  taskEXIT_CRITICAL(spinlock);

  return break_len;
}

uint32_t dmx_get_break_len(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  const uint32_t break_len = dmx_driver[dmx_num]->break_len;
  taskEXIT_CRITICAL(spinlock);

  return break_len;
}

uint32_t dmx_set_mab_len(dmx_port_t dmx_num, uint32_t mab_len) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp the mark-after-break length to within DMX specification
  if (mab_len < DMX_MIN_MAB_LEN_US) {
    mab_len = DMX_MIN_MAB_LEN_US;
  } else if (mab_len > DMX_MAX_MAB_LEN_US) {
    mab_len = DMX_MAX_MAB_LEN_US;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  dmx_driver[dmx_num]->mab_len = mab_len;
  taskEXIT_CRITICAL(spinlock);

  return mab_len;
}

uint32_t dmx_get_mab_len(dmx_port_t dmx_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  taskENTER_CRITICAL(spinlock);
  const uint32_t mab_len = dmx_driver[dmx_num]->mab_len;
  taskEXIT_CRITICAL(spinlock);

  return mab_len;
}

size_t dmx_read(dmx_port_t dmx_num, void *destination, size_t size) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(destination, 0, "destination is null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp size to the maximum DMX packet size
  if (size > DMX_MAX_PACKET_SIZE) {
    size = DMX_MAX_PACKET_SIZE;
  } else if (size == 0) {
    return 0;
  }

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Copy data from the driver buffer to the destination asynchronously
  memcpy(destination, driver->data.buffer, size);

  return size;
}

size_t dmx_read_offset(dmx_port_t dmx_num, size_t offset, void *destination,
                       size_t size) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(offset < DMX_MAX_PACKET_SIZE, 0, "offset error");
  DMX_CHECK(destination, 0, "destination is null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp size to the maximum DMX packet size
  if (size + offset > DMX_MAX_PACKET_SIZE) {
    size = DMX_MAX_PACKET_SIZE - offset;
  } else if (size == 0) {
    return 0;
  }

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Copy data from the driver buffer to the destination asynchronously
  memcpy(destination, driver->data.buffer + offset, size);

  return size;
}

int dmx_read_slot(dmx_port_t dmx_num, size_t slot_num) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, -1, "dmx_num error");
  DMX_CHECK(slot_num < DMX_MAX_PACKET_SIZE, -1, "slot_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), -1, "driver is not installed");

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Return data from the driver buffer asynchronously
  return driver->data.buffer[slot_num];
}

size_t dmx_write(dmx_port_t dmx_num, const void *source, size_t size) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(source, 0, "source is null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp size to the maximum DMX packet size or fail quickly on invalid size
  if (size > DMX_MAX_PACKET_SIZE) {
    size = DMX_MAX_PACKET_SIZE;
  } else if (size == 0) {
    return 0;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];
  uart_dev_t *const restrict uart = driver->uart;

  taskENTER_CRITICAL(spinlock);
  if (driver->is_sending && driver->data.type != RDM_PACKET_TYPE_NON_RDM) {
    // Do not allow asynchronous writes when sending an RDM packet
    taskEXIT_CRITICAL(spinlock);
    return 0;
  } else if (dmx_uart_get_rts(uart) == 1) {
    // Flip the bus to stop writes from being overwritten by incoming data
    dmx_uart_set_rts(uart, 0);
  }
  driver->data.tx_size = size;  // Update driver transmit size
  taskEXIT_CRITICAL(spinlock);

  // Copy data from the source to the driver buffer asynchronously
  memcpy(driver->data.buffer, source, size);

  return size;
}

size_t dmx_write_offset(dmx_port_t dmx_num, size_t offset, const void *source,
                        size_t size) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(offset < DMX_MAX_PACKET_SIZE, 0, "offset error");
  DMX_CHECK(source, 0, "source is null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  // Clamp size to the maximum DMX packet size
  if (size + offset > DMX_MAX_PACKET_SIZE) {
    size = DMX_MAX_PACKET_SIZE - offset;
  } else if (size == 0) {
    return 0;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];
  uart_dev_t *const restrict uart = driver->uart;

  taskENTER_CRITICAL(spinlock);
  if (driver->is_sending && driver->data.type != RDM_PACKET_TYPE_NON_RDM) {
    // Do not allow asynchronous writes when sending an RDM packet
    taskEXIT_CRITICAL(spinlock);
    return 0;
  } else if (dmx_uart_get_rts(uart) == 1) {
    // Flip the bus to stop writes from being overwritten by incoming data
    dmx_uart_set_rts(uart, 0);
  }
  driver->data.tx_size = offset + size;  // Update driver transmit size
  taskEXIT_CRITICAL(spinlock);

  // Copy data from the source to the driver buffer asynchronously
  memcpy(driver->data.buffer + offset, source, size);

  return size;
}

int dmx_write_slot(dmx_port_t dmx_num, size_t slot_num, uint8_t value) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, -1, "dmx_num error");
  DMX_CHECK(slot_num < DMX_MAX_PACKET_SIZE, -1, "slot_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), -1, "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];
  uart_dev_t *const restrict uart = driver->uart;

  taskENTER_CRITICAL(spinlock);
  if (driver->is_sending && driver->data.type != RDM_PACKET_TYPE_NON_RDM) {
    // Do not allow asynchronous writes when sending an RDM packet
    taskEXIT_CRITICAL(spinlock);
    return 0;
  } else if (dmx_uart_get_rts(uart) == 1) {
    // Flip the bus to stop writes from being overwritten by incoming data
    dmx_uart_set_rts(uart, 0);
  }

  // Ensure that the next packet to be sent includes this slot
  if (driver->data.tx_size < slot_num) {
    driver->data.tx_size = slot_num;
  }
  taskEXIT_CRITICAL(spinlock);

  // Set the driver buffer slot to the assigned value asynchronously
  driver->data.buffer[slot_num] = value;

  return value;
}

size_t dmx_receive(dmx_port_t dmx_num, dmx_packet_t *packet,
                   TickType_t wait_ticks) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");
  DMX_CHECK(dmx_driver_is_enabled(dmx_num), 0, "driver is not enabled");

  dmx_driver_t *const restrict driver = dmx_driver[dmx_num];

  // Set default return value and default values for output argument
  uint32_t packet_size = 0;
  if (packet != NULL) {
    packet->err = ESP_ERR_TIMEOUT;
    packet->sc = -1;
    packet->size = 0;
    packet->is_rdm = false;
  }

  // Block until mutex is taken and driver is idle, or until a timeout
  TimeOut_t timeout;
  vTaskSetTimeOutState(&timeout);
  if (!xSemaphoreTakeRecursive(driver->mux, wait_ticks) ||
      (wait_ticks && xTaskCheckForTimeOut(&timeout, &wait_ticks))) {
    return packet_size;
  } else if (!dmx_wait_sent(dmx_num, wait_ticks) ||
             (wait_ticks && xTaskCheckForTimeOut(&timeout, &wait_ticks))) {
    xSemaphoreGiveRecursive(driver->mux);
    return packet_size;
  }

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  uart_dev_t *const restrict uart = driver->uart;

  // Set the RTS pin to enable reading from the DMX bus
  if (dmx_uart_get_rts(uart) == 0) {
    taskENTER_CRITICAL(spinlock);
    xTaskNotifyStateClear(xTaskGetCurrentTaskHandle());
    driver->data.head = -1;  // Wait for DMX break before reading data
    driver->new_packet = false;
    dmx_uart_set_rts(uart, 1);
    taskEXIT_CRITICAL(spinlock);
  }

  // Wait for new DMX packet to be received
  taskENTER_CRITICAL(spinlock);
  const bool new_packet_available = driver->new_packet;
  taskEXIT_CRITICAL(spinlock);
  if (!new_packet_available && wait_ticks > 0) {
    // Set task waiting and get additional DMX driver flags
    taskENTER_CRITICAL(spinlock);
    driver->task_waiting = xTaskGetCurrentTaskHandle();
    const bool sent_last = driver->data.sent_last;
    const enum rdm_packet_type_t data_type = driver->data.type;
    taskEXIT_CRITICAL(spinlock);

    // Check for early timeout according to RDM specification
    if (sent_last && (data_type & RDM_PACKET_TYPE_EARLY_TIMEOUT)) {
      taskENTER_CRITICAL(spinlock);
      const int64_t last_timestamp = driver->data.timestamp;
      taskEXIT_CRITICAL(spinlock);

      // Guard against setting hardware alarm durations with negative values
      int64_t elapsed = esp_timer_get_time() - last_timestamp;
      if (elapsed >= RDM_CONTROLLER_RESPONSE_LOST_TIMEOUT) {
        xSemaphoreGiveRecursive(driver->mux);
        return packet_size;
      }

      // Set an early timeout with the hardware timer
      taskENTER_CRITICAL(spinlock);
#if ESP_IDF_VERSION_MAJOR >= 5
      const gptimer_handle_t gptimer_handle = driver->gptimer_handle;
      const gptimer_alarm_config_t alarm_config = {
          .alarm_count = RDM_CONTROLLER_RESPONSE_LOST_TIMEOUT,
          .flags.auto_reload_on_alarm = false};
      gptimer_set_raw_count(gptimer_handle, elapsed);
      gptimer_set_alarm_action(gptimer_handle, &alarm_config);
      gptimer_start(gptimer_handle);
#else
      const timer_group_t timer_group = driver->timer_group;
      const timer_idx_t timer_idx = driver->timer_idx;
      timer_set_counter_value(timer_group, timer_idx, elapsed);
      timer_set_alarm_value(timer_group, timer_idx,
                            RDM_CONTROLLER_RESPONSE_LOST_TIMEOUT);
      timer_start(timer_group, timer_idx);
#endif
      taskEXIT_CRITICAL(spinlock);
    }

    // Wait for a task notification
    const bool notified = xTaskNotifyWait(0, -1, &packet_size, wait_ticks);
    taskENTER_CRITICAL(spinlock);
    driver->task_waiting = NULL;
    taskEXIT_CRITICAL(spinlock);
    if (!notified) {
#if ESP_IDF_VERSION_MAJOR >= 5
      gptimer_stop(driver->gptimer_handle);
#else
      timer_pause(driver->timer_group, driver->timer_idx);
#endif
      xTaskNotifyStateClear(xTaskGetCurrentTaskHandle());
      xSemaphoreGiveRecursive(driver->mux);
      return packet_size;
    }

  } else if (!new_packet_available) {
    // Fail early if there is no data available and this function cannot block
    xSemaphoreGiveRecursive(driver->mux);
    return packet_size;
  }

  // Parse DMX data packet
  if (packet != NULL) {
    taskENTER_CRITICAL(spinlock);
    packet->err = driver->data.err;
    packet_size = driver->data.head;
    if (packet_size > 0) {
      const rdm_data_t *const restrict rdm = (rdm_data_t *)driver->data.buffer;
      packet->is_rdm = (rdm->sc == RDM_SC && rdm->sub_sc == RDM_SUB_SC) ||
                       rdm->sc == RDM_PREAMBLE || rdm->sc == RDM_DELIMITER;
      packet->sc = driver->data.buffer[0];
    }
    driver->new_packet = false;
    taskEXIT_CRITICAL(spinlock);
    if (packet_size == -1) {
      packet_size = 0;
    }
    packet->size = packet_size;
  }

  // Give the mutex back and return
  xSemaphoreGiveRecursive(driver->mux);
  return packet_size;
}

size_t dmx_send(dmx_port_t dmx_num, size_t size) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");
  DMX_CHECK(dmx_driver_is_enabled(dmx_num), 0, "driver is not enabled");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Block until the mutex can be taken
  if (!xSemaphoreTakeRecursive(driver->mux, portMAX_DELAY)) {
    return 0;
  }

  // Block until the driver is done sending
  if (!dmx_wait_sent(dmx_num, portMAX_DELAY)) {
    xSemaphoreGiveRecursive(driver->mux);
    return 0;
  }

  // Determine if it is too late to send a response packet
  int64_t elapsed = 0;
  taskENTER_CRITICAL(spinlock);
  rdm_data_t *const rdm = (rdm_data_t *)driver->data.buffer;
  if (rdm->sc == RDM_SC && rdm->sub_sc == RDM_SUB_SC &&
      (rdm->cc == RDM_CC_DISC_COMMAND_RESPONSE ||
       rdm->cc == RDM_CC_GET_COMMAND_RESPONSE ||
       rdm->cc == RDM_CC_SET_COMMAND_RESPONSE)) {
    elapsed = esp_timer_get_time() - driver->data.timestamp;
  }
  taskEXIT_CRITICAL(spinlock);
  if (elapsed >= RDM_RESPONDER_RESPONSE_LOST_TIMEOUT) {
    xSemaphoreGiveRecursive(driver->mux);
    return 0;
  }

  // Determine if an alarm needs to be set to wait until driver is ready
  uint32_t timeout = 0;
  taskENTER_CRITICAL(spinlock);
  if (driver->data.sent_last) {
    if (driver->data.type == RDM_PACKET_TYPE_DISCOVERY) {
      timeout = RDM_DISCOVERY_NO_RESPONSE_PACKET_SPACING;
    } else if (driver->data.type == RDM_PACKET_TYPE_BROADCAST) {
      timeout = RDM_BROADCAST_PACKET_SPACING;
    } else if (driver->data.type == RDM_PACKET_TYPE_REQUEST) {
      timeout = RDM_REQUEST_NO_RESPONSE_PACKET_SPACING;
    }
  } else if (driver->data.type != RDM_PACKET_TYPE_NON_RDM) {
    timeout = RDM_RESPOND_TO_REQUEST_PACKET_SPACING;
  }
  elapsed = esp_timer_get_time() - driver->data.timestamp;
  if (elapsed < timeout) {
#if ESP_IDF_VERSION_MAJOR >= 5
    gptimer_set_raw_count(driver->gptimer_handle, elapsed);
    const gptimer_alarm_config_t alarm_config = {
        .alarm_count = timeout,
        .flags.auto_reload_on_alarm = false};
    gptimer_set_alarm_action(driver->gptimer_handle, &alarm_config);
    gptimer_start(driver->gptimer_handle);
#else
    timer_set_counter_value(driver->timer_group, driver->timer_idx, elapsed);
    timer_set_alarm_value(driver->timer_group, driver->timer_idx, timeout);
    timer_start(driver->timer_group, driver->timer_idx);
#endif
    driver->task_waiting = xTaskGetCurrentTaskHandle();
  }
  taskEXIT_CRITICAL(spinlock);

  // Block if an alarm was set
  if (elapsed < timeout) {
    bool notified = xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY);
    if (!notified) {
#if ESP_IDF_VERSION_MAJOR >= 5
      gptimer_stop(driver->gptimer_handle);
#else
      timer_pause(driver->timer_group, driver->timer_idx);
#endif
      xTaskNotifyStateClear(driver->task_waiting);
    }
    driver->task_waiting = NULL;
    if (!notified) {
      xSemaphoreGiveRecursive(driver->mux);
      return 0;
    }
  }

  // Turn the DMX bus around and get the send size
  uart_dev_t *const restrict uart = driver->uart;
  taskENTER_CRITICAL(spinlock);
  if (dmx_uart_get_rts(uart) == 1) {
    xTaskNotifyStateClear(xTaskGetCurrentTaskHandle());
    dmx_uart_set_rts(uart, 0);
  }
  taskEXIT_CRITICAL(spinlock);

  // Update the transmit size if desired
  if (size > 0) {
    if (size > DMX_MAX_PACKET_SIZE) {
      size = DMX_MAX_PACKET_SIZE;
    }
    taskENTER_CRITICAL(spinlock);
    driver->data.tx_size = size;
    taskEXIT_CRITICAL(spinlock);
  } else {
    taskENTER_CRITICAL(spinlock);
    size = driver->data.tx_size;
    taskEXIT_CRITICAL(spinlock);
  }

  // Record the outgoing packet type
  int packet_type = RDM_PACKET_TYPE_NON_RDM;
  if (rdm->sc == RDM_SC && rdm->sub_sc == RDM_SUB_SC) {
    if (rdm->cc == RDM_CC_DISC_COMMAND &&
        rdm->pid == bswap16(RDM_PID_DISC_UNIQUE_BRANCH)) {
      packet_type = RDM_PACKET_TYPE_DISCOVERY;
      ++driver->rdm.tn;
    } else if (rdm_uid_is_broadcast(buf_to_uid(rdm->destination_uid))) {
      packet_type = RDM_PACKET_TYPE_BROADCAST;
      ++driver->rdm.tn;
    } else if (rdm->cc == RDM_CC_GET_COMMAND || rdm->cc == RDM_CC_SET_COMMAND ||
               rdm->cc == RDM_CC_DISC_COMMAND) {
      packet_type = RDM_PACKET_TYPE_REQUEST;
      ++driver->rdm.tn;
    } else {
      packet_type = RDM_PACKET_TYPE_RESPONSE;
    }
  } else if (rdm->sc == RDM_PREAMBLE || rdm->sc == RDM_DELIMITER) {
    packet_type = RDM_PACKET_TYPE_DISCOVERY_RESPONSE;
  }
  driver->data.type = packet_type;
  driver->data.sent_last = true;

  // Determine if a DMX break is required and send the packet
  if (packet_type == RDM_PACKET_TYPE_DISCOVERY_RESPONSE) {
    // RDM discovery responses do not send a DMX break - write immediately
    taskENTER_CRITICAL(spinlock);
    driver->is_sending = true;

    size_t write_size = driver->data.tx_size;
    dmx_uart_write_txfifo(uart, driver->data.buffer, &write_size);
    driver->data.head = write_size;

    // Enable DMX write interrupts
    dmx_uart_enable_interrupt(uart, DMX_INTR_TX_ALL);
    taskEXIT_CRITICAL(spinlock);
  } else {
    // Send the packet by starting the DMX break
    taskENTER_CRITICAL(spinlock);
    driver->data.head = 0;
    driver->is_in_break = true;
    driver->is_sending = true;
#if ESP_IDF_VERSION_MAJOR >= 5
    gptimer_set_raw_count(driver->gptimer_handle, 0);
    const gptimer_alarm_config_t alarm_config = {
        .alarm_count = driver->break_len,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true};
    gptimer_set_alarm_action(driver->gptimer_handle, &alarm_config);
    gptimer_start(driver->gptimer_handle);
#else
    timer_set_counter_value(driver->timer_group, driver->timer_idx, 0);
    timer_set_alarm_value(driver->timer_group, driver->timer_idx,
                          driver->break_len);
    timer_start(driver->timer_group, driver->timer_idx);
#endif

    dmx_uart_invert_tx(uart, 1);
    taskEXIT_CRITICAL(spinlock);
  }

  // Give the mutex back
  xSemaphoreGiveRecursive(driver->mux);
  return size;
}

bool dmx_wait_sent(dmx_port_t dmx_num, TickType_t wait_ticks) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, false, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), false, "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Block until the mutex can be taken
  TimeOut_t timeout;
  vTaskSetTimeOutState(&timeout);
  if (!xSemaphoreTakeRecursive(driver->mux, wait_ticks) ||
      (wait_ticks && xTaskCheckForTimeOut(&timeout, &wait_ticks))) {
    return false;
  }

  // Determine if the task needs to block
  bool result = true;
  if (wait_ticks > 0) {
    bool task_waiting = false;
    taskENTER_CRITICAL(spinlock);
    if (driver->is_sending) {
      driver->task_waiting = xTaskGetCurrentTaskHandle();
      task_waiting = true;
    }
    taskEXIT_CRITICAL(spinlock);

    // Wait for a notification that the driver is done sending
    if (task_waiting) {
      result = xTaskNotifyWait(0, ULONG_MAX, NULL, wait_ticks);
      driver->task_waiting = NULL;
    }
  } else {
    taskENTER_CRITICAL(spinlock);
    if (driver->is_sending) {
      result = false;
    }
    taskEXIT_CRITICAL(spinlock);
  }

  // Give the mutex back and return
  xSemaphoreGiveRecursive(driver->mux);
  return result;
}
