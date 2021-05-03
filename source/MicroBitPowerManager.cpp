/*
The MIT License (MIT)

Copyright (c) 2017 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "MicroBitPowerManager.h"
#include "MicroBit.h"

static const uint8_t UIPM_I2C_NOP[3] = {0,0,0};

static const KeyValueTableEntry uipmPropertyLengthData[] = {
    {MICROBIT_UIPM_PROPERTY_BOARD_REVISION, 2},
    {MICROBIT_UIPM_PROPERTY_I2C_VERSION,2},
    {MICROBIT_UIPM_PROPERTY_DAPLINK_VERSION, 2},
    {MICROBIT_UIPM_PROPERTY_POWER_SOURCE, 1},
    {MICROBIT_UIPM_PROPERTY_POWER_CONSUMPTION, 4},
    {MICROBIT_UIPM_PROPERTY_USB_STATE, 1},
    {MICROBIT_UIPM_PROPERTY_KL27_POWER_MODE, 1},
    {MICROBIT_UIPM_PROPERTY_KL27_POWER_LED_STATE, 1}
};
CREATE_KEY_VALUE_TABLE(uipmPropertyLengths, uipmPropertyLengthData);

/**
 * Constructor.
 * Create a software abstraction of a power manager.
 *
 * @param i2c the I2C bus to use to communicate with the micro:bit USB interface chip
 * @param ioPins the IO pins in use on this device.
 * @param id the unique EventModel id of this component. Defaults to: MICROBIT_ID_POWER_MANAGER
 * @param systemTimer the system timer.
 *
 */
MicroBitPowerManager::MicroBitPowerManager(MicroBitI2C &i2c, MicroBitIO &ioPins, uint16_t id) : i2cBus(i2c), io(ioPins), sysTimer(NULL)
{
    this->id = id;

    // Indicate we'd like to receive periodic callbacks both in idle and interrupt context.
    status |= DEVICE_COMPONENT_STATUS_IDLE_TICK;
}

MicroBitPowerManager::MicroBitPowerManager(MicroBitI2C &i2c, MicroBitIO &ioPins, NRFLowLevelTimer &systemTimer, uint16_t id) : i2cBus(i2c), io(ioPins), sysTimer(&systemTimer)
{
    this->id = id;

    // Indicate we'd like to receive periodic callbacks both in idle and interrupt context.
    status |= DEVICE_COMPONENT_STATUS_IDLE_TICK;
}

/**
 * Attempts to determine the power source currently in use on this micro:bit.
 * note: This will query the USB interface chip via I2C, and wait for completion.
 * 
 * @return the current power source used by this micro:bit
 */
MicroBitPowerSource MicroBitPowerManager::getPowerSource()
{
    ManagedBuffer b;
    b = readProperty(MICROBIT_UIPM_PROPERTY_POWER_SOURCE);

    powerSource = (MicroBitPowerSource)b[3];

    return powerSource;
}

/**
 * Attempts to determine the USB interface chip version in use on this micro:bit.
 * note: This will query the USB interface chip via I2C if necessary.
 * 
 * @return MicroBitUSBVersion information.
 */
MicroBitVersion MicroBitPowerManager::getVersion()
{
    if (!(status & MICROBIT_USB_INTERFACE_VERSION_LOADED))
    {
        ManagedBuffer b;
        
        // Read Board Revision ID
        b = readProperty(MICROBIT_UIPM_PROPERTY_BOARD_REVISION);
        memcpy(&version.board, &b[3], 2);

        // Read I2C protocol version 
        b = readProperty(MICROBIT_UIPM_PROPERTY_I2C_VERSION);
        memcpy(&version.i2c, &b[3], 2);

        // Read DAPLink version
        b = readProperty(MICROBIT_UIPM_PROPERTY_DAPLINK_VERSION);
        memcpy(&version.daplink, &b[3], 2);

        // Version data in non-volatile, so cache it for later.
        //status |= MICROBIT_USB_INTERFACE_VERSION_LOADED;
    }

    return version;
}

/**
 * Attempts to determine the status of the USB interface on this micro:bit.
 * note: This will query the USB interface chip via I2C, and wait for completion.
 * 
 * @return the current status of the USB interface on this micro:bit
 */
MicroBitUSBStatus MicroBitPowerManager::getUSBStatus()
{
    ManagedBuffer b;
    b = readProperty(MICROBIT_UIPM_PROPERTY_USB_STATE);

    usbStatus = (MicroBitUSBStatus)b[3];
    return usbStatus;
}

/**
 * Attempts to determine the instantaneous power consumption of this micro:bit.
 * note: This will query the USB interface chip via I2C, and wait for completion.
 * 
 * @return the current power consumption of this micro:bit
 */
uint32_t MicroBitPowerManager::getPowerConsumption()
{
    ManagedBuffer b;
    b = readProperty(MICROBIT_UIPM_PROPERTY_POWER_CONSUMPTION);

    memcpy(&powerConsumption, &b[3], 4);
    return powerConsumption;
}

/**
 * Perform a NULL opertion I2C transcation wit the interface chip.
 * This is used to awken the KL27 interface chip from light sleep, 
 * as a work around for silicon errata in the KL27.
 */
void MicroBitPowerManager::nop()
{
    i2cBus.write(MICROBIT_UIPM_I2C_ADDRESS, (uint8_t *)UIPM_I2C_NOP, 3, false);
    target_wait(10);
}

/**
 * Attempts to issue a control packet to the USB interface chip.
 * @param packet The data to send
 * @return MICROBIT_OK on success, or an I2C related error code on failure.
 */
int MicroBitPowerManager::sendUIPMPacket(ManagedBuffer packet)
{
    nop();
    return i2cBus.write(MICROBIT_UIPM_I2C_ADDRESS, &packet[0], packet.length(), false);
}

/**
 * Attempts to read a packet from the USB interface chip, either as a response to a
 * prior request, or following an interrupt request.
 * 
 * @return A buffer containing the complete response.
 */
ManagedBuffer MicroBitPowerManager::recvUIPMPacket()
{

    if(io.irq1.isActive())
    {
        ManagedBuffer b(MICROBIT_UIPM_MAX_BUFFER_SIZE);

        nop();  
        if (i2cBus.read(MICROBIT_UIPM_I2C_ADDRESS, &b[0], MICROBIT_UIPM_MAX_BUFFER_SIZE, false) == MICROBIT_OK)
            return b;
    }

    return ManagedBuffer();
}

/**
 * Awaits a response to a previous requests to the USB interface chip.
 * Up to MICROBIT_UIPM_MAX_RETRIES attempts will be made at ~1ms intervals.
 * 
 * @return A buffer containing the complete response.
 */
ManagedBuffer MicroBitPowerManager::awaitUIPMPacket()
{
    ManagedBuffer response;
    int attempts = 0;

    awaitingPacket(true);

    // Wait for a response, signalled (possibly!) by a LOW on the combined irq line
    // Retry until we get a valid response or we time out.
    while(attempts++ < MICROBIT_UIPM_MAX_RETRIES)
    {
        target_wait(1);

        // Try to read a response from the KL27
        response = recvUIPMPacket();
        
        // If we receive an INCOMPLETE response, then the KL27 is still working on our request, so wait a little longer and try again.
        // Similarly, if the I2C transaction fails, retry.
        if (response.length() == 0 || (response[0] == MICROBIT_UIPM_COMMAND_ERROR_RESPONSE && response[1] == MICROBIT_UIPM_INCOMPLETE_CMD))
            continue;

        // Sanitize the length of the packet to meet specification and return it.
        response.truncate((response[0] == MICROBIT_UIPM_COMMAND_ERROR_RESPONSE || response[0] == MICROBIT_UIPM_COMMAND_WRITE_RESPONSE) ? 2 : 3 + uipmPropertyLengths.get(response[1]));
        awaitingPacket(false);

        return response;
    }

    // If we time out, return a generic write error to the caller
    ManagedBuffer error(2);
    error[0] = MICROBIT_UIPM_COMMAND_ERROR_RESPONSE;
    error[1] = MICROBIT_UIPM_WRITE_FAIL;
    awaitingPacket(false);

    return error;    
}


/**
 * Attempts to issue a control packet to the USB interface chip.
 * @param packet The data to send
 * @return MICROBIT_OK on success 
 */
ManagedBuffer MicroBitPowerManager::writeProperty(ManagedBuffer request, bool ack)
{
    ManagedBuffer response;

    if (sendUIPMPacket(request) == MICROBIT_OK && ack)
    {
        response = awaitUIPMPacket();
    }

    return response;
}

/**
 * Attempts to issue a control packet to the USB interface chip.
 * @param packet The data to send
 * @return A response or error packet 
 */
ManagedBuffer MicroBitPowerManager::readProperty(int property)
{
    ManagedBuffer request(2);
    ManagedBuffer response;

    request[0] = MICROBIT_UIPM_COMMAND_READ_REQUEST;
    request[1] = property;

    if (sendUIPMPacket(request) == MICROBIT_OK)
        response = awaitUIPMPacket();
    
    return response;
}
        
/**
 * Powers down the CPU and USB interface and enters OFF state. All user code and peripherals will cease operation. 
 * Device can subsequently be awoken only via a RESET. User program state will be lost and will restart
 * from main().
 */
void MicroBitPowerManager::off()
{
    setSleepMode(true);

    // Instruct the KL27 interface chip to go into deep sleep.
    ManagedBuffer sleepCommand(4);
    sleepCommand[0] = MICROBIT_UIPM_COMMAND_WRITE_REQUEST;
    sleepCommand[1] = MICROBIT_UIPM_PROPERTY_KL27_POWER_MODE;
    sleepCommand[2] = 1;
    sleepCommand[3] = MICROBIT_USB_INTERFACE_POWER_MODE_VLLS0;
    writeProperty(sleepCommand, true);

    // Wait a little while to ensure all hardware and peripherals have reacted to the change of power mode.
    target_wait(10);

    // Configure combined IRQ line to DETECT HI->LO transitions and reset the NRF52.
    // When sleeping, this allows us to reset on USB insertion, as raised by the KL27 interface chip.
    io.irq1.setDetect(GPIO_PIN_CNF_SENSE_Low);

    // Enter System Off state.
    NRF_POWER->SYSTEMOFF = 1;
}

/**
 * A periodic callback invoked by the fiber scheduler idle thread.
 * Service any IRQ requests raised by the USB interface chip.
 */
void MicroBitPowerManager::idleCallback()
{
    static int activeCount = 0;

    // Do nothing if there is a transaction in progress.
    if (status & MICROBIT_USB_INTERFACE_AWAITING_RESPONSE || !io.irq1.isActive())
    {
        activeCount = 0;
        return;
    }

    // Ensure the line has been held low for a little before services, as other sensors
    // are much more likley to be the source of the IRQ.
    if (activeCount++ < MICROBIT_USB_INTERFACE_IRQ_THRESHOLD)
        return;

    // The line has been held active beyond our threshold.
    // Reset our counter, and see if the USB interface chip has any data ready for us.
    activeCount  = 0;

    // Determine if the KL27 is trying to indicate an event
    ManagedBuffer response;
    response = recvUIPMPacket();

    if (response.length() > 0)
    {    
        // We have a valid frame.
        if(response[0] == MICROBIT_UIPM_COMMAND_READ_RESPONSE && response[1] == MICROBIT_UIPM_PROPERTY_KL27_USER_EVENT && response[2] == 1)
        {
            // The frame is for us - process the event.
            switch (response[3])
            {
                case MICROBIT_UIPM_EVENT_WAKE_RESET:
                    DMESG("WAKE FROM RESET BUTTON");
                    break;

                case MICROBIT_UIPM_EVENT_WAKE_USB_INSERTION:
                    DMESG("WAKE FROM USB");
                    break;

                case MICROBIT_UIPM_EVENT_RESET_LONG_PRESS:
                    DMESG("LONG RESET BUTTON PRESS");
                    off();
                    break;

                default:
                    DMESG("UNKNOWN KL27 EVENT CODE [%d]", response[2]);
            }
        }
        else
        {
            // The frame is not for us - forward the event to a Flash Manager if it has been registered
            DMESG("UIPM: RECEIVED UNKNWON FRAME");
        }
        
    }
}

/**
  * Clear configured wake-up sources
  * Note: this doesn't clear Timer events
  */
void MicroBitPowerManager::clearWakeUpSources()
{
    CodalComponent::manageAllWakeUp( wakeUpClear, NULL);
}

/**
 * Powers down the CPU and USB interface and instructs peripherals to enter an inoperative low power state. However, all
 * program state is preserved. CPU will deepsleep until the next codal::Timer event or other wake up source event, before returning to normal
 * operation.
 * 
 * note: ALL peripherals will be shutdown in this period. If you wish to keep peripherals active,
 * simply use uBit.sleep();
 * @return DEVICE_OK if deep sleep occurred, or DEVICE_INVALID_STATE if no usable wake up source is available 
 */
void MicroBitPowerManager::deepSleep()
{
    CODAL_TIMESTAMP eventTime = 0;
    bool wakeOnTime = system_timer_deepsleep_wakeup_time( &eventTime);
    deepSleep( wakeOnTime, eventTime, true /*wakeUpSources*/, NULL /*wakeUpPin*/);
}

/**
 * Powers down the CPU and USB interface and instructs peripherals to enter an inoperative low power state. However, all
 * program state is preserved. CPU will deepsleep for the given period of time, before returning to normal
 * operation.
 * 
 * note: ALL peripherals will be shutdown in this period. If you wish to keep peripherals active,
 * simply use uBit.sleep();
 */
void MicroBitPowerManager::deepSleep(uint32_t milliSeconds)
{
    CODAL_TIMESTAMP wakeUpTime = system_timer_current_time_us() + milliSeconds * 1000;
    deepSleep( true /*wakeOnTime*/, wakeUpTime, false /*wakeUpSources*/, NULL /*wakeUpPin*/);
}

/**
 * Powers down the CPU nd USB interface and instructs peripherals to enter an inoperative low power state. However, all
 * program state is preserved. CPU will deepsleep until the given pin becomes active, then return to normal
 * operation.
 * 
 * note: ALL peripherals will be shutdown in this period. If you wish to keep peripherals active,
 * simply use uBit.sleep();
 */
void MicroBitPowerManager::deepSleep(MicroBitPin &pin)
{
    deepSleep( false /*wakeOnTime*/, 0 /*wakeUpTime*/, false /*wakeUpSources*/, &pin /*wakeUpPin*/);
}

////////////////////////////////////////////////////////////////
// deepSleep implementation

#define deepSleepDEBUG 1  //TODO remove debug code

/**
 * Prepare configured wake-up sources before entering deep sleep or after return from sleep.
 * 
 * @param enable Set to true to prepare for sleep, false to prepare to reawaken.
 */
void MicroBitPowerManager::enableWakeUpSources(bool enable)
{
    CodalComponent::manageAllWakeUp( enable ? wakeUpEnable : wakeUpDisable, NULL);

    if ( enable)
    {
        // Enable wakeup from the the KL27 interrupt line.
        io.irq1.setDetect(GPIO_PIN_CNF_SENSE_Low);
        if (!NVIC_GetEnableIRQ(GPIOTE_IRQn))
            NVIC_EnableIRQ(GPIOTE_IRQn);
    }
    else
    {
        // Disable DETECT events 
        io.irq1.setDetect(GPIO_PIN_CNF_SENSE_Disabled);
    }
}

volatile uint16_t MicroBitPowerManager::timer_irq_channels;

void MicroBitPowerManager::deepSleepTimerIRQ(uint16_t chan)
{
    timer_irq_channels = chan;
}

/**
 * Powers down the CPU and USB interface and instructs peripherals to enter an inoperative low power state. However, all
 * program state is preserved. CPU will deepsleep until the next codal::Timer event or other wake up source event, before returning to normal
 * operation.
 * 
 * note: ALL peripherals will be shutdown in this period. If you wish to keep peripherals active,
 * simply use uBit.sleep();
 *
 * @param wakeOnTime    Set to true to wake up at time wakeUpTime
 * @param wakeUpTime    Time to wake up.
 * @param wakeUpSources Set to true to use external wake up sources configured by e.g. pin->setAwakeOnActive(true)
 * @param wakeUpPin     Pin to wake up. Ignored if wakeUpSources == true.
 */
void MicroBitPowerManager::deepSleep( bool wakeOnTime, CODAL_TIMESTAMP wakeUpTime, bool wakeUpSources, NRF52Pin *wakeUpPin)
{
    if ( sysTimer == NULL)
      return;

    CODAL_TIMESTAMP timeEntry = system_timer_current_time_us();

    if ( wakeOnTime)
    {
        if ( wakeUpTime - timeEntry < MICROBIT_POWER_MANAGER_MINIMUM_DEEP_SLEEP * 1000)
        {
            DMESG( "deepSleep: time too short");
            return;
        }
    }
    else
    {
        if ( wakeUpSources)
        {
            wakeUpResult result;
            CodalComponent::manageAllWakeUp( wakeUpCount, &result);
            if ( result.count == 0)
            {
                DMESG( "deepSleep: no wake up sources");
                return;
            }
        }
        else
        {
            if ( wakeUpPin == NULL)
            {
                DMESG( "deepSleep: no wake up pin");
                return;
            }
        }
    }

    // Configure for sleep mode
    setSleepMode(true);

    CODAL_TIMESTAMP tickStart;
    CODAL_TIMESTAMP timeStart = system_timer_deepsleep_begin( &tickStart);

    int      channel      = 2;      //System timer uses period = 0, event = 1 and capture = 3
    uint32_t saveCompare  = sysTimer->timer->CC[channel];
    uint32_t saveIntenset = sysTimer->timer->INTENSET;

    sysTimer->timer->INTENCLR = sysTimer->timer->INTENSET;

    void (*sysTimerIRQ) (uint16_t channel_bitmsk) = sysTimer->timer_pointer;
    sysTimer->setIRQ( deepSleepTimerIRQ);
    timer_irq_channels = 0;

#ifdef deepSleepDEBUG
    ManagedString dbg;
    dbg = dbg + "IRQ  ";

    for ( int irq = Reset_IRQn; irq <= SPIM3_IRQn; irq++)
    {
      if ( NVIC_GetEnableIRQ( (IRQn_Type) irq))
      {
        dbg = dbg + ManagedString( (int) irq);
        dbg = dbg + " ";
      }
    }
    dbg = dbg + "\n";
    DMESG( dbg.toCharArray());

    //These are enabled: RADIO_IRQn, UARTE0_UART0_IRQn
    //With BLE, also: POWER_CLOCK_IRQn, RTC0_IRQn, RTC1_IRQn, SWI2_EGU2_IRQn, SWI5_EGU5_IRQn, MWU_IRQn
#endif

    uint32_t usPerTick   = 1;
    uint32_t ticksPerMS  = 1000;
    uint32_t ticksMax    = 0xFFFFFFFFul - ticksPerMS * 1000; // approx 71min
    //ticksMax = 2000000;

    if ( wakeUpSources)
    {
        enableWakeUpSources(true);
    }
    else
    {
        if ( wakeUpPin)
        {
            // Ensure the requested pin into digital input mode. 
            wakeUpPin->getDigitalValue();

            // Enable an interrupt on the requested pin or an interrupt from the KL27.
            wakeUpPin->setDetect(wakeUpPin->getPolarity() ? GPIO_PIN_CNF_SENSE_High : GPIO_PIN_CNF_SENSE_Low);
        }

        // Enable wakeup from the the KL27 interrupt line.
        io.irq1.setDetect(GPIO_PIN_CNF_SENSE_Low);
        NVIC_EnableIRQ(GPIOTE_IRQn);
    }

    sysTimer->setCompare( channel, tickStart);
    sysTimer->enableIRQ();

    uint32_t tick0 = tickStart;
    uint32_t tick1 = tick0;

    // assume setSleepMode(false) needs at least same time as setSleepMode(true)
    // it may need much longer in general
    uint64_t totalTicks = wakeUpTime - timeEntry
                        - (timeStart - timeEntry) * 2 / usPerTick
                        - 13;  // __WFI() latency

    uint64_t sleepTicks = 0;

    while ( true)
    {
        uint32_t remain;

        if ( wakeOnTime)
        {
            if ( sleepTicks >= totalTicks)
            {
                DMESG( "deepSleep: time up");
                break;
            }
            uint64_t remain64 = totalTicks - sleepTicks;
            remain = remain64 > ticksMax ? ticksMax : remain64;
        }
        else
        {
          remain = ticksMax;
        }

        sysTimer->setCompare( channel, tick0 + remain);

        // Wait for an interrupt to occur. This will either be the requested transition,
        // or an asynchronous event from the KL27 interface chip.
        __WFI();

        tick1 = sysTimer->captureCounter();

        uint32_t ticks = tick1 - tick0;
        tick0 = tick1;

        sleepTicks += ticks;

        if ( timer_irq_channels == 0)
        {
            DMESG( "deepSleep: non-time interrupt");
            break; // It must be another interrupt
        }

        timer_irq_channels = 0;
    }

    if ( wakeUpSources)
    {
        enableWakeUpSources(false);
    }
    else
    {
        NVIC_DisableIRQ(GPIOTE_IRQn);

        // Diasble DETECT events 
        io.irq1.setDetect(GPIO_PIN_CNF_SENSE_Disabled);

        if ( wakeUpPin)
            wakeUpPin->setDetect(GPIO_PIN_CNF_SENSE_Disabled);
    }

    // Restore timer state
    sysTimer->disableIRQ();
    sysTimer->timer->INTENCLR = sysTimer->timer->INTENSET;
    sysTimer->setIRQ(sysTimerIRQ);
    sysTimer->timer->CC[channel] = saveCompare;

    system_timer_deepsleep_end( tick1, sleepTicks * usPerTick);

    sysTimer->timer->INTENSET = saveIntenset;

    // Configure for running mode.
    setSleepMode(false);

    DMESG( "deepSleep ended after %u ms", (unsigned int) ((system_timer_current_time_us() - timeEntry) / 1000));
}

/**
 * Prepares the micro:bit to enter or leave deep sleep mode.
 * This includes updating the status of the power LED, peripheral drivers and SENSE events on the combined IRQ line.
 * 
 * @param doSleep Set to true to preapre for sleep, false to prepare to reawaken.
 */
void MicroBitPowerManager::setSleepMode(bool doSleep)
{
    // Instruct the KL27 interface chip to update its LED status the power LED
    ManagedBuffer sleepCommand(4);
    sleepCommand[0] = MICROBIT_UIPM_COMMAND_WRITE_REQUEST;
    sleepCommand[1] = MICROBIT_UIPM_PROPERTY_KL27_POWER_LED_STATE;
    sleepCommand[2] = 1;
    sleepCommand[3] = doSleep ? 0 : 1;
    writeProperty(sleepCommand);

    // Update peripheral drivers
    CodalComponent::setAllSleep(doSleep);
}

/**
 * Allows a subsystem to indicate that it is actively waiting for a I2C response from the KL27
 * (e.g. the USBFlashManager). If set, the PowerManager will defer polling of the KL27 control interface
 * if an interrupt is asserted.
 *
 * @param awaiting true if a subsystem is awaiting a packet from the KL27, false otherwise.
 */
void MicroBitPowerManager::awaitingPacket(bool awaiting)
{
    if (awaiting)
        status |= MICROBIT_USB_INTERFACE_AWAITING_RESPONSE;
    else
        status &= ~MICROBIT_USB_INTERFACE_AWAITING_RESPONSE;
}

/**
 * Destructor.
 */
MicroBitPowerManager::~MicroBitPowerManager()
{

}
