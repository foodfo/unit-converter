/*
// TO DO LIST:: 
Fill out all functions with at minimum header comments
Ensure all flagged bugs are squashed

// STUFF FOR VERSION 2  

--Features--
fix small number handling. use scientific notation instead of printing 0 if number is small?
implement decimal to inch-fraction conversion  
Show battery voltage as percentage
maybe make version 2 a whole basic calculator??

--Bugs--
Every once in awhile, the unit will shut down and will not come back on. can't track down why but adding delay between attach interrupt and power down may help.  RST button fixes the error.
sleep and reset work better now, but are contingent upon resetting the keypress state before sleep
not sure if delay() in EEPROM function actually helps
UNITS_LIST length > 30 (only 50 tested) causes eeprom calls to fail. maybe causes dynamic memory usage to go too high?
/*/

#include <Keypad.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ss_oled.h>  // DONT FORGET TO DISABLE FONT_LARGE IF NOT IN USE. FONT_LARGE CONSUMES ~21% (6500 bytes) of PROGMEM. IN SS_OLED.CPP COMMENT OUT LINE 100, LINE 486, LINE 1560, LINE 1609 to enable FONT_16X32
#include <BitBang_I2C.h>
#include <LowPower.h>

//------------------------------------------------------//
//
//           USER CONFIGURABLE SETTINGS
//
//-----------------------------------------------------//

/// enter your own values in to DEFAULT_UNITS_LIST below ///
#define long_press_time 1000          // hold key for 1 seconds for long press to enter menus
#define debounce_time 5               // default from Keypad.cpp = 10ms
#define LIST_SIZE 30                   // Number of units that can be stored on device. Recommended values: 10-30. Max value theoretical value: 60 (end of EEPROM). Max tested value: 30 (>30 may cause stability issues due to insufficient SRAM)
#define voltage_correction_factor 100  // (real voltage / analog read voltage) * 100  -----> 93.6% = 94

/////////////////////// ENABLE/DISABLE SERIAL MONITOR ///////////////////////

#define DEBUG 0    // flip DEBUG to 1 to enable serial monitor. will have to shut down portions of code for space. Recommend commenting out functions in main switch case in loop()

#if DEBUG == 1
#define startSerial Serial.begin(9600)
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define debugF(x) Serial.print(x, 6)
#else
#define startSerial
#define debug(x)
#define debugln(x)
#define debugF(x)
#endif

/////////////////////// INITIALIZE KEYPAD ///////////////////////

#define rows 4
#define cols 5
// long_press_time is in User Configurable Settings

char keys[rows][cols] = {
  { 'A', '7', '4', '1', '0' },
  { 'B', '8', '5', '2', 'x' },
  { 'C', '9', '6', '3', '.' },
  { 'D', '/', 'x', 'E', 'x' },
};

uint8_t pin_r[rows] = { 11, 8, 6, 12 };  // pin assignment on board
uint8_t pin_c[cols] = { 7, 5, 9, 2, 10 }; // rows/cols are swapped w.r.t schematic since diode direction is incorrect for the keypad.h library

Keypad tenKey = Keypad(makeKeymap(keys), pin_r, pin_c, rows, cols);

/////////////////////// INITIALIZE DISPLAYS ///////////////////////

// Left Display
#define displaySize_L OLED_128x64
#define SDA_L 0xC0          // A0/D14 Pin
#define SCL_L 0xC1          // A1/D15 Pin
#define I2C_Address_L 0x3C  // oled display I2C address
#define flipDisplay_L 0     // dont flip the diplay
#define invertDisplay_L 0   // dont invert the display
#define USE_HW_I2C_L 0      // use bitbang'd software I2C

// Right Display
#define displaySize_R OLED_128x64
#define SDA_R 0xC0          // A0/D14 Pin
#define SCL_R 0xC1          // A1/D15 Pin
#define I2C_Address_R 0x3D  // oled display I2C address
#define flipDisplay_R 0     // dont flip the diplay
#define invertDisplay_R 0   // dont invert the display
#define USE_HW_I2C_R 0      // use bitbang'd software I2C

#define displayResetPin 0xC6  // -1 is no reset pin -- 0xC6 is pro mini RST pin

// initialize display structs
SSOLED dispL, dispR;

///////////////////////////// ENUMS & VARIABLES /////////////////////////////

enum DEVICE_STATES {
  BEGIN,
  MAIN,
  SETTINGS_MAIN,
  SETTINGS_MORE,
  SETTINGS_RST,
  UNITS_MAIN,
  UNITS_OVERWRITE,
  UNITS_UNIT1,
  UNITS_UNIT2,
  UNITS_CONVERT,
} STATE;

#define unitStringLength 5    // max characters = 4. add +1 for null terminator '/0'
#define numSelctUnits 4       // Number of Selectable Units :: A, B, C, D keys on keypad
// LIST_SIZE is in User Configurable Settings

char key;
bool factoryResetFlag = false;
bool forceSleepFlag = false;
bool pwrOnLowBattFlag = false;

// UNIT MATRIX
struct UNITS_STRUCT {
  char unit1[unitStringLength];
  char unit2[unitStringLength];
  uint8_t special;                // flag for special conversion math. use 0 for standard multiply/divide conversion, use 8 byte number for special conversion and write custom code into doMath()
  float conversion;
};

// SETTINGS PROTOTYPES
uint8_t currentUnit;
bool longFormat;
uint8_t nextCustomIndex;                  // index of next custom unit
bool conversionDirection[numSelctUnits];  // true is multiply, false is divide
int8_t activeUnitsIndex[numSelctUnits];  
uint8_t sleepSetting;
bool saveValueOnSleep;
UNITS_STRUCT activeUnits[numSelctUnits];

//------------------------------------------------------//
//
//           EEPROM & MEMORY FUNCTIONS
//
//-----------------------------------------------------//

#define writeHoldTime 4         // ms delay between eeprm write orders, not sure its actually needed

//  EEPROM INDEX LOCATIONS
#define EE_INIT 2               // INIT / RESET byte
#define EE_currentUnit 6        // 1
#define EE_longFormat 8         // 1
#define EE_sleepSetting 10      // 1
#define EE_nextCustIdx 12       // 1
#define EE_saveValue 14         // 1
#define EE_activeUnitsIdx 22    // 4
#define EE_conversionDir 27     // 4 
#define EE_UNITS_LIST 100       // each entry 15 bytes
//  more info @eof

void EEPROM_INIT() {

  if (EEPROM[EE_INIT] == 255) {

    clearEEPROM();

    uint8_t default_currentUnit = 0;
    bool default_longFormat = false;
    bool default_saveValue = true;
    uint8_t default_activeUnitsIndex[numSelctUnits] = { 0, 7, 9, 3 };  // CHOOSE FROM DEFAULT_UNITS_LIST
    bool default_conversionDirection[numSelctUnits] = { 1, 1, 1, 1 };  // true is multiply, false is divide
    uint8_t default_sleepSetting = 0;  // 10 seconds

    UNITS_STRUCT DEFAULT_UNITS_LIST[] = {
      { "  in", "  mm", 0, 25.4000 },     // 1 unit1 = 1 unit2 * conversion
      { "  yd", "  ft", 0, 3.00000 },     
      { "  kg", " lbf", 0, 2.20462 }, 
      { "   N", " lbf", 0, 0.22481 },    
      { "knot", " mph", 0, 1.15078 },  
      { " mph", " m/s", 0, 0.44704 }, 
      { " m/s", "knot", 0, 1.94384 },  
      { " ksi", " mPa", 0, 6.89476 }, 
      { "lbft", "  Nm", 0, 1.35582 }, 
      { "inlb", "  Nm", 0, 0.11298 },   
      { " mm2", " in2", 0, 0.00155 },
      { "diam", "area", 1, 1.0 },    
      { "mm_d", "in_A", 2, 1.0 },
      { " dec", "frac", 0, 1.0 }, 
    };

    uint8_t default_nextCustomIndex = ( sizeof(DEFAULT_UNITS_LIST) / sizeof(DEFAULT_UNITS_LIST[0]) ) ;

    // put default values into eeprom
    EEPROM.put(EE_currentUnit, default_currentUnit);
    delay(writeHoldTime);
    EEPROM.put(EE_longFormat, default_longFormat);
    delay(writeHoldTime);
    EEPROM.put(EE_saveValue, default_saveValue);
    delay(writeHoldTime);
    EEPROM.put(EE_sleepSetting, default_sleepSetting);
    delay(writeHoldTime);
    EEPROM.put(EE_conversionDir, default_conversionDirection);
    delay(writeHoldTime);
    EEPROM.put(EE_activeUnitsIdx, default_activeUnitsIndex);
    delay(writeHoldTime);
    EEPROM.put(EE_nextCustIdx, default_nextCustomIndex);
    delay(writeHoldTime);
    EEPROM.put(EE_UNITS_LIST, DEFAULT_UNITS_LIST);
    delay(writeHoldTime);

    EEPROM[EE_INIT] = 69; // nice

  } else {
    debugln("EEPROM UNTOUCHED");
  }
}

void clearEEPROM() {
  for (uint16_t i = 0; i < EEPROM.length(); i++) {
    EEPROM[i] = 255;
    // EEPROM.update(i, 255);  // less destructive while I test
    delay(writeHoldTime);
  }
}

void printEEPROM() {            // print out settings from EEPROM into serial monitor. currently unused function, but helpful for debugging
  
  debugln("START PRINT");

  for (int i = 0; i < 50; i++) {
    EEPROM.read(i);
    debugln(EEPROM.read(i));
    delay(writeHoldTime);
  }

  debugln("--------------------");
  debugln(EEPROM[EE_longFormat]);
  debugln(EEPROM[EE_sleepSetting]);
  debugln(EEPROM[EE_saveValue]);
  debugln(EEPROM[EE_currentUnit]);
  debugln(EEPROM[EE_nextCustIdx]);
  debugln(EEPROM[EE_activeUnitsIdx]);
  debugln("--------------------");
  debugln(activeUnitsIndex[0]);
  debugln(activeUnitsIndex[1]);
  debugln(activeUnitsIndex[2]);
  debugln(activeUnitsIndex[3]);
  debugln(conversionDirection[0]);
  debugln(conversionDirection[1]);
  debugln(conversionDirection[2]);
  debugln(conversionDirection[3]);
  debugln("--------------------");
  debugln(activeUnits[0].unit1);
  debugln(activeUnits[0].unit2);
  debugln(activeUnits[0].special);
  debugln(activeUnits[0].conversion);
  debugln("--------------------");
  debugln(activeUnits[1].unit1);
  debugln(activeUnits[1].unit2);
  debugln(activeUnits[1].special);
  debugln(activeUnits[1].conversion);
  debugln("--------------------");
  debugln(activeUnits[2].unit1);
  debugln(activeUnits[2].unit2);
  debugln(activeUnits[2].special);
  debugln(activeUnits[2].conversion);
  debugln("--------------------");
  debugln(activeUnits[3].unit1);
  debugln(activeUnits[3].unit2);
  debugln(activeUnits[3].special);
  debugln(activeUnits[3].conversion);

  debugln("END PRINT");
}

void getSettings() {        // get settings from eeprom and put them into variables

  EEPROM.get(EE_currentUnit, currentUnit);
  EEPROM.get(EE_longFormat, longFormat);
  EEPROM.get(EE_saveValue, saveValueOnSleep);
  EEPROM.get(EE_sleepSetting, sleepSetting);
  EEPROM.get(EE_conversionDir, conversionDirection);
  EEPROM.get(EE_activeUnitsIdx, activeUnitsIndex);

  EEPROM.get(EE_location(0), activeUnits[0]);
  EEPROM.get(EE_location(1), activeUnits[1]);
  EEPROM.get(EE_location(2), activeUnits[2]);
  EEPROM.get(EE_location(3), activeUnits[3]);
}

unsigned int EE_location(uint8_t idx) { // get eeprom index of units
  return EE_UNITS_LIST + (sizeof(activeUnits[0]) * activeUnitsIndex[idx]);
}

bool checkFactoryReset() {
  if (factoryResetFlag) {
    factoryResetFlag = false;
    factoryReset();
    return 1;
  } else {
    return 0;
  }
}

void factoryReset() {

  EEPROM[EE_INIT] = 255;  // this resets the EEPROM INIT location and forces a factory reset
  delay(100);             // fake delays just to make it feel substantial
  EEPROM_INIT();
  delay(100);
  getSettings();
  delay(100);
}

//------------------------------------------------------//
//
//               SLEEP / WAKE FUNCTIONS
//
//-----------------------------------------------------//

#define wakeUpPin 3  // hardware interrupt INT1
#define wakeUpPin_GND 4
#define sleepSetting_Override 600000  // 10 mins

volatile bool run_wakeUp;

unsigned long idleTimer = millis();
unsigned long battTimer = millis();

struct PWR_SETTINGS {
  unsigned long time;
  char text[7];
};

PWR_SETTINGS sleepTimer[] = {
  { 10000,  "10 sec" },
  { 30000,  "30 sec" },
  { 60000,  " 1 min" },
  { 120000, " 2 min" },
  { 300000, " 5 min" },
  { 600000, "10 min" },
};

bool checkIdle() {  // checks to see if device has been idle too long. Menus override idleTimer
  // returns TRUE for time to put device to sleep. Happens if you go too long without pressing an input
  // returns FALSE for stay awake

  unsigned long checkTimer;

  // delays sleep if you're in a menu
  if (STATE == MAIN) {
    checkTimer = sleepTimer[sleepSetting].time;
  } else {
    checkTimer = sleepSetting_Override;
  }

  if (millis() - idleTimer > checkTimer) {
    return 1;
  } else {
    return 0;
  }
}

bool checkForceSleep() {
  if (forceSleepFlag) {
    forceSleepFlag = false;
    oledPower(&dispL, 0); // turn off displays early so input feels snappy
    oledPower(&dispR, 0);
    return 1;
  } else {
    return 0;
  }
}

bool checkPwrOnLowBatt() {
  if (pwrOnLowBattFlag) {
    pwrOnLowBattFlag = false;
    return 1;
  } else {
    return 0;
  }
}

void sleep() {  // saves settings and puts the device to sleep
  // saves settings to EEPROM
  // puts the device to sleep


  debugln("*****POWER DOWN******");
  delay(1000);

  oledPower(&dispL, 0);
  oledPower(&dispR, 0);

  if (!saveValueOnSleep || STATE != MAIN){
    STATE = MAIN;
    resetCurrentValue(); 
    setCurrentValue();
    draw_MAIN();
  }

  pinMode(wakeUpPin_GND, OUTPUT);   // turn interrupt button on
  digitalWrite(wakeUpPin_GND, LOW);

  attachInterrupt(digitalPinToInterrupt(wakeUpPin), wakeUp_ISR, LOW);
  delay(300); // add delay. every once in awhile, the unit will shut down and will not come back on. can't track down why but adding delay between attach interrupt and power down may help
  LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
  // ... //
  detachInterrupt(digitalPinToInterrupt(wakeUpPin));

}

void wakeUp_ISR() {  // Interrupt Service Routinge
  // wakeUp() is too slow to run as ISR, so flag it with run_wakeUp. detachInterrupt also too slow to be in ISR
  run_wakeUp = true;
}

void wakeUp() {  // wake the device up, reset timers, initialize displays
  // called by interrupt :: wakes the device up
  // turns off interrupt button, checks for low battery, turns on the displays, continues back to loop

  debugln("*****WAKE UP******");

  pinMode(wakeUpPin_GND, INPUT);  // turn interrupt button off

  idleTimer = millis();           // reset timers
  battTimer = millis();         

  //initBattVoltage();            // used with EWMA filter on batt voltage

  if (checkLowBattery()) {
    pwrOnLowBattFlag = true;
  }

  run_wakeUp = false;             // reset run_wakeUp

  oledPower(&dispL, 1);           // turn displays on, could make this conditional with pwrOnLowBattFlag
  oledPower(&dispR, 1);
}

void resetKeyState() {
  // for some reason, when sleep() is called, the keystate from keypad.h is not reset
  // should figure out how to do this. but for now, here's a hack
  // calls getKey 3 times. This forces the keystate to idle before the device goes to sleep 
  // this is only needed for forcepowerdown or factoryreset  
  delay(100);
  key = tenKey.getKey(); // pressed
  delay(100);
  key = tenKey.getKey(); // released
  delay(100);
  key = tenKey.getKey(); // idle
}

//------------------------------------------------------//
//
//              BATTERY MONITOR FUNCTIONS
//
//-----------------------------------------------------//

#define voltageSensePin A2
#define refVoltage 3.3
#define volt_divider_R1 100.0       // kilo Ohms :: 100 kO
#define volt_divider_R2 100.0

// voltage_correction_factor is in User Configurable Settings
#define battCheckDelayTime 5000      // only check battery every 5 seconds! does less float math and lets battery voltage average out
#define weight 20                    // Weight for EWMA :: 0 - 100. Higher values add more weight to newer readings

unsigned int batteryVoltage;
unsigned int battPercent;
unsigned int prev_batteryVoltage = 360;  // set to 3.6V as a rough bias correction for EWMA filter
unsigned int warningVoltage = 340;       // centi-Volt :: 330 = 3.30V
unsigned int shutdownVoltage = 335;      // MINIMUM safe shutdown voltage.  LDO does not regulate below 3.30V so ADC value will float when input value < 3.30V rendering ADC useless (refVoltage = battVoltage when battVoltage < 3.30V) 
bool warningVoltageFlag = false;         // not necessary, but nice to have so low battery warning doesnt flicker

void getBatteryVoltage() {  // get current battery voltage using Exp Weighted Moving Average :: 3.375V ---->  338

  batteryVoltage = rawVoltage() * 100.0;  // convert float voltage to 3 digit cV output :: 4.235645 ---->  424

  // remove EMWA filter from calculation - probably not necessary since batt polling rate is so low
  // could also remove the loop from initBattVoltage
  //batteryVoltage = weight * batteryVoltage + (100 - weight) * prev_batteryVoltage;
  //batteryVoltage /= 100;
  //prev_batteryVoltage = batteryVoltage;
}

float rawVoltage() {  // compute the raw voltage as float from analog pin, does voltage divider math
  return ((volt_divider_R1 + volt_divider_R2) / volt_divider_R2) * analogRead(voltageSensePin) * (refVoltage / 1024.0) * (voltage_correction_factor / 100.0);
}

void initBattVoltage() {  // initializes the EWMA by computing voltage 30 times
  for (uint8_t i = 0; i < 30; i++) {
    getBatteryVoltage();
    debugln(batteryVoltage);
  }
}

bool checkLowBattery() {  // checks for low voltage or warning voltage

  if (millis() - battTimer > battCheckDelayTime || run_wakeUp) { // always check battery on wake

    battTimer = millis();

    getBatteryVoltage();

    //draw_BatteryStatus();   // This will live-update the battery voltage in the settings menu. Comment out to only load battery status once per entry
    //draw_VoltageOnMain();   // Show voltage on MAIN state. Comment out to disable

    debug("Volts:   ");
    debugln(batteryVoltage);

    if (batteryVoltage < shutdownVoltage) {
      warningVoltageFlag = true;
      draw_BattWarning();
      return 1;
    } else if (batteryVoltage < warningVoltage) {
      warningVoltageFlag = true;
      draw_BattWarning();
    } else if (batteryVoltage > (warningVoltage+5) ) { // add a bit to warning voltage to prevent flicker
      warningVoltageFlag = false;
      draw_BattWarning();
    }
  }
  return 0;
}

//------------------------------------------------------//
//
//               MAIN FUNCTIONS
//
//-----------------------------------------------------//

#define inputLength 8  // max digits = 6. add +1 for decimal. add +1 for null terminator '/0'
#define outputLength 8
#define zeros "0.00000"  // zeroes output result for formatting. number of zeros = outputLength - 2

// INPUT
char currentValue[inputLength];
uint8_t charIndex = 0;
float input_float;
bool allowRadix = true;
bool allowDivide = false;   // don't allow '/' to be the first character
uint8_t sigFigs = 0;
bool resetCurrentValueFlag = false;
bool containsRadix = false;
bool anotherRadixFlag = false;  // yet another flag to catch an edge case....

// OUTPUT
char result_char[outputLength];
float result_float;

// PROTOTYPES
void undoLastInput(bool any = 1, char c = 0); // default argument prototype

// FUNCTIONS
void setUnit() {  // sets current conversion to appropriate index in activeUnits struct
  if (key == 'A') {
    currentUnit = 0;
  } else if (key == 'B') {
    currentUnit = 1;
  } else if (key == 'C') {
    currentUnit = 2;
  } else {
    currentUnit = 3;
  }
}

void invertConversion() {  // changes conversion direction if selected unit is pressed again
  
  if (key == 'A' && currentUnit != 0) {
    return;
  } else if (key == 'B' && currentUnit != 1) {
    return;
  } else if (key == 'C' && currentUnit != 2) {
    return;
  } else if (key == 'D' && currentUnit != 3) {
    return;
  } else {
  conversionDirection[currentUnit] = !conversionDirection[currentUnit];
  }
}

void setCurrentValue() {    // adds key to next index of currentValue char array, with some formatting rules

  // do nothing if the char buffer is full
  if (charIndex == inputLength-1) {
    return;
  }
  
  // don't allow '/' to be the second character if the first character is '.', otherwise allow '/'
  if (charIndex == 1 && currentValue[0] != '.') {
    allowDivide = true;
  } else if (charIndex == 2 && currentValue[1] != '/') {
    allowDivide = true;
  }

  // if the last input was '.' and the next input is '/', overwrite '.' with '/' and decrement charIndex
  if (key == '/' && allowDivide && currentValue[charIndex-1] == '.') {
    containsRadix = false;
    charIndex--;
  }

  // if only 1 character remains, do not allow final character to be '.' or '/'
  if (charIndex == inputLength-2) {
    allowRadix = false;
    allowDivide = false;
  }

  // if '.' is pressed, disallow further '.'
  if (key == '.' && allowRadix) {
    containsRadix = true;
    allowRadix = false;
  } else if (key == '.' && !allowRadix) {
    return;
  }

  // if '/' is pressed, disallow further '/' - allow '.' again for denominator
  if (key == '/' && allowDivide) {
    allowDivide = false;
    allowRadix = true;

    if (containsRadix){
      containsRadix = false;
      anotherRadixFlag = true;
    }

  } else if (key == '/' && !allowDivide) {
    return;
  }

  currentValue[charIndex] = key;
  charIndex++;
  inputFloat();
}

void inputFloat() {         // split input char into fraction and compute result, compute number of SigFigs from input

  char numerator[inputLength] = {'\0'};
  char denominator[inputLength] = {'\0'};
  float top;
  float bottom;
  bool isNumerator = true;
  uint8_t j = 0;
  uint8_t k = 0;
  bool hasSigFigs = false;
  uint8_t nSF = 0;
  uint8_t dSF = 0;

  // step thru currentValue and parse into numerator and denomenator, use '/' as deliminator
  for (uint8_t i=0 ; i<inputLength ; i++) {

    if (currentValue[i] == '/') {
      isNumerator = false;
      hasSigFigs = false;
      i++; // skips over '/'
    }

    if (isNumerator) {
      numerator[j] = currentValue[i];
      j++;
    } else {
      denominator[k] = currentValue[i];
      k++;
    }

    if (currentValue[i] == '.') {
      hasSigFigs = true;
    }

    if (hasSigFigs && currentValue[i] != '\0' && currentValue[i] != '.') {
      if (isNumerator) {
        nSF++;
      } else {
        dSF++;
      }
    }
  }

  top = atof(numerator);
  bottom = atof(denominator);

  if (bottom == 0) {  // don't divide by zero
    bottom = 1;
  }

  input_float = top / bottom;   // calculate input float value


  if (nSF >= dSF) {
    sigFigs = nSF;
  } else {
    sigFigs = dSF;
  }

  // serial monitor output
  debug("numerator:   ");
  debug(numerator);
  debug("  --->   ");
  debugF(top);
  debugln();
  debug("denominator:   ");
  debug(denominator);
  debug("  --->   ");
  debugF(bottom);
  debugln();
}

void undoLastInput(bool any, char c) {
  // if any = TRUE, will undo ANY last input
  // if any = FALSE, will ONLY undo if last input = c 
  
  if ( any || (currentValue[charIndex-1] == c && !any)) {          
    charIndex--;
    currentValue[charIndex] = 32;
    draw_Values();
    currentValue[charIndex] = '\0';
  }

  doMath();
  draw_Values();
}

void resetCurrentValue() {  // resets current value and reinitializes formatting flags
  memset(currentValue, '\0', inputLength);  //reset currentValue to null
  charIndex = 0;
  allowRadix = true;
  allowDivide = false;
  containsRadix = false;
  anotherRadixFlag = false;
  resetCurrentValueFlag = true;
  sigFigs = 0;
}

void doMath() {  // do multiply/divide or do special function

  switch (activeUnits[currentUnit].special) {

    case 0: //// default multiply or divide for general conversions ////
      if (conversionDirection[currentUnit]) {
        result_float = input_float * activeUnits[currentUnit].conversion;
      } else {
        result_float = input_float / activeUnits[currentUnit].conversion;
      }
      break;

    case 1:  // diameter -> area
      if (conversionDirection[currentUnit]) {
        result_float = (3.14159) * input_float * input_float / 4;
      } else {
        result_float = sqrt(4 * input_float / 3.14159);
      }
      break;

    case 2: // mm diameter -> inch area
      if (conversionDirection[currentUnit]) {
        result_float = (3.14159) * input_float * input_float / 4 / 25.4 / 25.4;
      } else {
        result_float = sqrt(4 * input_float / 3.14159) * 25.4 * 25.4;
      }
      break;
  }

  resultChar();  // compute and format result
}

void resultChar() {  // do all output formatting of result
  //resultChar handles the output formatting -- taking in the output float and displaying a result max 6 digits long or scientific if necessary

  long leadingDigits = (long)result_float;
  bool longFormat_Override = longFormat;  // there to override user selected short/long format. can remove if you dont want the option to choose

  // set 0.xxx = 1 leading digit and override to long format for precision
  if (leadingDigits == 0) {
    leadingDigits = 1;
    longFormat_Override = true;
  }

  // count number of leading digits in result
  uint8_t countDigits = 0;
  while (leadingDigits) {
    leadingDigits = leadingDigits / 10;
    countDigits++;
  }

  // determine output format; how many digits must be present at end of decimal -> depends on A: longFormat; B: number of sigfigs in input; C: number of leading digits in output
  uint8_t num_digits_after_decimal;
  int8_t remaining_digits_allowed = outputLength - countDigits - 2;  // note that this value CAN go negative so cant be unsigned! subtract 2 for decimal and null terminator

  if (longFormat || longFormat_Override) {  // long formatting logic
    num_digits_after_decimal = remaining_digits_allowed;
  } else {  // short formatting logic
    if (remaining_digits_allowed > sigFigs) {
      num_digits_after_decimal = sigFigs + 1;
    } else if (remaining_digits_allowed >= 0) {
      num_digits_after_decimal = remaining_digits_allowed;
    } else {
      num_digits_after_decimal = 0;  // this else statement is redundant since this line is implicit, but it improves clarity and shows design intent
    }
  }

  // format output: either scientific notation, or decimal notation
  if (countDigits > outputLength - 2) {                          // do scientific notation  <-- consider improving this. currently output is only allowed 2 digits total.
    dtostre(result_float, result_char, outputLength - 7, 0x04);  // subtract 7: 1: /0, 2: '.', 3: '+/-', 4: 1st exp digit, 5: 2nd exp digit, 6: leading single digit, 7: 'E'
  } else {                                                       // do decimal notation
    dtostrf(result_float, 1, num_digits_after_decimal, result_char);
  }

  // format "0.00000" output to "0.0"
  if (!strcmp(result_char, zeros)) {  // strcmp throws 0 when strings are the same
    strcpy(result_char, "0.0");
  }

  // fill remainder of result_char with the space character, 32 :: {'3','.','1','2',\0,\0,\0,\0} ---> {'3','.','1','2',32,32,32,\0}
  for (uint8_t j = strlen(result_char); j < outputLength - 1; j++) {  // this aids formatting since moving from strlen 7>6 wont delete old characters. this ensures all strlen are 7
    result_char[j] = ' ';
  }
}


//------------------------------------------------------//
//
//               DISPLAY FUNCTIONS
//
//-----------------------------------------------------//

#define NUMBER_FONT FONT_LARGE
#define UNIT_FONT FONT_STRETCHED
#define BANNER_FONT FONT_NORMAL
#define SETTINGS_FONT FONT_NORMAL
#define SMALL_FONT FONT_SMALL
#define numX 0
#define numY 0
#define unitX 60
#define unitY 4
#define bannerX1 0
#define bannerX2 65
#define bannerY 6
#define settingX1 0
#define settingX2 65
#define settingY  2
#define battX 0
#define battY 5
#define clearUnits "    "       // {32,32,32,32,/0} 4 spaces char array :: reference unitStringLength
#define clearNumbers "       "  // {32,32,32,32,32,32,32,/0} 7 spaces char array  :: reference inputLength & outputLength
#define clearRow "                  " // should clear a whole row of information as long as wordwrap is off

// ADMIN ------------------------------

void clearDisplays() {  // clears both displays to black
  oledFill(&dispL, 0, 1);
  oledFill(&dispR, 0, 1);
}

void serialPrintMath() {  // print input, result, and units to serial monitor

  if (conversionDirection[currentUnit]) {  // true is multiply
    debug(currentValue);
    debug(" ");
    debug(activeUnits[currentUnit].unit1);
    debug("  --->   ");
    debug(result_char);
    debug(" ");
    debug(activeUnits[currentUnit].unit2);
    debug("  ");
    debugF(result_float);
    debugln();
  } else {
    debug(currentValue);
    debug(" ");
    debug(activeUnits[currentUnit].unit2);
    debug("  --->   ");
    debug(result_char);
    debug(" ");
    debug(activeUnits[currentUnit].unit1);
    debug("  ");
    debugF(result_float);
    debugln();
  }
}

void serialPrintReset() {  // indicate value has been reset
  debug(currentValue);
  debugln("*");
}

// MAIN ------------------------------

void draw_MAIN() {  // clears displays and goes back to MAIN routine
  clearDisplays();
  doMath();
  draw_Values();
  draw_Units();
  draw_BattWarning();
}

void draw_Values() {  // updates left and right displays with currentValue and result_char
    
  if (charIndex == 0 || resetCurrentValueFlag) {
    resetCurrentValueFlag = false;
    oledWriteString(&dispL, 0, numX, numY, clearNumbers, NUMBER_FONT, 0, 1);
  }

  oledWriteString(&dispL, 0, numX, numY, currentValue, NUMBER_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, numY, result_char, NUMBER_FONT, 0, 1);
}

void draw_Units() {  // updates left and right displays with correct units based on conversion direction

  if (conversionDirection[currentUnit]) {  // true is multiply; unit1 = unit2 * conversion; unit1 ---> unit2
    oledWriteString(&dispL, 0, unitX, unitY, activeUnits[currentUnit].unit1, UNIT_FONT, 0, 1);
    oledWriteString(&dispR, 0, unitX, unitY, activeUnits[currentUnit].unit2, UNIT_FONT, 0, 1);
  } else {
    oledWriteString(&dispL, 0, unitX, unitY, activeUnits[currentUnit].unit2, UNIT_FONT, 0, 1);
    oledWriteString(&dispR, 0, unitX, unitY, activeUnits[currentUnit].unit1, UNIT_FONT, 0, 1);
  }
}

void draw_ResetValues() {  // highlights currentValue to show user it is ready to be overwritten
  oledWriteString(&dispL, 0, numX, numY, currentValue, NUMBER_FONT, 1, 1);
}

// FULL MENUS ------------------------------

void draw_SettingsMenu() {  // wrapper to draw all fields in settings menu
  clearDisplays();
  draw_SleepSetting();
  draw_FormatSelection();
  draw_BatteryStatus();
  drawBanner("SLEEP", "FORMAT", "NEXT", "SAVE");
}

void draw_MoreSettings() {
  clearDisplays();
  draw_BatteryStatus();
  draw_SaveValue();
  oledWriteString(&dispL, 0, bannerX1, bannerY-1, "POWER", BANNER_FONT, 0, 1);
  oledWriteString(&dispL, 0, bannerX2, bannerY-1, "SAVE", BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, bannerX1, bannerY-1, "FACTORY", BANNER_FONT, 0, 1);
  drawBanner("DOWN", "VALUE", "RESET", "BACK");
}

void draw_UnitsMenu() {  // wrapper to draw all fields in units menu
  clearDisplays();
  draw_SelectedUnit();
  drawBanner("PREV", "NEXT", "NEW", "SAVE");
}

void draw_UnitsOverwrite(){
  clearDisplays();
  oledWriteString(&dispL, 0, numX, 2, "ARE YOU SURE?", BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 1, "This will overwrite", SMALL_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 2, "the currently", SMALL_FONT, 0, 1);               //////////////// this line is weird
  oledWriteString(&dispR, 0, numX, 3, "selected unit", SMALL_FONT, 0, 1);
  drawBanner("YES", "   ", "   ","NO");
}

// SUB MENUS ------------------------------

void draw_Unit1() {
  clearDisplays();
  drawBanner("UP","DOWN","NEXT","UNIT 2");
  draw_UserUnit1();
  draw_UserUnit2();
  draw_UserUnitIndicator(1);
  oledWriteString(&dispL, 0, 0, 0, "UNIT 1:", SMALL_FONT, 0, 1);
  oledWriteString(&dispR, 0, 0, 0, "UNIT 2:", SMALL_FONT, 0, 1);

}

void draw_Unit2() {
  oledWriteString(&dispL, 0, 0, bannerY, clearRow, BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, 0, bannerY, clearRow, BANNER_FONT, 0, 1);
  drawBanner("UP","DOWN","NEXT","CONVERT");
  draw_UserUnitIndicator(0);
}

void draw_Convert() {
  clearDisplays();
  drawBanner("SAVE", "EXIT", "UNDO", "UNIT 1");
  draw_UserConvertInfo();
  draw_UserConvertValue();
}

void draw_FactoryReset() {  
  clearDisplays();
  oledWriteString(&dispL, 0, numX, 2, "ARE YOU SURE?", BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 1, "This will reset all", SMALL_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 2, "settings and wll", SMALL_FONT, 0, 1);             
  oledWriteString(&dispR, 0, numX, 3, "clear all custom", SMALL_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 4, "units", SMALL_FONT, 0, 1);
  drawBanner("YES", "   ", "   ","NO");
}

// BANNERS ------------------------------

void drawBanner(char a[], char b[], char c[], char d[]) {
  oledWriteString(&dispL, 0, bannerX1, bannerY, a, BANNER_FONT, 0, 1);
  oledWriteString(&dispL, 0, bannerX2, bannerY, b, BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, bannerX1, bannerY, c, BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, bannerX2, bannerY, d, BANNER_FONT, 0, 1);
}

// INDIVIDUAL SETTINGS ------------------------------

void draw_SleepSetting() {  // draws currently selected sleep setting
  oledWriteString(&dispL, 0, settingX1, settingY, sleepTimer[sleepSetting].text, SETTINGS_FONT, 0, 1);
}

void draw_FormatSelection() {  // draws currently selected short/long format setting
  if (longFormat) {
    oledWriteString(&dispL, 0, settingX2, settingY, "LONG ", SETTINGS_FONT, 0, 1);
  } else {
    oledWriteString(&dispL, 0, settingX2, settingY, "SHORT", SETTINGS_FONT, 0, 1);
  }
}

void draw_SaveValue() {  // draws currently selected short/long format setting
  if (saveValueOnSleep) {
    oledWriteString(&dispL, 0, settingX2, settingY, "SAVE ", SETTINGS_FONT, 0, 1);
  } else {
    oledWriteString(&dispL, 0, settingX2, settingY, "CLEAR", SETTINGS_FONT, 0, 1);
  }
}

void draw_SelectedUnit() {

  EEPROM.get(EE_location(currentUnit), activeUnits[currentUnit]);

  debugln(activeUnits[currentUnit].unit1);
  debugln(activeUnits[currentUnit].unit2);

  if (activeUnitsIndex[currentUnit] != nextCustomIndex ) {
    oledWriteString(&dispL, 0, numX+20, numY, activeUnits[currentUnit].unit1, NUMBER_FONT, 0, 1);
    oledWriteString(&dispR, 0, numX+20, numY, activeUnits[currentUnit].unit2, NUMBER_FONT, 0, 1);
  } else {
    oledWriteString(&dispL, 0, numX+20, numY, "NEW ", NUMBER_FONT, 0, 1);
    oledWriteString(&dispR, 0, numX+20, numY, "UNIT", NUMBER_FONT, 0, 1);
  }

  char buffer[4];
  
  oledWriteString(&dispL, 0, 0, 4, clearRow, SETTINGS_FONT, 0, 1);
  oledWriteString(&dispL, 0, 30, 4, itoa(activeUnitsIndex[currentUnit]+1, buffer, 10), SETTINGS_FONT, 0, 1);
  oledWriteString(&dispL, 0, 30+20, 4, "/", SETTINGS_FONT, 0, 1);
  oledWriteString(&dispL, 0, 30+33, 4, itoa(LIST_SIZE, buffer, 10), SETTINGS_FONT, 0, 1);

}

void draw_ResetConfirm() {
  clearDisplays();
  oledWriteString(&dispL, 0, numX, 2, "FORMAT-", UNIT_FONT, 0, 1);
  oledWriteString(&dispL, 0, numX, 2+2, "TING", UNIT_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 2, "PLEASE", UNIT_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 2+2, "WAIT", UNIT_FONT, 0, 1);
}

// BATTERY DISPLAYS ------------------------------

void draw_BattWarning() {
  if (warningVoltageFlag && STATE == MAIN) {
    oledWriteString(&dispL, 0, battX, battY, "LOW BATT", SMALL_FONT, 0, 1); // cant fit "battery" since it runs into units :c    
  } else if (!warningVoltageFlag && STATE == MAIN) {
    oledWriteString(&dispL, 0, battX, battY, "        ", SMALL_FONT, 0, 1);  
  }
}

void draw_VoltageOnMain() {   // This function will display battery voltage on main screen if its uncommented in CheckBattery()
  if (STATE == MAIN) {
    char buffer[4];
    itoa(batteryVoltage, buffer, 10);
    char batt_char[6] = {buffer[0], '.', buffer[1], buffer[2], 'V', 0};
    oledWriteString(&dispL, 0, 0, 6, batt_char, SMALL_FONT, 0, 1);
  }
}

void draw_BatteryStatus() {  // draws current battery voltage/percentage in SETTINGS_MAIN
  char buffer[4];
  itoa(batteryVoltage, buffer, 10);

  char batt_char[6] = {buffer[0], '.', buffer[1], buffer[2], 'V', 0};

  oledWriteString(&dispR, 0, 85, 0, batt_char, SMALL_FONT, 0, 1);

  if (warningVoltageFlag) {
    oledWriteString(&dispR, 0, 0, 0, "LOW BATTERY", SMALL_FONT, 0, 1);
  } else {
    oledWriteString(&dispR, 0, 0, 0, "           ", SMALL_FONT, 0, 1);
  }
}

//------------------------------------------------------//
//
//             USER CUSTOM UNITS FUNCTIONS
//
//------------------------------------------------------//
char usr_letter = 96; // 96 --> 97 = 'a'
char usr_unit1[unitStringLength];
char usr_unit2[unitStringLength];
uint8_t usr_index = 0;

void initCustomUnits() {
  strcpy(usr_unit1, clearUnits);
  strcpy(usr_unit2, clearUnits);
  resetCurrentValue();
}

void selectUnit(bool direction) {
  // 0 is decrement :: 1 is increment

  // increment or decrement
  if (direction) {
    activeUnitsIndex[currentUnit]++;
  } else {
    activeUnitsIndex[currentUnit]--;
  }

  // loop around available units. stop looping at LIST_SIZE if UNITS_LIST is completely full
  if (activeUnitsIndex[currentUnit] > nextCustomIndex || activeUnitsIndex[currentUnit] > LIST_SIZE-1) {

    activeUnitsIndex[currentUnit] = 0;

  } else if (activeUnitsIndex[currentUnit] < 0) {

    if (nextCustomIndex > LIST_SIZE-1) {
      activeUnitsIndex[currentUnit] = LIST_SIZE-1;
    } else {
      activeUnitsIndex[currentUnit] = nextCustomIndex;
    }
  }

  debugln(activeUnitsIndex[currentUnit]);

}

void saveNewUnit() {

  // create struct for new unit
  UNITS_STRUCT NEW_UNIT;
  strcpy(NEW_UNIT.unit1, usr_unit1);
  strcpy(NEW_UNIT.unit2, usr_unit2);
  NEW_UNIT.special = 0;
  NEW_UNIT.conversion = input_float;

  // replace activeUnits with new unit
  activeUnits[currentUnit] = NEW_UNIT;
  conversionDirection[currentUnit] = 1; // set conversion direction to match default

  if (activeUnitsIndex[currentUnit] == nextCustomIndex){ // increment ONLY if changed unit is at end of list
    nextCustomIndex++;
  }

  EEPROM.put(EE_nextCustIdx, nextCustomIndex);
  EEPROM.put(EE_activeUnitsIdx, activeUnitsIndex);
  EEPROM.put(EE_location(currentUnit), NEW_UNIT);
}


// USER DEFINED UNITS DISPLAYS ------------------------------

void draw_UserUnit1() {
  oledWriteString(&dispL, 0, 45, 2, usr_unit1, UNIT_FONT, 0, 1);
}

void draw_UserUnit2() {
  oledWriteString(&dispR, 0, 45, 2, usr_unit2, UNIT_FONT, 0, 1);
}

void draw_UserUnitIndicator(bool side) {

  unsigned int xloc = 45+5 + (16 * usr_index);
  char indicator[] = "^";

  oledWriteString(&dispL, 0, 0, 2+2, clearRow, BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, 0, 2+2, clearRow, BANNER_FONT, 0, 1);

  if (side) {
  oledWriteString(&dispL, 0, xloc, 2+2, indicator, BANNER_FONT, 0, 1);
  } else {
  oledWriteString(&dispR, 0, xloc, 2+2, indicator, BANNER_FONT, 0, 1);
  }
}

void draw_UserConvertInfo() {
  oledWriteString(&dispL, 0, numX, numY, "1.0", NUMBER_FONT, 0, 1);
  oledWriteString(&dispL, 0, unitX, unitY, usr_unit1, UNIT_FONT, 0, 1);
  oledWriteString(&dispR, 0, unitX, unitY, usr_unit2, UNIT_FONT, 0, 1);
}

void draw_UserConvertValue() {
  if (charIndex == 1) {
    oledWriteString(&dispR, 0, numX, numY, clearNumbers, NUMBER_FONT, 0, 1);
  }
  oledWriteString(&dispR, 0, numX, numY, currentValue, NUMBER_FONT, 0, 1);
}



//------------------------------------------------------//
//
//                    STATE FUNCTIONS
//
//------------------------------------------------------//

void transitionTo(DEVICE_STATES TO) {
  switch (TO) {
    case BEGIN: break;  // unused

    case MAIN:
      // debugln("MAIN");
      STATE = MAIN;
      draw_MAIN();
      break;

    case SETTINGS_MAIN:
      // debugln("SETTINGS");
      STATE = SETTINGS_MAIN;
      draw_SettingsMenu();
      break;

    case SETTINGS_MORE:
      // debugln("MORE");
      STATE = SETTINGS_MORE;
      draw_MoreSettings();
      break;

    case SETTINGS_RST:
      // debugln("FACTORY RESET");
      STATE = SETTINGS_RST;
      draw_FactoryReset();
      break;

    case UNITS_MAIN:
      // debugln("UNITS - CALLED");
      STATE = UNITS_MAIN;
      EEPROM.get(EE_nextCustIdx, nextCustomIndex);
      draw_UnitsMenu();
      break;
    
    case UNITS_OVERWRITE:
      // debugln("NEW");
      STATE = UNITS_OVERWRITE;
      draw_UnitsOverwrite();
      break;

    case UNITS_UNIT1:
      STATE = UNITS_UNIT1;
      usr_letter = 96;
      usr_index = 0;
      draw_Unit1();
      break;

    case UNITS_UNIT2:
      STATE = UNITS_UNIT2;
      usr_letter = 96;
      usr_index = 0;
      draw_Unit2();
      break;

    case UNITS_CONVERT:
      STATE = UNITS_CONVERT;
      draw_Convert();
      break;
  }
}

//------------------------------------------------------//
//
//                    MENUS
//
//------------------------------------------------------//

// Menu and Submenu ENUMS defined at top of file

void enter_menus(KeypadEvent key) {  // KeypadEvent handler for detecting long press - built into keypad.h

  if (tenKey.getState() == HOLD && STATE == MAIN) {
    switch (key) {
      case 'A': case 'B': case 'C': case 'D':
        transitionTo(UNITS_MAIN);
        break;

      case '.':
        if (currentValue[charIndex-1] == '.'){
          allowRadix = true;
          containsRadix = false;
        }
        undoLastInput(0, '.');
        transitionTo(SETTINGS_MAIN);
        break;

      case '/':
        if (currentValue[charIndex-1] == '/'){
          allowDivide = true;
        }
        if (anotherRadixFlag) {
          anotherRadixFlag = false;
          allowRadix = false;
        }
        longFormat = !longFormat;
        undoLastInput(0, '/');
        EEPROM.put(EE_longFormat, longFormat);
    }
  }
}

void settings_menu() {  // change settings in the SETTINGS_MAIN, display results
  switch (key) {

    case 'A':  // SLEEP SETTINGS
      sleepSetting++;
      if (sleepSetting > ( sizeof(sleepTimer) / sizeof(sleepTimer[0]) ) - 1 ) { sleepSetting = 0; }
      draw_SleepSetting(); 
      break;

    case 'B':  // NUMBER FORMAT SETTING
      longFormat = !longFormat;
      draw_FormatSelection();
      break;

    case 'C':  // MORE SETTINGS
      transitionTo(SETTINGS_MORE);
      break;

    case 'D':  // SAVE AND EXIT TO MAIN
      EEPROM.put(EE_longFormat, longFormat);
      EEPROM.put(EE_sleepSetting, sleepSetting);
      EEPROM.put(EE_saveValue, saveValueOnSleep);
      transitionTo(MAIN);
      break;
  }
}

void more_menu() {
  switch (key) {

    case 'A':  // POWER OFF
      forceSleepFlag = true;
      break;

    case 'B':  // SAVE VALUES ON SLEEP
      saveValueOnSleep = !saveValueOnSleep;
      draw_SaveValue();
      break;

    case 'C':  // FACTORY RESET
      transitionTo(SETTINGS_RST);
      break;

    case 'D':  // BACK TO SETTINGS
      transitionTo(SETTINGS_MAIN);
      break;
  }
}

void fact_reset_menu() {
  switch (key) {

    case 'A':  // YES
      draw_ResetConfirm();
      factoryResetFlag = true;
      break;

    case 'B': case 'C': break; // unused

    case 'D':  // NO
      transitionTo(SETTINGS_MORE);
      break;
  }
}

void units_menu() {  // change units in the UNITS_MAIN, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      selectUnit(0);
      draw_SelectedUnit();
      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      selectUnit(1); 
      draw_SelectedUnit();
      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      if (activeUnitsIndex[currentUnit] == nextCustomIndex){
        initCustomUnits();
        transitionTo(UNITS_UNIT1);
      } else {
        transitionTo(UNITS_OVERWRITE);
      }
      break;

    case 'D':  // EXIT TO MAIN MENU

      if (activeUnitsIndex[currentUnit] == nextCustomIndex) {
        break; // dont do anything if user hasnt selected valid unit
      }

      conversionDirection[currentUnit] = 1; // set conversion direction to match default
      EEPROM.put(EE_activeUnitsIdx, activeUnitsIndex);

      transitionTo(MAIN);
      break;
  }
}

void confirm_overwrite() { 
  switch (key) {

    case 'A':  // YES
      initCustomUnits();
      transitionTo(UNITS_UNIT1);
      break;

    case 'B': case 'C': break; // unused

    case 'D':  // NO
      transitionTo(UNITS_MAIN);
      break;
  }
}

void set_unit1() {  // change units in UNITS_UNIT1, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      usr_letter--;
      if (usr_letter < 32) { usr_letter = 126; }  // skip special characters
      usr_unit1[usr_index] = usr_letter;
      draw_UserUnit1();
      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      usr_letter++;
      if (usr_letter > 126) { usr_letter = 32; }
      usr_unit1[usr_index] = usr_letter;
      draw_UserUnit1();
      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      usr_letter = 96;
      usr_index++;
      debugln(usr_index);
      if (usr_index > unitStringLength - 2){
        usr_index = 0;
      }
      draw_UserUnitIndicator(1);

      break;

    case 'D':  // GO TO NEXT UNIT
      transitionTo(UNITS_UNIT2);
      break;
  }
}

void set_unit2() {  // change units in UNITS_UNIT2, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      usr_letter--;
      if (usr_letter < 32) { usr_letter = 126; }
      usr_unit2[usr_index] = usr_letter;
      draw_UserUnit2();
      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      usr_letter++;
      if (usr_letter > 126) { usr_letter = 32; }
      usr_unit2[usr_index] = usr_letter;
      draw_UserUnit2();
      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      usr_letter = 96;
      usr_index++;
      debugln(usr_index);
      if (usr_index > unitStringLength - 2){
        usr_index = 0;
      }
      draw_UserUnitIndicator(0);
      break;

    case 'D':  // GO TO CONVERSION INPUT
      transitionTo(UNITS_CONVERT);
      break;
  }
}

void set_convert() {  // change number in UNITS_CONVERT, display results

  switch (key) {
    case 'A': // SAVE
      debugln("SAVE");
      if (input_float == 0) { // cancel if user conversion is zero. Zero will break the code (divide by zero)
        break;
      }
      saveNewUnit();
      resetCurrentValue();
      transitionTo(MAIN);
      break;  
      
    case 'B': // CANCEL
      debugln("CANCEL");
      transitionTo(UNITS_MAIN);
      resetCurrentValue();
      break;

    case 'C':  // GO TO CONVERSION INPUT
      debugln("DELETE");
      resetCurrentValue();
      oledWriteString(&dispR, 0, numX, numY, clearNumbers, NUMBER_FONT, 0, 1);
      break;

    case 'D':  // LOOP BACK TO UNIT1
      debugln("BACK TO UNIT1");
      transitionTo(UNITS_UNIT1);
      break;

    case 'x': case '/': break;  // SKIP UNUSED KEYS, disallow fractional input by preventing '/'

    default:
      setCurrentValue();
      draw_UserConvertValue();
  }
}

//------------------------------------------------------//
//
//               UNIT CONVERSION CODE
//
//-----------------------------------------------------//

void UNIT_CONVERSION() {

  switch (key) {
    case 'x': break;  // SKIP UNUSED KEYS (only needed for dev work with matrix keypad)
    case 'A': case 'B': case 'C': case 'D':
      invertConversion();
      setUnit();
      doMath();
      draw_Units();
      draw_Values();
      serialPrintMath();
      break;

    case 'E':
      draw_ResetValues();
      resetCurrentValue();
      serialPrintReset();
      break;

    default:
      setCurrentValue();
      doMath();
      draw_Values();
      serialPrintMath();

  }
}

//------------------------------------------------------//
//
//                    SETUP
//
//-----------------------------------------------------//

void setup() {

  STATE = BEGIN;   // currently unused

  startSerial;

  // at any time, reset Byte 02 to 255 to fully reset device
  EEPROM_INIT();  // also funtions to init EEPROM on a brand new board
  getSettings();

  // initialize Pin 2 for use as external interrupt
  pinMode(wakeUpPin, INPUT_PULLUP);
  pinMode(voltageSensePin, INPUT);

  // disable internal pullups on I2C lines since we're using external pullups
  digitalWrite(SDA_L, LOW);
  digitalWrite(SCL_L, LOW);
  digitalWrite(SDA_R, LOW);
  digitalWrite(SCL_R, LOW);

  // initialize keyboard settings
  tenKey.addEventListener(enter_menus);
  tenKey.setHoldTime(long_press_time);
  tenKey.setDebounceTime(debounce_time);

  // initialize OLED displays
  // oledInit(SSOLED *, type, oled_addr, rotate180, invert, bWire, SDA_PIN, SCL_PIN, RESET_PIN, speed)
  oledInit(&dispL, displaySize_L, I2C_Address_L, flipDisplay_L, invertDisplay_L, USE_HW_I2C_L, SDA_L, SCL_L, displayResetPin, 400000L);  // 400kHz
  oledInit(&dispR, displaySize_R, I2C_Address_R, flipDisplay_R, invertDisplay_R, USE_HW_I2C_R, SDA_R, SCL_R, displayResetPin, 400000L);  // 400kHz
  oledFill(&dispL, 0, 1);
  oledFill(&dispR, 0, 1);

  draw_MAIN();

  STATE = MAIN;

  // do wakeUp
  run_wakeUp = true;
  wakeUp();

}


//------------------------------------------------------//
//
//                    LOOP
//
//-----------------------------------------------------//


void loop() {

  // do wakeUp?
  if (run_wakeUp) { wakeUp(); }

  key = tenKey.getKey(); 

  if (key) {

    idleTimer = millis();
    
    switch (STATE) {
      case BEGIN:           transitionTo(MAIN);   break;
      case MAIN:            UNIT_CONVERSION();    break;
      case SETTINGS_MAIN:   settings_menu();      break;
      case SETTINGS_MORE:   more_menu();          break;
      case SETTINGS_RST:    fact_reset_menu();    break;
      case UNITS_MAIN:      units_menu();         break;
      case UNITS_OVERWRITE: confirm_overwrite();  break;
      case UNITS_UNIT1:     set_unit1();          break;
      case UNITS_UNIT2:     set_unit2();          break;
      case UNITS_CONVERT:   set_convert();        break;
    }

  }

  // do Sleep?
  if (checkIdle() || checkLowBattery() || checkPwrOnLowBatt() || checkForceSleep() || checkFactoryReset() ) {
    resetKeyState();
    sleep();
  }

  delay(5);  // keep a pace
}

/////////////////////// NOTES ///////////////////////

/* --EEPROM LOCATIONS--
0-4:      INIT / RESET
5:        -buffer-
6-20      bools, bytes
21:       -buffer-
22-98     arrays, charstrings
99:       -buffer-
100-851:  UNITS_LIST  
851-860:  -buffer-
860-1023: empty
/*/



/////////////////////// COMMENT TEMPLATES ///////////////////////

// Keypad Setup ------------------------------
// Display Setup -----------------------------
/////////////////////// KEYPAD SETUP ///////////////////////
/////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------//
//
//                    xxxxxxx
//
//-----------------------------------------------------//