/*
  An outline example of using Reclaim GPIOs and performing specific
  initialization for multiple versions of a Flash part.

  Shows reclaiming GPIOs from preinit().
  See "OutlineXMC.ino.globals.h" for build options.

  Additionally shows using the example code generate by Analyze.ino for
  handling additional Flash devices.
*/
#include <ModeDIO_ReclaimGPIOs.h>

// Variable is used before C++ runtime init has started.
bool gpio_9_10_available __attribute__((section(".noinit")));

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n\nOutline Sketch using 'reclaim_GPIO_9_10()'");

}

void loop() {
  if (gpio_9_10_available) {
    /*

    */
  }

}

extern "C"
void preinit() {
  /*
    If using `-DDEBUG_FLASH_QE=1`, reclaim_GPIO_9_10() printing will be at
    115200 bps.
  */
  gpio_9_10_available = reclaim_GPIO_9_10();
  if (gpio_9_10_available) {
    /*
      Add additional urgent GPIO pin initialization here
    */
  }
}