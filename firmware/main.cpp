/*
 * Blinky Controller
 *
* Copyright (c) 2014 Matt Mets
 *
 * based on Fadecandy Firmware, Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <math.h>
#include <algorithm>
#include <stdlib.h>
#include <stdio.h>
#include "fc_usb.h"
#include "arm_math.h"
#include "HardwareSerial.h"
#include "usb_serial.h"

#include "blinkytile.h"
#include "animation.h"
#include "jedecflash.h"
#include "nofatstorage.h"
#include "dmaLed.h"
#include "addressprogrammer.h"
#include "patterns.h"
#include "serialloop.h"
#include "buttons.h"

// USB data buffers
static fcBuffers buffers;
fcLinearLUT fcBuffers::lutCurrent;

// External flash chip
FlashSPI flash;

// Flash storage class, works on top of the base flash chip
NoFatStorage flashStorage;

// Animations class
Animations animations;

// Button inputs
Buttons userButtons;

// Reserved RAM area for signalling entry to bootloader
extern uint32_t boot_token;

// Token to signal that the animation loop should be restarted
volatile bool reloadAnimations;

static void dfu_reboot()
{
    // Reboot to the Fadecandy Bootloader
    boot_token = 0x74624346;

    // Short delay to allow the host to receive the response to DFU_DETACH.
    uint32_t deadline = millis() + 10;
    while (millis() < deadline) {
        watchdog_refresh();
    }

    // Detach from USB, and use the watchdog to time out a 10ms USB disconnect.
    __disable_irq();
    USB0_CONTROL = 0;
    while (1);
}

extern "C" int usb_fc_rx_handler()
{
    // USB packet interrupt handler. Invoked by the ISR dispatch code in usb_dev.c
    return buffers.handleUSB();
}


void setupWatchdog() {
    // Change the watchdog timeout because the SPI access is too slow.
    const uint32_t watchdog_timeout = F_BUS / 2;  // 500ms

    WDOG_UNLOCK = WDOG_UNLOCK_SEQ1;
    WDOG_UNLOCK = WDOG_UNLOCK_SEQ2;
    asm volatile ("nop");
    asm volatile ("nop");
    WDOG_STCTRLH = WDOG_STCTRLH_ALLOWUPDATE | WDOG_STCTRLH_WDOGEN |
        WDOG_STCTRLH_WAITEN | WDOG_STCTRLH_STOPEN | WDOG_STCTRLH_CLKSRC;
    WDOG_PRESC = 0;
    WDOG_TOVALH = (watchdog_timeout >> 16) & 0xFFFF;
    WDOG_TOVALL = (watchdog_timeout)       & 0xFFFF;
}

extern "C" int main()
{
    setupWatchdog();

    initBoard();

    userButtons.setup();

    enableOutputPower();

    serialReset(SERIAL_MODE_DATA);

    flash.begin(FlashClass::autoDetect);

    DmaLed.setOutputType(CDmaLed::WS2812);

    reloadAnimations = true;

    // Application main loop
    while (usb_dfu_state == DFU_appIDLE) {
        watchdog_refresh();
       
        // TODO: put this in an ISR? Make the buttons do pin change interrupts?
        userButtons.buttonTask();

        #define BRIGHTNESS_COUNT 5
        static int brightnessLevels[BRIGHTNESS_COUNT] = {5,20,60,120,255};
        static int brightnessStep = BRIGHTNESS_COUNT-1;
        //static int brightnessStep = 3;

        static bool streaming_mode;

        static int animation;          // Flash animation to show
        static int frame;              // current frame to display
        static uint32_t nextTime;           // Time to display next frame

        DmaLed.setBrightness(brightnessLevels[brightnessStep]);

        if(reloadAnimations) {
            reloadAnimations = false;
            flashStorage.begin(flash);
            animations.begin(flashStorage);

            streaming_mode = false;
            animation = 0;
            frame = 0;
            nextTime = 0;
        }

        if(!streaming_mode) {
            // If the flash wasn't initialized, show a default flashing pattern
            if(animations.getCount() == 0) {
                count_up_loop();
                DmaLed.show();
            }
            else {

                // Flash-based
                if(millis() > nextTime) {
                    animations.getAnimation(animation)->getFrame(frame, DmaLed.getPixels());
                    frame++;
                    if(frame >= animations.getAnimation(animation)->frameCount) {
                        frame = 0;
                    }
    
                    nextTime += animations.getAnimation(animation)->speed;
    
                    // If we've gotten too far ahead of ourselves, reset the counter
                    if(millis() > nextTime) {
                        nextTime = millis() + animations.getAnimation(animation)->speed;
                    }
    
                    DmaLed.show();
                }
            }
        }

        // Handle fadecandy status messages
        if(buffers.finalizeFrame()) {
	    streaming_mode = true;

            if(!DmaLed.drawWaiting()) {
                for(int i = 0; i <  LED_COUNT; i++) {
                    DmaLed.setPixel(i, *(buffers.fbNext->pixel(i)+2),
                                       *(buffers.fbNext->pixel(i)+1),
                                       *(buffers.fbNext->pixel(i)));
                }
                DmaLed.show();
            }
        }

        // Check for serial data
        if(usb_serial_available() > 0) {
            streaming_mode = true;
            while(usb_serial_available() > 0) {
                serialLoop();
                watchdog_refresh();
            }
/*
            for(int i = 0; i <  LED_COUNT; i++) {
                DmaLed.setPixel(i, 0,0,0);
            }
            DmaLed.show();
*/
        }

        if(userButtons.isPressed()) {
            uint8_t button = userButtons.getPressed();
    
            if(button == BUTTON_A) {
                animation = (animation + 1)%animations.getCount();
                frame = 0;
            }
            else if(button == BUTTON_B) {
                
                if(DmaLed.getOutputType() == CDmaLed::DMX) {
                    DmaLed.setOutputType(CDmaLed::WS2812);
                }
                else if(DmaLed.getOutputType() == CDmaLed::WS2812) {
                    DmaLed.setOutputType(CDmaLed::LPD8806);
                }
                else if(DmaLed.getOutputType() == CDmaLed::LPD8806) {
                    DmaLed.setOutputType(CDmaLed::APA102);
                }
                else {
                    DmaLed.setOutputType(CDmaLed::DMX);
                }

//                brightnessStep = (brightnessStep + 1) % BRIGHTNESS_COUNT;
//                DmaLed.setBrightness(brightnessLevels[brightnessStep]);
            }
        }

    }

    // Reboot into DFU bootloader
    dfu_reboot();
}
