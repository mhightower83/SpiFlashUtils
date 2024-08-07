/*
 *   Copyright 2024 M Hightower
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
/*
  Test that pin function /WP can be disabled
  Returns true on a successful write

  Uses Status Register Protect bit to test writing.
  Currently uses P0 bit for test

  Test that pin function /HOLD can be disabled
  If the test passes, the call will return; otherwise, a HWDT reboot occurs.

  Notes:
    GPIO9 may work because the SPI Flash chip does not implement a /HOLD. eg. EN25Q32C
    GPIO10 may work because other Status Register Bits indicate not to use /WP.
    eg. SRP1:SRP0 = 0:0.  There are also parts that do no have pin function /WP.
*/

#include <Arduino.h>
#include <user_interface.h> // system_soft_wdt_feed()
#include "BootROM_NONOS.h"
#include <SpiFlashUtils.h>
#include "WP_HOLD_Test.h"
#define PRINTF(a, ...)        printf_P(PSTR(a), ##__VA_ARGS__)
#define PRINTF_LN(a, ...)     printf_P(PSTR(a "\n"), ##__VA_ARGS__)

////////////////////////////////////////////////////////////////////////////////
// GPIO pins 9 and 10 short circuit test
/*
  While testing every ESP8266 module I had, I found one module that Analyze
  failed to find a QE bit for. It passed WP and HOLD tests without any special
  bits set; however, it failed to work with Blinky, where the state of GPIO10 is
  read and written back to LED_BUILTIN. After I isolated the /WP and /HOLD pins
  of the flash chip, the sketch ran as expected.

  The flash memory has a short between /HOLD (GPIO9) and +3.3V. This module was
  a poor-quality ESP01 that would only work when flashed with SPI Flash Mode:
  "DOUT." While this was not a suitable module for GPIO9/10 usage, it
  illustrates potential issues that may arise with modules that have poorly
  chosen flash memory. Also, the datasheet I found for the flash memory did not
  match the behaviors I saw with the part.
*/

// We need pinMode functionality in IRAM.  GPF is a problem it resides in
// PROGMEM we have replaced with localGPF.
// Partial extraction of pinMode from core_esp8266_wiring_digital.cpp -
//   only intended to handle GPIO9 and GPIO10 with OUTPUT and SPECIAL.
#define localGPF(p) (*_gpioToFn[(p & 0xF)])

static void IRAM_ATTR pinSpecial(uint32_t pin, uint32_t mode) {
  volatile uint32_t* const _gpioToFn[16] = { &GPF0, &GPF1, &GPF2, &GPF3, &GPF4, &GPF5, &GPF6, &GPF7, &GPF8, &GPF9, &GPF10, &GPF11, &GPF12, &GPF13, &GPF14, &GPF15 };
  if (SPECIAL == mode) {
    GPC(pin) = (GPC(pin) & (0xF << GPCI)); //SOURCE(GPIO) | DRIVER(NORMAL) | INT_TYPE(UNCHANGED) | WAKEUP_ENABLE(DISABLED)
    GPEC = (1 << pin); //Disable
    localGPF(pin) = GPFFS(GPFFS_BUS(pin));//Set mode to BUS (RX0, TX0, TX1, SPI, HSPI or CLK depending in the p     40 in)
    if(pin == 3) {
      localGPF(pin) |= (1 << GPFPU);//enable pullup on RX
    }
  } else
  if (OUTPUT == mode) {
    localGPF(pin) = GPFFS(GPFFS_GPIO(pin));//Set mode to GPIO  -
    GPC(pin) = (GPC(pin) & (0xF << GPCI)); //SOURCE(GPIO) | DRIVER(NORMAL) | INT_TYPE(UNCHANGED) | WAKEUP_ENABLE(DISABLED)
    GPES = (1 << pin); //Enable
  } else {
    panic();
  }
}

////////////////////////////////////////////////////////////////////////////////
// Check for shorted GPIO pins - intended for use with GPIO 9 and 10
// Because changing the state of the /HOLD pin, may cause a HWDT resets. We need
// to guard against the chance that the iCache may have a miss. The complete
// call chain must be in IRAM. Using IRAM and disabled interrupts should be
// enough to guard against a flash read. It looks like we don't need the more
// extream guard of using Cache_Read_Disable_2 / Cache_Read_Enable_2.
bool IRAM_ATTR test_GPIO_pin_short(uint8_t pin) {
  // Cache_Read_Disable_2();
  uint32_t saved_ps = xt_rsil(15);
  Wait_SPI_Idle(flashchip);

  digitalWrite(pin, HIGH);
  pinSpecial(pin, OUTPUT);  // private version of pinMode(pin, OUTPUT);  // is in ICACHE
  bool pass1 = (HIGH == digitalRead(pin));

  digitalWrite(pin, LOW);
  bool pass2 = (LOW == digitalRead(pin));

  pinSpecial(pin, SPECIAL); // restore default function
  xt_wsr_ps(saved_ps);
  // Cache_Read_Enable_2();

  Serial.PRINTF_LN("%c GPIO%u digitalWrite %s test %s", (pass1) ? ' ' : '*', pin,
    "HIGH", (pass1) ? "passed" : "failed");
  if (! pass1) Serial.PRINTF_LN("* GPIO%u has a %s short", pin, "ground");

  Serial.PRINTF_LN("%c GPIO%u digitalWrite %s test %s", (pass2) ? ' ' : '*', pin,
    "LOW", (pass2) ? "passed" : "failed");
  if (! pass2) Serial.PRINTF_LN("* GPIO%u has a %s short", pin, "Vcc");

  return pass1 && pass2;
}


////////////////////////////////////////////////////////////////////////////////
//
// For SPI Flash devices that use QE/S9, they may also support bits SRP0 and
// SRP1. Setting SRP1:SRP0 = 0:1 enables the pin feature /WP.
// Ensures success by setting /WP pin high then restores to SPECIAL at exit.
//
// For QE/S9 case only, set SR1 and SR2 such that SRP0=1 (BIT7) and
// SRP1=0 (BIT8), and clear all other bits including QE.
//
// This may cause pin function /WP to be enabled on some devices for non-quad
// instructions regardless of QE=1 state.
//
// returns:
//   0 on succees
//   mask of bits stuck on (ignoring WEL and WIP)
//   all ones when BIT7 fails to set
//   sets pinMode SPECIAL at exit
//
uint32_t test_set_SRP1_SRP0_clear_QE([[maybe_unused]] const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile) {
  using namespace experimental;

  // Only call for QE/S9 case
  if (9u != qe_pos) panic();

  spi0_flash_write_disable(); // For some devices, EN25Q32C, this clears OTP mode.
  digitalWrite(10u, HIGH);     // ensure /WP is not asserted
  pinMode(10u, OUTPUT);

  uint32_t sr1 = 0u;
  spi0_flash_read_status_register_1(&sr1);

  if (BIT7 == (BIT7 & sr1)) {
    Serial.PRINTF_LN("  SRP0 already set.");
  }
  sr1 = BIT7;   // SRP1 must be zero to avoid permanently protected!
  if (use_16_bit_sr1) {
    spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16);
  } else {
    spi0_flash_write_status_register(/* SR2 */ 1u, 0u, _non_volatile, 8);
    spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    // Just in case the clearing order was wrong for allowing writes to SR2
    spi0_flash_write_status_register(/* SR2 */ 1u, 0u, _non_volatile, 8);
  }

  // Verify
  uint32_t sr21 = 0u, sr2 = 0u;
  spi0_flash_read_status_register_1(&sr21);
  sr21 &= 0xFCu;

  spi0_flash_read_status_register_2(&sr2);
  sr21 |= (sr2 << 8u);

  if (BIT7 == ((BIT8 | BIT7) & sr1)) {  // Expects SRP1:SRP0 = 0:1
    sr21 &= ~BIT7;  // Clear expected bits leaving only stuck bits.
  } else {
    sr21 = ~0u;
  }

  pinMode(10u, SPECIAL);
  return sr21;
}

////////////////////////////////////////////////////////////////////////////////
//
// For QE/S9 and QE/S6 cases, clears SR1 (and SR2)
// Ensures success by setting /WP pin high then restores to SPECIAL
//
// This is needed to completely disable pin function /WP on some devices for
// non-quad instructions. Contrary to some datasheets QE=1 was not always enough
//C to disable /WP. ie. Winbond, BergMicro, XMC. TODO: reconfirm statement.
//
// returns:
//   0 on succees
//   mask of bits stuck on (ignoring WEL and WIP)
//   sets pinMode SPECIAL at exit
//
uint32_t test_clear_SRP1_SRP0_QE(const bool has_8bw_sr2, const bool use_16_bit_sr1, const bool _non_volatile) {
  using namespace experimental;

  uint32_t
  spi0_flash_write_disable();
  digitalWrite(10u, HIGH);     // ensure /WP is not asserted
  pinMode(10u, OUTPUT);

  if (use_16_bit_sr1) {
    spi0_flash_write_status_register(/* SR1 */ 0u, 0u, _non_volatile, 16);
  } else {
    spi0_flash_write_status_register(/* SR1 */ 0u, 0u, _non_volatile, 8);
    if (has_8bw_sr2) {
      spi0_flash_write_status_register(/* SR2 */ 1u, 0u, _non_volatile, 8);
      spi0_flash_write_status_register(/* SR1 */ 0u, 0u, _non_volatile, 8);
    }
  }

  // Verify
  uint32_t sr21 = 0u, sr2 = 0u;
  spi0_flash_read_status_register_1(&sr21);
  sr21 &= 0xFCu;
  if (has_8bw_sr2) {
    spi0_flash_read_status_register_2(&sr2);
    sr21 |= (sr2 << 8u);
  }

  pinMode(10u, SPECIAL);
  return sr21;
}


////////////////////////////////////////////////////////////////////////////////
//
// use_preset == false, Sets the proposed QE bit as indicated by qe_pos and
// verifies that the bit is set
//
// use_preset == true, get value of current QE bit as defined with qe_pos {S9 or
// S6} and returns existing value of QE. Intended for using the current Status
// Register settings.
//
// returns:
//   value  of the QE bit (0 or 1) selected from SR by qe_pos (6u or 9u for S6 or S9}
//
//   -1     qe_pos was not S9 or S6.
//
//    0     On failure to set - for use_preset == false
//
static int test_set_QE(const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile, const bool use_preset) {
  using namespace experimental;

  spi0_flash_write_disable();
  bool qe = false;

  // check and report state of QE
  if (use_preset) {
    uint32_t sr1 = 0u, sr2 = 0u;
    if (9u == qe_pos) {
      spi0_flash_read_status_register_2(&sr2);
      qe = BIT1 == (BIT1 & sr2);
    } else
    if (6u == qe_pos) {
      spi0_flash_read_status_register_1(&sr1);
      qe = BIT6 == (BIT6 & sr1);
    } else {
      return -1;
    }
    Serial.PRINTF_LN("  QE/S%x=%u used", qe_pos, (qe) ? 1u : 0u);
    return (qe) ? 1 : 0;
  }

  // Set QE and report result true if successful
  if (9u == qe_pos) {
    uint32_t sr2 = 0u;
    spi0_flash_read_status_register_2(&sr2); // flash_gd25q32c_read_status(/* SR2 */ 1);
    qe = BIT1 == (BIT1 & sr2);
    if (qe) {
      Serial.PRINTF_LN("  QE/S%X already set.", qe_pos);
    } else {
      sr2 |= BIT1;    // S9
      if (use_16_bit_sr1) {
        uint32_t sr1 = 0u;
        spi0_flash_read_status_register_1(&sr1);
        sr1 |= sr2 << 8u;
        spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16u);
      } else {
        spi0_flash_write_status_register(/* SR2 */ 1u, sr2, _non_volatile, 8u);
      }
      // Verify
      sr2 = 0u;
      spi0_flash_read_status_register_2(&sr2); // flash_gd25q32c_read_status(/* SR2 */ 1);
      qe =  BIT1 == (BIT1 & sr2);
    }
  } else
  if (6u == qe_pos) {
    uint32_t sr1 = 0u;
    spi0_flash_read_status_register_1(&sr1);
    qe = BIT6 == (BIT6 & sr1);
    if (qe) {
      Serial.PRINTF_LN("  QE/S%X already set.", qe_pos);
    } else {
      sr1 |= BIT6;
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8u);
      // verify
      sr1 = 0u;
      spi0_flash_read_status_register_1(&sr1);
      qe = BIT6 == (BIT6 & sr1);
    }
  } else {
    return -1;
  }
  return (qe) ? 1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// Lets try and test /WP feature by setting and clearing a BP0 bit in SR1.
//
// For QE/S9 with SR1 and SR2, with 8 or 16-bit writes or
// For QE/S6 with SR1 only
//
// Only allow QE bit, SRP0, and PM0 to be set. All other are 0.
//
static bool testFlashWrite(const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile) {
  using namespace experimental;

  bool test = false;
  spi0_flash_write_disable();

  if (9u == qe_pos) {
    // bool _non_volatile = true;
    uint32_t sr1 = 0u, sr2 = 0u;
    spi0_flash_read_status_register_1(&sr1);
    spi0_flash_read_status_register_2(&sr2);
    sr1 &= BIT7;  // Keep SRP0 asis
    sr1 |= BIT2;  // set PM0
    sr2 &= BIT1;  // Keep QE/S9
    if (use_16_bit_sr1) {
      sr1 |= sr2 << 8u;
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16);
    } else {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    }
    uint32_t verify_sr1 = 0u;
    spi0_flash_read_status_register_1(&verify_sr1);
    test = BIT2 == (BIT2 & verify_sr1);
    sr1 &= ~BIT2;   // clear PM0
    if (use_16_bit_sr1) {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16);
    } else {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    }
  } else
  if (6 == qe_pos) {
    // No SRP0 or SRP1
    uint32_t sr1 = 0u;
    spi0_flash_read_status_register_1(&sr1);
    sr1 &= BIT6;  // QE/S6 or WPDis
    sr1 |= BIT2;  // set PM0
    spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    uint32_t verify_sr1 = 0u;
    spi0_flash_read_status_register_1(&verify_sr1);
    test = BIT2 == (BIT2 & verify_sr1);
    sr1 &= ~BIT2;
    spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
  } else
  if (0xFFu == qe_pos) {
    // No QE, SRP0 or SRP1
    uint32_t sr1 = 0u, sr2 = 0u;
    spi0_flash_read_status_register_1(&sr1);
    sr1 |= BIT2;  // set PM0
    if (use_16_bit_sr1) {
      spi0_flash_read_status_register_2(&sr2);
      sr1 |= sr2 << 8u;
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16);
    } else {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    }
    uint32_t verify_sr1 = 0u;
    spi0_flash_read_status_register_1(&verify_sr1);
    test = BIT2 == (BIT2 & verify_sr1);
    sr1 &= ~BIT2;   // clear PM0
    if (use_16_bit_sr1) {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 16);
    } else {
      spi0_flash_write_status_register(/* SR1 */ 0u, sr1, _non_volatile, 8);
    }
  }
  return test;
}


static int get_SRP10(const uint32_t qe_pos) {
  using namespace experimental;

  if (qe_pos != 9u) return 0;

  uint32_t status = 0u;
  if (SPI_RESULT_OK == spi0_flash_read_status_registers_2B(&status)) {
    return ((status >> 7u) & 3u); // return SRP1:SRP0
  }
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
//
// Verify that Flash pin function /WP (shared with GPIO10) can be disabled.
//
// There are three situations:
//  1. Flash allows write with /WP LOW when QE=1 and fail when QE=0 and SRP1:SRP0=0:1
//  2. Flash doesn't care about QE. They allow writes with /WP LOW when SRP1:SRP0=0:0 and block when SRP1:SRP0=0:1.
//  3. Combination of 1 and 2. Ignore /WP when either QE=1 or SRP1:SRP0=0:0
//  4. Flash doesn't ever monitor /WP.
//
bool testOutputGPIO10(const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile, const bool use_preset) {
  using namespace experimental;
  bool pass = false;

  Serial.PRINTF_LN("\nRun verification test for pin function /WP disable");
  if (9u != qe_pos && 6u != qe_pos && 0xFFu != qe_pos) {
    Serial.PRINTF_LN("* QE/S%X bit field specification undefined should be either S6 or S9", qe_pos);
    return pass;
  }
  digitalWrite(10u, HIGH);     // ensure /WP is not asserted, otherwise test_set_QE may fail
  pinMode(10u, OUTPUT);

  int _qe = test_set_QE(qe_pos, use_16_bit_sr1, _non_volatile, use_preset); // -1, 0, 1
  if (0xFFu != qe_pos && -1 == _qe) {
    Serial.PRINTF_LN("* Test Write: set QE/S%X bit - failed", qe_pos);
    return false;
  }

  int srp10_WP = get_SRP10(qe_pos); // already masked with 3

  if (9u == qe_pos) {
    Serial.PRINTF_LN("  Test Write: QE/S%X=%d SRP1:SRP0=%u:%u, and GPIO10 as OUTPUT",
      qe_pos, _qe, (srp10_WP >> 1u) & 1u, srp10_WP & 1u);
  } else
  if (6u == qe_pos) {
    Serial.PRINTF_LN("  Test Write: QE/S%X=%d, and GPIO10 as OUTPUT", qe_pos, _qe);
  } else {
    if (! use_preset) {
      Serial.PRINTF_LN("  Test Write: No QE bit, and GPIO10 as OUTPUT");
    }
  }
  Serial.PRINTF_LN("  Test Write: using %svolatile Status Register", (_non_volatile) ? "non-" : "");

  // With pin 10, HIGH, expect success regardless of QE and SRP1 and SRP0
  pass = testFlashWrite(qe_pos, use_16_bit_sr1, _non_volatile);
  Serial.PRINTF_LN("  Test Write: With /WP set %s write %s", "HIGH", (pass) ? "succeeded" : "failed.");

  // Expect success if QE=1 and/or SRP1:0=0:0 are relavent, failure otherwise
  digitalWrite(10u, LOW);
  pass = testFlashWrite(qe_pos, use_16_bit_sr1, _non_volatile);
  Serial.PRINTF_LN("  Test Write: With /WP set %s write %s", "LOW", (pass) ? "succeeded" : "failed.");

  pinMode(10u, SPECIAL);
  return pass;
}

////////////////////////////////////////////////////////////////////////////////
//  Verify that Flash pin function /HOLD (shared with GPIO9) can be disabled.
//
//  Missing from this test is testing for a crash when not attempting to disable
//  /HOLD, Instead we just verify that the /HOLD pin is not causing a crash
//  when held low. Some Flash do not have a /HOLD pin feature.
bool testOutputGPIO9(const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile, const bool use_preset) {
  using namespace experimental;

  Serial.PRINTF_LN("\nRun test to confirm pin function /HOLD is disabled");
  if (9u != qe_pos && 6u != qe_pos && 0xFFu != qe_pos) {
    Serial.PRINTF_LN("* QE/S%X bit field specification undefined should be either S6 or S9", qe_pos);
    return false;
  }

  int _qe = test_set_QE(qe_pos, use_16_bit_sr1, _non_volatile, use_preset);
  if (0 <= _qe) {
    Serial.PRINTF_LN("  Verify /HOLD is disabled by Status Register QE/S%X=%d", qe_pos, _qe);
  }
  Serial.PRINTF_LN("  Change GPIO9 to OUTPUT and set LOW. If module crashes, it failed.");
  pinMode(9u, OUTPUT);
  digitalWrite(9u, LOW);
  // No WDT Reset - then it passed.
  if (0 > _qe) {
    Serial.PRINTF_LN("  passed - current settings worked.");
  } else {
    if (_qe) {
      Serial.PRINTF_LN("  passed - bit QE/S%X=%d worked.", qe_pos, _qe); // No WDT Reset - then it passed.
    } else {
      Serial.PRINTF_LN("* Unexpected results. QE/S%X=0 and we did not crash. Flash may not support /HOLD.", qe_pos);
    }
  }

  pinMode(9u, SPECIAL);
  return true;
}

#if 0
////////////////////////////////////////////////////////////////////////////////
//
bool testInput_GPIO9_GPIO10(const uint32_t qe_pos, const bool use_16_bit_sr1, const bool _non_volatile, const bool use_preset) {
  using namespace experimental;

  Serial.PRINTF_LN("\nRun GPIO9 and GPIO10 INPUT test");
  Serial.PRINTF_LN("Test GPIO9 and GPIO10 by reading from the pins as INPUT and print result.");
  if (9u != qe_pos && 6u != qe_pos) {
    Serial.PRINTF_LN("* QE/S%X bit field specification undefined should be either S6 or S9", qe_pos);
    return false;
  }
  int _qe = test_set_QE(qe_pos, use_16_bit_sr1, _non_volatile, use_preset);
  Serial.PRINTF_LN("  Test: QE/S%X=%u, GPIO pins 9 and 10 as INPUT", qe_pos, _qe);
  pinMode(9u, INPUT);
  pinMode(10u, INPUT);
  uint32_t pin9 = digitalRead(9u);
  Serial.PRINTF_LN("  digitalRead result: GPIO_9(%u) and GPIO_10(%u)", pin9, digitalRead(10u));

  if (0u == pin9){
    if (0 == _qe) {
      Serial.PRINTF_LN("* Ambiguous results. QE/S%X=0 and we did not crash. Flash may not support /HOLD.");
    } else {
      Serial.PRINTF_LN("  Passed - no crash"); // No WDT Reset - then it passed.
    }
  }
  return true;
}
#endif
