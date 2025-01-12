/* This file is based on unreality's FujiHeatPump project */

#define DEBUG_FUJI
#include "FujiHeatPump.h"
#include "esphome/core/log.h"
#include "string.h"

namespace esphome {
namespace fujitsu {

static const char* TAG = "FujiHeatPump";

FujiFrame FujiHeatPump::decodeFrame() {
    FujiFrame ff;

    ff.messageSource = readBuf[0] & 0b01111111;
    if (readBuf[0] & 0b10000000) {
        // Seems like the high bit means it's a broadcast
        ff.messageDest = controllerAddress;
    } else {
        ff.messageDest = readBuf[1] & 0b01111111;
    }
    ff.messageType = (readBuf[2] & 0b00110000) >> 4;

    ff.acError = (readBuf[kErrorIndex] & kErrorMask) >> kErrorOffset;
    ff.temperature =
        (readBuf[kTemperatureIndex] & kTemperatureMask) >> kTemperatureOffset;
    ff.acMode = (readBuf[kModeIndex] & kModeMask) >> kModeOffset;
    ff.fanMode = (readBuf[kFanIndex] & kFanMask) >> kFanOffset;
    ff.economyMode = (readBuf[kEconomyIndex] & kEconomyMask) >> kEconomyOffset;
    ff.swingMode = (readBuf[kSwingIndex] & kSwingMask) >> kSwingOffset;
    ff.swingStep =
        (readBuf[kSwingStepIndex] & kSwingStepMask) >> kSwingStepOffset;
    ff.controllerPresent =
        (readBuf[kControllerPresentIndex] & kControllerPresentMask) >>
        kControllerPresentOffset;
    ff.updateMagic =
        (readBuf[kUpdateMagicIndex] & kUpdateMagicMask) >> kUpdateMagicOffset;
    ff.onOff = (readBuf[kEnabledIndex] & kEnabledMask) >> kEnabledOffset;
    ff.controllerTemp = (readBuf[kControllerTempIndex] & kControllerTempMask) >>
                        kControllerTempOffset;  // there are 2 leading bits here
                                                // that are unknown

    ff.writeBit = (readBuf[2] & 0b00001000) != 0;
    ff.loginBit = (readBuf[1] & 0b00100000) != 0;
    ff.unknownBit = (readBuf[1] & 0b10000000) > 0;

    return ff;
}

void FujiHeatPump::encodeFrame(FujiFrame ff, byte* writeBuf) {
    memset(writeBuf, 0, kFrameSize);

    writeBuf[0] = ff.messageSource;

    writeBuf[1] &= 0b10000000;
    writeBuf[1] |= ff.messageDest & 0b01111111;

    writeBuf[2] &= 0b11001111;
    writeBuf[2] |= ff.messageType << 4;

    if (ff.writeBit) {
        writeBuf[2] |= 0b00001000;
    } else {
        writeBuf[2] &= 0b11110111;
    }

    writeBuf[1] &= 0b01111111;
    if (ff.unknownBit) {
        writeBuf[1] |= 0b10000000;
    }

    if (ff.loginBit) {
        writeBuf[1] |= 0b00100000;
    } else {
        writeBuf[1] &= 0b11011111;
    }

    writeBuf[kModeIndex] =
        (writeBuf[kModeIndex] & ~kModeMask) | (ff.acMode << kModeOffset);
    writeBuf[kModeIndex] = (writeBuf[kEnabledIndex] & ~kEnabledMask) |
                           (ff.onOff << kEnabledOffset);
    writeBuf[kFanIndex] =
        (writeBuf[kFanIndex] & ~kFanMask) | (ff.fanMode << kFanOffset);
    writeBuf[kErrorIndex] =
        (writeBuf[kErrorIndex] & ~kErrorMask) | (ff.acError << kErrorOffset);
    writeBuf[kEconomyIndex] = (writeBuf[kEconomyIndex] & ~kEconomyMask) |
                              (ff.economyMode << kEconomyOffset);
    writeBuf[kTemperatureIndex] =
        (writeBuf[kTemperatureIndex] & ~kTemperatureMask) |
        (ff.temperature << kTemperatureOffset);
    writeBuf[kSwingIndex] =
        (writeBuf[kSwingIndex] & ~kSwingMask) | (ff.swingMode << kSwingOffset);
    writeBuf[kSwingStepIndex] = (writeBuf[kSwingStepIndex] & ~kSwingStepMask) |
                                (ff.swingStep << kSwingStepOffset);
    writeBuf[kControllerPresentIndex] =
        (writeBuf[kControllerPresentIndex] & ~kControllerPresentMask) |
        (ff.controllerPresent << kControllerPresentOffset);
    writeBuf[kUpdateMagicIndex] =
        (writeBuf[kUpdateMagicIndex] & ~kUpdateMagicMask) |
        (ff.updateMagic << kUpdateMagicOffset);
    writeBuf[kControllerTempIndex] =
        (writeBuf[kControllerTempIndex] & ~kControllerTempMask) |
        (ff.controllerTemp << kControllerTempOffset);
}

void heat_pump_uart_event_task(void *pvParameters) {
    FujiHeatPump *heatpump = (FujiHeatPump *)pvParameters;
    uart_event_t event;
    TickType_t wakeTime;
    byte send_buf[kFrameSize];
    int msgsSent = 0;
    while (true) {
        if(xQueueReceive(heatpump->uart_queue, (void * )&event, pdMS_TO_TICKS(1000))) {
            ESP_LOGI(TAG, "messages sent so far: %d", msgsSent);
            switch(event.type) {

                //Event of UART receving data
                /*We'd better handler data event fast, there would be much more data events than
                  other types of events. If we take too much time on data event, the queue might
                  be full.*/
                case UART_DATA:
                    size_t bufferLen;

                    wakeTime = xTaskGetTickCount();

                    uart_get_buffered_data_len(heatpump->uart_port, &bufferLen);
                    ESP_LOGI(TAG, "[BUFFER LENGTH]: %d", bufferLen);
                    bufferLen %= kFrameSize;
                    if (bufferLen) {
                        ESP_LOGD(TAG, "Discarding %d bytes", bufferLen);
                        uart_read_bytes(heatpump->uart_port, heatpump->readBuf, bufferLen, portMAX_DELAY);
                    }

                    ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                    for (auto i = 0; i < event.size / kFrameSize; i++) {
                        if (kFrameSize != uart_read_bytes(heatpump->uart_port, heatpump->readBuf, kFrameSize, portMAX_DELAY)) {
                            ESP_LOGW(TAG, "Failed to read state update as expected");
                        }
                        else {
                            heatpump->processReceivedFrame();
                            if (!xSemaphoreTake(heatpump->updateStateMutex, portMAX_DELAY)) {
                                ESP_LOGW(TAG, "Failed to take update state mutex");
                            }
                            if (heatpump->updateFields == 0) {
                                // We only should update HA if we don't have a pending update
                                xQueueOverwrite(heatpump->state_dropbox, &heatpump->currentState);
                            }
                            if (!xSemaphoreGive(heatpump->updateStateMutex)) {
                                ESP_LOGW(TAG, "Failed to give update state mutex");
                            }
                            if (xQueueReceive(heatpump->response_queue, send_buf, 0)) {
#if 0
                                ESP_LOGD(TAG, "Now, handling a pending frame txmit");
                                // This causes us to wait until 100 ms have passed since we read wakeTime, so that we account for the processReceivedFrame(). It also allows other tasks to use the core in the meantime because it suspends instead of busy-waiting.
                                vTaskDelayUntil(&wakeTime, pdMS_TO_TICKS(100));
                                if (uart_write_bytes(heatpump->uart_port, (const char*)send_buf, kFrameSize) != kFrameSize) {
                                    ESP_LOGW(TAG, "Failed to write state update as expected");
                                }
                                msgsSent++;
                                ESP_LOGD(TAG, "Completed txmit");
                                if (!xSemaphoreTake(heatpump->updateStateMutex, portMAX_DELAY)) {
                                    ESP_LOGW(TAG, "Failed to take update state mutex");
                                }
                                heatpump->updateFields = 0;
                                if (!xSemaphoreGive(heatpump->updateStateMutex)) {
                                    ESP_LOGW(TAG, "Failed to give update state mutex");
                                }
#endif
                            }
                        }
                    }
                    break;
                //Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "hw fifo overflow");
                    // If fifo overflow happened, you should consider adding flow control for your application.
                    // The ISR has already reset the rx FIFO,
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(heatpump->uart_port);
                    xQueueReset(heatpump->uart_queue);
                    break;
                //Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "ring buffer full");
                    // If buffer full happened, you should consider increasing your buffer size
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(heatpump->uart_port);
                    xQueueReset(heatpump->uart_queue);
                    break;
                //Event of UART RX break detected
                case UART_BREAK:
                    ESP_LOGI(TAG, "uart rx break");
                    break;
                //Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGI(TAG, "uart parity error");
                    break;
                //Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "uart frame error");
                    break;
                //Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d", event.type);
                    break;
            }
        }
        //ESP_LOGI(TAG, "uart task heartbeat");
    }
}

void FujiHeatPump::connect(uart_port_t uart_port, bool secondary, int rxPin, int txPin) {
    ESP_LOGD("FujitsuClimate", "Connect has been entered!");
    int rc;
    uart_config_t uart_config = {
        .baud_rate = 500,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    if (uart_is_driver_installed(uart_port)) {
        ESP_LOGW(TAG, "uninstalling uart driver...");
        rc = uart_driver_delete(uart_port);
        if (rc != 0) {
            ESP_LOGW(TAG, "Failed to uninstall existing uart driver");
            return;
        }
    }
    rc = uart_driver_install(uart_port, 2048, 2048, 20, &this->uart_queue, 0);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to install uart driver");
        return;
    }
    rc = uart_param_config(uart_port, &uart_config);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to configure uart params");
        return;
    }
    rc = uart_set_pin(uart_port, txPin /* TXD */,  rxPin /* RXD */, UART_PIN_NO_CHANGE /* RTS */, UART_PIN_NO_CHANGE /* CTS */);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set uart pins");
        return;
    }

    rc = uart_set_mode(uart_port, UART_MODE_RS485_HALF_DUPLEX);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set uart to half duplex");
        return;
    }
    ESP_LOGD(TAG, "Serial port configured");

    if (secondary) {
        controllerIsPrimary = false;
        controllerAddress = static_cast<byte>(FujiAddress::SECONDARY);
        ESP_LOGI(TAG, "Controller in secondary mode");
    } else {
        controllerIsPrimary = true;
        controllerAddress = static_cast<byte>(FujiAddress::PRIMARY);
        ESP_LOGI(TAG, "Controller in primary mode");
    }

    this->uart_port = uart_port;

    this->response_queue = xQueueCreate(10, sizeof(uint8_t[kFrameSize]));

    //rc = xTaskCreatePinnedToCore(heat_pump_uart_event_task, "FujiTask", 4096, (void *)this,
    //        // TODO is the priority reasonable? find & investigate the freertosconfig.h
    //                        configMAX_PRIORITIES - 1, NULL /* ignore the task handle */, 1);
    rc = xTaskCreate(heat_pump_uart_event_task, "FujiTask", 4096, (void *)this,
                            12, NULL /* ignore the task handle */);
    if (rc != pdPASS) {
        ESP_LOGW(TAG, "Failed to create heat pump event task");
        return;
    }
}

void FujiHeatPump::printFrame(byte buf[kFrameSize], FujiFrame ff) {
    ESP_LOGD(TAG, "%02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX",
        buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    ESP_LOGD(
        TAG,
        " mSrc: %d mDst: %d mType: %d write: %d login: %d unknown: %d onOff: "
        "%d temp: %d, mode: %d cP:%d uM:%d cTemp:%d acError:%d \n",
        ff.messageSource, ff.messageDest, ff.messageType, ff.writeBit,
        ff.loginBit, ff.unknownBit, ff.onOff, ff.temperature, ff.acMode,
        ff.controllerPresent, ff.updateMagic, ff.controllerTemp, ff.acError);
}


void FujiHeatPump::sendResponse(FujiFrame& ff) {
    if (!comms_is_enabled) {
        ESP_LOGD(TAG, "Comms is disabled, so not sending a response");
        return;
    }
    byte writeBuf[kFrameSize];
    encodeFrame(ff, writeBuf);

#ifdef DEBUG_FUJI
    ESP_LOGD(TAG, "--> ");
    printFrame(writeBuf, ff);
#endif

    for (int i = 0; i < kFrameSize; i++) {
        writeBuf[i] ^= 0xFF;
    }

    if (xQueueSend(this->response_queue, &writeBuf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Unable to send response into response_queue");
    }
}

void FujiHeatPump::processReceivedFrame() {
    FujiFrame ff;

    for (int i = 0; i < kFrameSize; i++) {
        readBuf[i] ^= 0xFF;
    }

    ff = decodeFrame();

#ifdef DEBUG_FUJI
    ESP_LOGD(TAG, "<-- ");
    printFrame(readBuf, ff);
#endif

    if (ff.messageDest == controllerAddress) {
        ESP_LOGD(TAG, "Matched addr");
        lastFrameReceived = xTaskGetTickCount();

        if (ff.messageType == static_cast<byte>(FujiMessageType::STATUS)) {
            ESP_LOGD(TAG, "status msg");
            if (ff.loginBit) {
                ESP_LOGD(TAG, "We are being asked to log in, primary=%d", controllerIsPrimary);
                if (controllerIsPrimary) {
                    // if this is the first message we have received,
                    // announce ourselves to the indoor unit
                    uint8_t oldUpdateMagic = ff.updateMagic;
                    memset(&ff, 0, sizeof(FujiFrame));
                    ff.messageSource = controllerAddress;
                    ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
                    ff.loginBit = true;
                    ff.messageType =
                        static_cast<byte>(FujiMessageType::LOGIN);
                    ff.updateMagic = oldUpdateMagic;
                    sendResponse(ff);
                    return;
                } else {
                    // secondary controller never seems to get any other
                    // message types, only status with controllerPresent ==
                    // 0 the secondary controller seems to send the same
                    // flags no matter which message type

                    ff.messageSource = controllerAddress;
                    ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
                    ff.loginBit = false;
                    ff.controllerPresent = 1;
                    ff.updateMagic = 2;
                    ff.unknownBit = true;
                    ff.writeBit = 0;
                }
            } else {
                // we have logged into the indoor unit
                // this is what most frames are
                ff.messageSource = controllerAddress;

                if (seenSecondaryController) {
                    ff.messageDest =
                        static_cast<byte>(FujiAddress::SECONDARY);
                    ff.loginBit = true;
                    ff.controllerPresent = 0;
                } else {
                    ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
                    ff.loginBit = false;
                    ff.controllerPresent = 1;
                }

                ff.updateMagic = 0;
                ff.unknownBit = true;
                ff.writeBit = 0;
                ff.messageType = static_cast<byte>(FujiMessageType::STATUS);
            }

#if 0
            if (ff.acError) {
                ESP_LOGD(TAG, "Got error, asking for details");
                memset(&ff, 0, sizeof(FujiFrame));
                ff.messageSource = controllerAddress;
                ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
                ff.updateMagic = 10;
                ff.messageType =
                    static_cast<byte>(FujiMessageType::ERROR);
                sendResponse(ff);
                return;
            }
#endif

            // if we have any updates, set the flags
            if (!xSemaphoreTake(updateStateMutex, portMAX_DELAY)) {
                ESP_LOGW(TAG, "Failed to take update state mutex");
            }

            if (updateFields) {
                ff.writeBit = 1;
                ESP_LOGD(TAG, "We have fields to update");
            }

            if (updateFields & kOnOffUpdateMask) {
                // updateStateMutex is held
                ff.onOff = updateState.onOff;
            }

            if (updateFields & kTempUpdateMask) {
                // updateStateMutex is held
                ff.temperature = updateState.temperature;
            }

            if (updateFields & kModeUpdateMask) {
                // updateStateMutex is held
                ff.acMode = updateState.acMode;
            }

            if (updateFields & kFanModeUpdateMask) {
                // updateStateMutex is held
                ff.fanMode = updateState.fanMode;
            }

            if (updateFields & kSwingModeUpdateMask) {
                // updateStateMutex is held
                ff.swingMode = updateState.swingMode;
            }

            if (updateFields & kSwingStepUpdateMask) {
                // updateStateMutex is held
                ff.swingStep = updateState.swingStep;
            }

            memcpy(&currentState, &ff, sizeof(FujiFrame));

            if (!xSemaphoreGive(updateStateMutex)) {
                ESP_LOGW(TAG, "Failed to give update state mutex");
            }

            if (ff.writeBit) {
                ff.updateMagic = 10;
                ESP_LOGD(TAG, "Sending field updates");
                sendResponse(ff);
                return;
            }
        } else if (ff.messageType ==
                   static_cast<byte>(FujiMessageType::LOGIN)) {
            ESP_LOGD(TAG, "recv a login msg, going to ack");
            // received a login frame OK frame
            ff.loginBit = true;
            ff.controllerPresent = 1;
            ff.updateMagic = 0;
            ff.unknownBit = true;
            ff.writeBit = 0;

            if (!xSemaphoreTake(updateStateMutex, portMAX_DELAY)) {
                ESP_LOGW(TAG, "Failed to take update state mutex");
            }

            ff.onOff = currentState.onOff;
            ff.temperature = currentState.temperature;
            ff.acMode = currentState.acMode;
            ff.fanMode = currentState.fanMode;
            ff.swingMode = currentState.swingMode;
            ff.swingStep = currentState.swingStep;
            ff.acError = currentState.acError;
            if (!xSemaphoreGive(updateStateMutex)) {
                ESP_LOGW(TAG, "Failed to give update state mutex");
            }

            // ack the login
            ff.messageDest = ff.messageSource;
            ff.messageSource = controllerAddress;
            ff.messageType = static_cast<byte>(FujiMessageType::STATUS);
            sendResponse(ff);

            if (controllerIsPrimary) {
                ESP_LOGD(TAG, "also pinging secondary on login");
                // the primary will send packet to a secondary controller to see
                // if it exists
                ff.messageSource = controllerAddress;
                ff.messageDest = static_cast<byte>(FujiAddress::SECONDARY);
                ff.messageType = static_cast<byte>(FujiMessageType::LOGIN);
                sendResponse(ff);
            }
            return;
        } else if (ff.messageType ==
                   static_cast<byte>(FujiMessageType::ERROR)) {
            ESP_LOGD(TAG, "AC ERROR RECV: ");
            printFrame(readBuf, ff);
            // handle errors here
        }
    } else if (ff.messageDest ==
               static_cast<byte>(FujiAddress::SECONDARY)) {
        seenSecondaryController = true;
        if (!xSemaphoreTake(updateStateMutex, portMAX_DELAY)) {
            ESP_LOGW(TAG, "Failed to take update state mutex");
        }
        currentState.controllerTemp =
            ff.controllerTemp;  // we dont have a temp sensor, use the temp
                                // reading from the secondary controller
        if (!xSemaphoreGive(updateStateMutex)) {
            ESP_LOGW(TAG, "Failed to give update state mutex");
        }
    }
}

bool FujiHeatPump::isBound() {
    if (xTaskGetTickCount() - lastFrameReceived < pdMS_TO_TICKS(1000))
    {
        return true;
    }
    return false;
}

bool FujiHeatPump::updatePending() {
    if (updateFields) {
        return true;
    }
    return false;
}

void FujiHeatPump::setOnOff(bool o) {
    updateFields |= kOnOffUpdateMask;
    updateState.onOff = o ? 1 : 0;
}
void FujiHeatPump::setTemp(byte t) {
    updateFields |= kTempUpdateMask;
    updateState.temperature = t;
}
void FujiHeatPump::setMode(byte m) {
    updateFields |= kModeUpdateMask;
    updateState.acMode = m;
}
void FujiHeatPump::setFanMode(byte fm) {
    updateFields |= kFanModeUpdateMask;
    updateState.fanMode = fm;
}
void FujiHeatPump::setEconomyMode(byte em) {
    updateFields |= kEconomyModeUpdateMask;
    updateState.economyMode = em;
}
void FujiHeatPump::setSwingMode(byte sm) {
    updateFields |= kSwingModeUpdateMask;
    updateState.swingMode = sm;
}
void FujiHeatPump::setSwingStep(byte ss) {
    updateFields |= kSwingStepUpdateMask;
    updateState.swingStep = ss;
}

bool FujiHeatPump::getOnOff() { return currentState.onOff == 1 ? true : false; }
byte FujiHeatPump::getTemp() { return currentState.temperature; }
byte FujiHeatPump::getMode() { return currentState.acMode; }
byte FujiHeatPump::getFanMode() { return currentState.fanMode; }
byte FujiHeatPump::getEconomyMode() { return currentState.economyMode; }
byte FujiHeatPump::getSwingMode() { return currentState.swingMode; }
byte FujiHeatPump::getSwingStep() { return currentState.swingStep; }
byte FujiHeatPump::getControllerTemp() { return currentState.controllerTemp; }


void FujiHeatPump::setState(FujiFrame *state) {
    if (!xSemaphoreTake(updateStateMutex, portMAX_DELAY)) {
        ESP_LOGW(TAG, "Failed to take update state mutex");
    }
    ESP_LOGD(TAG, "About to get the current state");
    //vTaskDelay(pdMS_TO_TICKS(1000));
    FujiFrame *current = &this->currentState;
    if (state->onOff != current->onOff) {
        ESP_LOGD(TAG, "About to change onoff");
        //vTaskDelay(pdMS_TO_TICKS(1000));
        this->setOnOff(state->onOff);
        ESP_LOGD(TAG, "changed onoff");
        //vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (state->temperature != current->temperature) {
        this->setTemp(state->temperature);
    }

    if (state->acMode != current->acMode) {
        this->setMode(state->acMode);
    }

    if (state->fanMode != current->fanMode) {
        this->setFanMode(state->fanMode);
    }

    if (state->economyMode != current->economyMode) {
        this->setEconomyMode(state->economyMode);
    }

    if (state->swingMode != current->swingMode) {
        this->setSwingMode(state->swingMode);
    }

    if (state->swingStep != current->swingStep) {
        this->setSwingStep(state->swingStep);
    }
    if (!xSemaphoreGive(updateStateMutex)) {
        ESP_LOGW(TAG, "Failed to give update state mutex");
    }
    ESP_LOGD(TAG, "Successfully set state");
}

byte FujiHeatPump::getUpdateFields() { return updateFields; }

}
}
