
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

/* STANDARDS

#define uses whateverIt_Wants
functions are all camelCase()
enums are ALL_CAPS
structs are ALL_CAPS
variables are all camelCase
  unless its a continued type which would use alt_camelCase

/*/

/*
// KNOWN ISSUES:: 

not sure if EWMA filter is weighted correctly
code doesnt handle small numbers well. make some way for very small numbers to use scientific notation?
code doesnt save all values when turning off yet! implement that soon

sleep and reset work better now, but are contingent upon resetting the keypress state before sleep
not sure if delay() in EEPROM function actually helps
seems like UNITS_LIST length > 30 (50 tested) causes eeprom to fail. maybe causes dynamic memory usage to go too high?

/*/

/*
// STUFF TO DO:: 

slow down battery check time. currently checks too often
fix small number handling. use scientific notation instead of printing 0?
implement fractional input?
do dec to frac conversion?
consolodate serial.print fucntions
move both displays to bit-bang i2c. easier to pinout on breadboard
remove battery display from main screen?

/*/


#include <Keypad.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ss_oled.h>  // DONT FORGET TO DISABLE FONT_LARGE IF NOT IN USE
#include <BitBang_I2C.h>
#include <LowPower.h>

/////////////////////// ENABLE/DISABLE SERIAL MONITOR ///////////////////////

#define VIEW 0   // used for viewing typical output of display, can replace OLEDS
#define DEBUG 0  // used for debugging - typically lower level code
#define EPRM 0  // view EEPROM output to serial monitor

#if VIEW == 1
#define view(x) Serial.print(x)
#define viewln(x) Serial.println(x)
#define viewF(x) Serial.print(x, 6)   // prints floats
#define viewS Serial.print("      ")  // prints spaces
#else
#define view(x)
#define viewln(x)
#define viewF(x)
#define viewS
#endif

#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define debugF(x) Serial.print(x, 6)
#define debugS Serial.print("      ")
#else
#define debug(x)
#define debugln(x)
#define debugF(x)
#define debugS
#endif

#if VIEW == 1 || DEBUG == 1
#define startSerial Serial.begin(9600)
#define serial_print //serialPrint()
#define serial_print_reset //serialPrintReset()
#else
#define startSerial
#define serial_print
#define serial_print_reset
#endif

#if EPRM == 1
#define print_EEPROM printEEPROM()
#else
#define print_EEPROM
#endif

/////////////////////// INITIALIZE KEYPAD ///////////////////////

#define rows 5
#define cols 4
#define long_press_time 1000  // hold key for 1 seconds for long press to enter menus

char keys[rows][cols] = {
  { 'D', 'C', 'B', 'A' },
  { 'u', '3', '2', '1' },
  { 'd', '6', '5', '4' },
  { 'S', '9', '8', '7' },
  { 'E', '.', '0', 'l' }
};

uint8_t pin_r[rows] = { 11, 10, 9, 8, 7 };  // pin assignment on board
uint8_t pin_c[cols] = { 6, 5, 4, 3 };

Keypad tenKey = Keypad(makeKeymap(keys), pin_r, pin_c, rows, cols);

/////////////////////// INITIALIZE DISPLAYS ///////////////////////

// left display
#define displaySize_L OLED_128x64
#define SDA_L 0xC4          // default pro mini SDA pin   -- or -1
#define SCL_L 0xC5          // default SCL pin            -- or -1
#define I2C_Address_L 0x3C  // oled display I2C address   -- or -1
#define flipDisplay_L 0     // dont flip the diplay
#define invertDisplay_L 0   // dont invert the display
#define USE_HW_I2C_L 1      // use hardware I2C

// right display
#define displaySize_R OLED_128x64
#define SDA_R 0xC0          // A0/D14 Pin
#define SCL_R 0xC1          // A1/D15 Pin
#define I2C_Address_R 0x3C  // oled display I2C address   -- or -1
#define flipDisplay_R 0     // dont flip the diplay
#define invertDisplay_R 0   // dont invert the display
#define USE_HW_I2C_R 0      // use bitbang'd software I2C

#define displayResetPin 0xC6  // -1 is no reset pin -- 0xC6 is pro mini RST pin

// initialize display structs
SSOLED dispL, dispR;

///////////////////////////// ENUMS /////////////////////////////

enum DEVICE_STATES {
  BEGIN,
  MAIN,
  SETTINGS_MAIN,
  SETTINGS_SYS,
  SETTINGS_RST,
  UNITS_MAIN,
  UNITS_OVERWRITE,
  UNITS_UNIT1,
  UNITS_UNIT2,
  UNITS_CONVERT,
} STATE;

char key;

//------------------------------------------------------//
//
//           EEPROM & MEMORY FUNCTIONS
//
//-----------------------------------------------------//

#define unitStringLength 5  // max characters = 4. add +1 for null terminator '/0'
#define LIST_SIZE 30        // recommended values: 10-50. Max value: 60 before end of EEPROM

bool factoryResetFlag = false;
bool forceSleepFlag = false;


// UNIT MATRIX
struct UNITS_STRUCT {
  char unit1[unitStringLength];
  char unit2[unitStringLength];
  uint8_t special;
  float conversion;
};

// SETTINGS PROTOTYPES
uint8_t currentUnit;
bool longFormat;
uint8_t nextCustomIndex;      // index of next custom unit
bool conversionDirection[4];  // true is multiply, false is divide
int8_t activeUnitsIndex[4];  
uint8_t sleepSetting;
UNITS_STRUCT activeUnits[4];

/*
--EEPROM LOCATIONS--
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

#define EE_INIT 2  // INIT / RESET byte

#define EE_currentUnit 6    // 1
#define EE_longFormat 8     // 1
#define EE_sleepSetting 10  // 1
#define EE_nextCustIdx 12   // 1

#define EE_activeUnitsIdx 22   // 4 --> 22,23,24,25,-26-
#define EE_conversionDir 27    // 4 --> 27,28,29,30,-31-
#define EE_formatSelection 32  // 3 --> 32,33,34,-35-

#define EE_UNITS_LIST 100  // 10min=150, 50max=750, each entry 15 bytes
#define writeHoldTime 4

void EEPROM_INIT() {

  if (EEPROM[EE_INIT] == 255) {

    clearEEPROM();

    uint8_t default_currentUnit = 0;
    bool default_longFormat = false;
    uint8_t default_activeUnitsIndex[4] = { 0, 7, 3, 2 };  // CHOOSE FROM DEFAULT_UNITS_LIST
    bool default_conversionDirection[4] = { 1, 1, 1, 1 };  // true is multiply, false is divide

    uint8_t default_sleepSetting = 1;  // 30 seconds
    uint8_t default_nextCustomIndex = 8; // 1+ num indexes in DEFAULT_UNITS_LIST

    UNITS_STRUCT DEFAULT_UNITS_LIST[LIST_SIZE] = {
      { "  in", "  mm", 0, 25.4 },     // 0   ---- 1 unit1 = 1 unit2 * conversion
      { "  yd", "  ft", 0, 3.0 },      // 1
      { "  kg", " lbf", 0, 2.2 },      // 2
      { "knot", " mph", 0, 1.15078 },  // 3
      { " ksi", " mPa", 0, 6.89476 },  // 4
      { "  Nm", "lbft", 0, .7376 },    // 5
      { "smol", " big", 0, 91684 },    // 6
      { "diam", "area", 4, 1.0 }       // 7
    };

    EEPROM.put(EE_currentUnit, default_currentUnit);
    delay(writeHoldTime);
    EEPROM.put(EE_longFormat, default_longFormat);
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
    viewln("EEPROM UNTOUCHED #2");
  }
}

void clearEEPROM() {
  for (uint16_t i = 0; i < EEPROM.length(); i++) {
    // EEPROM[i] = 255;
    EEPROM.update(i, 255);  // less destructive while I test
    delay(writeHoldTime);
  }
}

void printEEPROM() {
  debugln("START PRINT");
  for (int i = 0; i < 50; i++) {
    //EEPROM.read(i);
    //debugln(EEPROM.read(i));
    // delay(30);
  }

  debugln("--------------------");
  debugln(EEPROM[EE_longFormat]);
  debugln(EEPROM[EE_sleepSetting]);
  debugln(EEPROM[EE_currentUnit]);
  debugln(EEPROM[EE_nextCustIdx]);
  //debugln(EEPROM[EE_activeUnitsIdx]);
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

void saveSettings() {
  debugln("SETTINGS SAVED!");
  EEPROM.put(EE_longFormat, longFormat);
  EEPROM.put(EE_sleepSetting, sleepSetting);

  print_EEPROM;
}

void getSettings() {

  EEPROM.get(EE_currentUnit, currentUnit);
  EEPROM.get(EE_longFormat, longFormat);
  EEPROM.get(EE_sleepSetting, sleepSetting);
  EEPROM.get(EE_conversionDir, conversionDirection);
  EEPROM.get(EE_activeUnitsIdx, activeUnitsIndex);
 
  getActiveUnits();
}

void getActiveUnits() {
  // this is much more space efficient than pulling the entrire UNITS_LIST struct from eeprom
  // not sure if theres a better way to index thru the eeprom.get than this
  unsigned int location0 = EE_UNITS_LIST + (sizeof(activeUnits[0]) * activeUnitsIndex[0]);
  unsigned int location1 = EE_UNITS_LIST + (sizeof(activeUnits[0]) * activeUnitsIndex[1]);
  unsigned int location2 = EE_UNITS_LIST + (sizeof(activeUnits[0]) * activeUnitsIndex[2]);
  unsigned int location3 = EE_UNITS_LIST + (sizeof(activeUnits[0]) * activeUnitsIndex[3]);

  EEPROM.get(location0, activeUnits[0]);
  EEPROM.get(location1, activeUnits[1]);
  EEPROM.get(location2, activeUnits[2]);
  EEPROM.get(location3, activeUnits[3]);
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

  EEPROM[EE_INIT] = 255; // this resets the EEPROM INIT location and forces a factory reset
  delay(200);
  EEPROM_INIT();
  delay(200);
  getSettings();
  delay(300);
}

//------------------------------------------------------//
//
//               SLEEP / WAKE FUNCTIONS
//
//-----------------------------------------------------//

#define wakeUpPin 2  // hardware interrupt INT0
#define wakeUpPin_GND 12
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
    return 1;
  } else {
    return 0;
  }
}

void sleep() {  // saves settings and puts the device to sleep
  // saves settings to EEPROM
  // puts the device to sleep


  viewln("*****POWER DOWN******");
  delay(1000);

  oledPower(&dispL, 0);
  oledPower(&dispR, 0);

  // comment these out to save values on sleep
  resetCurrentValue(); 
  doMath();
  draw_Values(); 
  
  pinMode(wakeUpPin_GND, OUTPUT);
  digitalWrite(wakeUpPin_GND, LOW);

  attachInterrupt(digitalPinToInterrupt(wakeUpPin), wakeUp_ISR, LOW);
  LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
  detachInterrupt(digitalPinToInterrupt(wakeUpPin));

}

void wakeUp_ISR() {  // Interrupt Service Routinge
  // wakeUp() is too slow to run as ISR, so flag it with run_wakeUp. detachInterrupt also too slow to be in ISR; move it elsewhere
  run_wakeUp = true;
}

void wakeUp() {  // wake the device up, reset timers, initialize displays
  // called by interrupt :: wakes the device up
  // turns off interrupt button, turns on the displays, continues back to loop

  viewln("*****WAKE UP******");

  pinMode(wakeUpPin_GND, INPUT);  // turn interrupt button off

  run_wakeUp = false;  // reset run_wakeUp

  idleTimer = millis();  // reset timers
  battTimer = 0;

  initVoltage();  // initBattVoltage?

  // mayube theres a cleaner way to do this? power down could put the device in SETUP mode? which automatically triggers STATE=MAIN?
  // this is really only to catch first power on and accidental power off in menu (10 mins)
  if (STATE != MAIN) {  // clear display and start normal mode
    STATE = MAIN;
    draw_MAIN();
  }

  oledPower(&dispL, 1);  // turn displays on
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
#define volt_divider_R1 100.0  // kilo Ohms :: 100 kO
#define volt_divider_R2 100.0

#define battCheckDelayTime 300      // only check battery every 5 seconds! does less float math and lets battery voltage average out
// #define skipCheckTime 60000           // skip the battery check if idleTime is less than 1 min
#define voltage_correction_factor 99  // (real voltage / analog read voltage) * 100  -----> 93.6% = 94//////////////////////////////////////////////////////////////////////dont forget to finish this
#define weight 20                     // Weight for EWMA :: 0 - 100. Higher values add more weight to newer readings

unsigned int batteryVoltage;
unsigned int prev_batteryVoltage = 360;  // set to 3.3V as a rough bias correction for EWMA filter
unsigned int warningVoltage = 330;       // centi-Volt :: 330 = 3.30V
unsigned int shutdownVoltage = 310;
bool warningVoltageFlag = false; // not necessary, but nice to have so low battery warning doesnt flicker

//  NOT SURE IF ILL USE EMWA OR NOT!
void getBatteryVoltage() {  // get current battery voltage using Exp Weighted Moving Average :: 3.375V ---->  338

  batteryVoltage = rawVoltage() * 100.0;  // convert float voltage to 3 digit cV output :: 4.235645 ---->  424

  debugF(rawVoltage());
  debug("         ");
  debug(batteryVoltage);
  debug("         ");

  batteryVoltage = weight * batteryVoltage + (100 - weight) * prev_batteryVoltage;
  batteryVoltage /= 100;

  prev_batteryVoltage = batteryVoltage;

  debugln(batteryVoltage);
}

float rawVoltage() {  // compute the raw voltage as float from analog pin, does voltage divider math
  return ((volt_divider_R1 + volt_divider_R2) / volt_divider_R2) * analogRead(voltageSensePin) * (refVoltage / 1024.0) * (voltage_correction_factor / 100.0);
}

void initVoltage() {  // initializes the EWMA by computing voltage 10 times
  for (uint8_t i = 0; i < 10; i++) {
    getBatteryVoltage();
  }
}

bool checkLowBattery() {  // checks for low voltage or warning voltage

  if (millis() - battTimer > battCheckDelayTime) {

    battTimer = millis();

    getBatteryVoltage();

    draw_BatteryStatus();   // This will live-update the battery voltage in the settings menu. Comment out to only load battery status once per entry
    draw_VoltageOnMain();   // Show voltage on MAIN state. Uncomment to enable

    //view("Volts:   ");
    //viewln(batteryVoltage);

    if (batteryVoltage < shutdownVoltage) {
      //viewln("BATTERY LOW - POWER DOWN");
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
// char key;
char currentValue[inputLength];
uint8_t currentValueIndex = 0;
bool containsRadix = false;
bool secondRadixFlag = false;
uint8_t sigFigs = 0;

// OUTPUT
char result_char[outputLength];
float result_float;

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

void invertConversion() {  // changes conversion direction for selected unit and stores in array to be saved
  conversionDirection[currentUnit] = !conversionDirection[currentUnit];
}

void setCurrentValue() {  // adds key to next index of currentValue char array

  if (key == '.' && containsRadix) { //return if theres already a radix present
    secondRadixFlag = true;  
    return; 
  }  

  if (checkOverflow()) {
    if (key == '.') { containsRadix = true; }     //set contains radix. used to flag return and change checkOverflow length
    currentValue[currentValueIndex] = key;
    currentValueIndex++;
    if (containsRadix && (key != '.')) {  // increments SigFigs if there is a radix, but not if the last key pressed was radix
      sigFigs++;
    }
  }
}

void undoRadixOnMenu() {  // bonus function to delete the unwanted radix that is created when you enter the menu using the '.' key

  if (!checkOverflow()) { return; } // do nothing if input is at max length

  if (secondRadixFlag) { // do nothing if input already has radix
    secondRadixFlag = false;
    return;
  }

  containsRadix = false;
  currentValueIndex--;
  currentValue[currentValueIndex] = '\0';

  draw_Values();
}

void resetCurrentValue() {                  // resets current value and reinitializes formatting flags
  memset(currentValue, '\0', inputLength);  //reset currentValue to null
  currentValueIndex = 0;
  containsRadix = false;
  sigFigs = 0;
  serial_print_reset;
}

bool checkOverflow() {  // checks to see if a new number would overflow the currentValue string. if so, do not add the number
  // output TRUE if string length has not overflown
  // output FALSE  if string length has overflown (if string length has reached max string length)
  uint8_t allowedDigits;
  if (containsRadix) {
    allowedDigits = inputLength - 1;
  } else {
    allowedDigits = inputLength - 2;
  }

  if (strlen(currentValue) == (allowedDigits)) {  // Detect full string
    return 0;
  } else {
    return 1;
  }
}

void doMath() {  // do multiply/divide or do special function

  if (!activeUnits[currentUnit].special) {  // if special = 0

    if (conversionDirection[currentUnit]) {
      result_float = atof(currentValue) * activeUnits[currentUnit].conversion;
    } else {
      result_float = atof(currentValue) / activeUnits[currentUnit].conversion;
    }

  } else {
    specialFunctions(activeUnits[currentUnit].special);
  }

  resultChar();  // compute and format result
  serial_print;
}

void resultChar() {  // do all output formatting of result
  //resultChar handles the output formatting -- taking in the output float and displaying a result max 6 digits long or scientific if necessary

  long leadingDigits = (long)result_float;
  bool longFormat_Override = longFormat;  // there to override user selected short/long format. can remove if I dont want the option to choose

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
//             SPECIAL MATH FUNCTIONS
//
//-----------------------------------------------------//

void specialFunctions(uint8_t special) {

  debugln("SPECIAL FUNCTION USED");

  switch (special) {
    case 4:  // diameter -> area
      if (conversionDirection[currentUnit]) {
        result_float = (3.14159) * atof(currentValue) * atof(currentValue) / 4;
      } else {
        result_float = sqrt(4 * atof(currentValue) / 3.14159);
      }
      break;
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

void serialPrint() {  // print input, result, and units to serial monitor

  if (conversionDirection[currentUnit]) {  // true is multiply
    view(currentValue);
    view(" ");
    view(activeUnits[currentUnit].unit1);
    view("  --->   ");
    view(result_char);
    view(" ");
    view(activeUnits[currentUnit].unit2);
    view("  ");
    viewF(result_float);
    viewln();
  } else {
    view(currentValue);
    view(" ");
    view(activeUnits[currentUnit].unit2);
    view("  --->   ");
    view(result_char);
    view(" ");
    view(activeUnits[currentUnit].unit1);
    view("  ");
    viewF(result_float);
    viewln();
  }
}

void serialPrintReset() {  // indicate value has been reset
  view(currentValue);
  viewln("*");
}

// MAIN ------------------------------

void draw_MAIN() {  // clears displays and goes back to MAIN routine
  clearDisplays();
  doMath();
  draw_Values();
  draw_Units();
}

void draw_Values() {  // updates left and right displays with currentValue and result_char

  if (currentValueIndex == 1 || currentValueIndex == 0) {
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
  draw_BatteryStatus(); // empty
  drawBanner("SLEEP", "FORMAT", "SYSTEM", "SAVE");
}

void draw_SystemSettings() {
  clearDisplays();
  draw_BatteryStatus();
  //drawBanner_systemSettings();
  oledWriteString(&dispL, 0, bannerX1, bannerY-1, "POWER", BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, bannerX1, bannerY-1, "FACTORY", BANNER_FONT, 0, 1);
  drawBanner("DOWN", "   ", "RESET", "BACK");
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

void draw_FactoryReset() {  //////////////// this text is werid
  clearDisplays();
  oledWriteString(&dispL, 0, numX, 2, "ARE YOU SURE?", BANNER_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 1, "This will reset all", SMALL_FONT, 0, 1);
  oledWriteString(&dispR, 0, numX, 2, "settings and wll", SMALL_FONT, 0, 1);               //////////////// this line is weird
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

void draw_SelectedUnit() {

  unsigned int location = EE_UNITS_LIST + ( sizeof(activeUnits[0]) * activeUnitsIndex[currentUnit] );

  EEPROM.get(location, activeUnits[currentUnit]);

  // viewln(activeUnits[currentUnit].unit1);
  // viewln(activeUnits[currentUnit].unit2);

  if (activeUnitsIndex[currentUnit] != nextCustomIndex) {
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
  //oledWriteString(&dispL, 0, 30+33, 4, itoa(nextCustomIndex+1, buffer, 10), SETTINGS_FONT, 0, 1);
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
  if (STATE == SETTINGS_MAIN || STATE == SETTINGS_SYS) {
    char buffer[4];
    itoa(batteryVoltage, buffer, 10);

    //char batt_char[11] = {buffer[0], '.', buffer[1], buffer[2], 32, 'V', 'o', 'l', 't', 's', 0};
    char batt_char[6] = {buffer[0], '.', buffer[1], buffer[2], 'V', 0};

    oledWriteString(&dispR, 0, 85, 0, batt_char, SMALL_FONT, 0, 1);

    if (warningVoltageFlag) {
      oledWriteString(&dispR, 0, 0, 0, "LOW BATTERY", SMALL_FONT, 0, 1);
    } else {
      oledWriteString(&dispR, 0, 0, 0, "           ", SMALL_FONT, 0, 1);
    }
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
int8_t saveIndex;
//char usr_conversion[outputLength];
uint8_t usr_index = 0;

void initCustomUnits() {
  strcpy(usr_unit1, clearUnits);
  strcpy(usr_unit2, clearUnits);
  resetCurrentValue();
}

void selectUnit(bool direction) {
  // 0 is decrement
  // 1 is increment

  // increment or decrement
  if (direction) {
    activeUnitsIndex[currentUnit]++;
  } else {
    activeUnitsIndex[currentUnit]--;
  }

  // loop around available units
  if (activeUnitsIndex[currentUnit] > nextCustomIndex) {
    activeUnitsIndex[currentUnit] = 0;
  } else if (activeUnitsIndex[currentUnit] < 0) {
    activeUnitsIndex[currentUnit] = nextCustomIndex;
  }

  //viewln(activeUnitsIndex[currentUnit]);

}

void saveNewUnit() {

  // if (atof(currentValue) == 0) { // cancel if user conversion is zero. Zero will break the code (divide by zero)
  //   activeUnitsIndex[currentUnit] = saveIndex;
  //   return;
  // }

  UNITS_STRUCT NEW_UNIT;
  strcpy(NEW_UNIT.unit1, usr_unit1);
  strcpy(NEW_UNIT.unit2, usr_unit2);
  NEW_UNIT.special = 0;
  NEW_UNIT.conversion = atof(currentValue);

  // replace activeUnits with new unit
  activeUnits[currentUnit] = NEW_UNIT;
  conversionDirection[currentUnit] = 1; // set conversion direction to match default

  if (activeUnitsIndex[currentUnit] == nextCustomIndex){ // increment ONLY if changed unit is at end of list
    nextCustomIndex++;
    if (nextCustomIndex > LIST_SIZE) {
      nextCustomIndex = LIST_SIZE;
    }
  }

  EEPROM.put(EE_nextCustIdx, nextCustomIndex);
  EEPROM.put(EE_activeUnitsIdx, activeUnitsIndex);

  unsigned int location = EE_UNITS_LIST + ( sizeof(activeUnits[0]) * activeUnitsIndex[currentUnit] );
  EEPROM.put(location, NEW_UNIT);

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
  if (currentValueIndex == 1) {
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
      // viewln("MAIN");
      STATE = MAIN;
      draw_MAIN();
      break;

    case SETTINGS_MAIN:
      // viewln("SETTINGS");
      STATE = SETTINGS_MAIN;
      draw_SettingsMenu();
      break;

    case SETTINGS_SYS:
      // viewln("SYSTEM");
      STATE = SETTINGS_SYS;
      draw_SystemSettings();
      break;

    case SETTINGS_RST:
      // viewln("FACTORY RESET");
      STATE = SETTINGS_RST;
      draw_FactoryReset();
      break;

    case UNITS_MAIN:
      // viewln("UNITS - CALLED");
      STATE = UNITS_MAIN;
      EEPROM.get(EE_nextCustIdx, nextCustomIndex);
      draw_UnitsMenu();
      break;
    
    case UNITS_OVERWRITE:
      // viewln("NEW");
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

void enter_menus(KeypadEvent key) {  // KeypadEvent handler for detecting long press - built in to keypad.h

  // viewln("EVENT HANDLER CALLED");
  // viewln(tenKey.getState());

  if (tenKey.getState() == HOLD && STATE == MAIN) {
    switch (key) {
      case 'A': case 'B': case 'C': case 'D':
        transitionTo(UNITS_MAIN);
        break;

      case '.':
        undoRadixOnMenu();
        transitionTo(SETTINGS_MAIN);
        break;
    }
  }
}

void settings_menu() {  // change settings in the SETTINGS_MAIN, display results
  switch (key) {

    case 'A':  // SLEEP SETTINGS
      //viewln("SLEEP TIMER");
      sleepSetting++;
      if (sleepSetting > 5) { sleepSetting = 0; }
      draw_SleepSetting();
      break;

    case 'B':  // NUMBER FORMAT SETTING
      //viewln("FORMAT");
      longFormat = !longFormat;
      draw_FormatSelection();
      break;

    case 'C':  // SYSTEM SETTINGS
      //viewln("SYSTEM");
      transitionTo(SETTINGS_SYS);
      break;

    case 'D':  // EXIT TO MAIN
      //viewln("EXIT");
      saveSettings();
      transitionTo(MAIN);
      break;
  }
}

void system_menu() {
  switch (key) {

    case 'A':  // POWER OFF
      forceSleepFlag = true;
      //sleep();
      break;

    case 'B':  // unused for now
      // viewln("BATTERY FORMAT");
      // //longFormat = !longFormat;
      // draw_BatteryFormat();
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
      // viewln("YES");
      draw_ResetConfirm();
      factoryResetFlag = true;
      // factoryReset();
      break;

    case 'B': case 'C': break; // unused

    case 'D':  // NO
      // viewln("NO");
      transitionTo(SETTINGS_SYS);
      break;
  }
}

void units_menu() {  // change units in the UNITS_MAIN, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      // viewln("DECREMENT");

      selectUnit(0);
      draw_SelectedUnit();
      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      // viewln("INCREMENT");

      selectUnit(1); 
      draw_SelectedUnit();
      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      // viewln("NEW");

      saveIndex = activeUnitsIndex[currentUnit];

      if (activeUnitsIndex[currentUnit] == nextCustomIndex) {
        initCustomUnits();
        transitionTo(UNITS_UNIT1);
      } else {
        transitionTo(UNITS_OVERWRITE);
      }

      break;

    case 'D':  // EXIT TO MAIN MENU
      // viewln("SAVE");

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
      // viewln("YES");
      initCustomUnits();
      transitionTo(UNITS_UNIT1);
      break;

    case 'B': case 'C': break; // unused

    case 'D':  // NO
      // viewln("NO");
      transitionTo(UNITS_MAIN);
      break;
  }
}

void set_unit1() {  // change units in UNITS_UNIT1, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      viewln("DOWN LETTER");

      usr_letter--;
      if (usr_letter < 32) { usr_letter = 126; }

      Serial.println(usr_letter);
      Serial.println((int)usr_letter);

      usr_unit1[usr_index] = usr_letter;

      //oledWriteString(&dispL, 0, 40, 2, usr_unit1, UNIT_FONT, 0, 1);
      draw_UserUnit1();
      //draw_UserUnitIndicator();
      //oledWriteString(&dispL, 0, 40, 2, usr_unit1, UNIT_FONT, 0, 1);


      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      // viewln("UP LETTER");
      
      usr_letter++;
      if (usr_letter > 126) { usr_letter = 32; }

      Serial.println(usr_letter);
      Serial.println((int)usr_letter);

      usr_unit1[usr_index] = usr_letter;

      //oledWriteString(&dispL, 0, 40, 2, usr_unit1, UNIT_FONT, 0, 1);
      draw_UserUnit1();
      //draw_UserUnitIndicator();


      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      // viewln("NEXT INDEX");
      
      usr_letter = 96;
      usr_index++;
      Serial.println(usr_index);
      if (usr_index > 3){
        usr_index = 0;
      }
      draw_UserUnitIndicator(1);

      break;

    case 'D':  // GO TO NEXT UNIT
      // viewln("UNITS2");
      transitionTo(UNITS_UNIT2);
      break;
  }
}

void set_unit2() {  // change units in UNITS_UNIT2, display results

  switch (key) {
    case 'A':  // INCREMENT DOWN STORED UNITS STRUCT
      // viewln("DOWN LETTER2");

      usr_letter--;
      if (usr_letter < 32) { usr_letter = 126; }

      Serial.println(usr_letter);
      Serial.println((int)usr_letter);

      usr_unit2[usr_index] = usr_letter;

      //oledWriteString(&dispL, 0, 40, 2, usr_unit1, UNIT_FONT, 0, 1);
      draw_UserUnit2();
      //draw_UserUnitIndicator();
      break;

    case 'B':  // INCREMENT UP STORED UNITS STRUCT
      // viewln("UP LETTER2");

      usr_letter++;
      if (usr_letter > 126) { usr_letter = 32; }

      Serial.println(usr_letter);
      Serial.println((int)usr_letter);

      usr_unit2[usr_index] = usr_letter;

      //oledWriteString(&dispL, 0, 40, 2, usr_unit1, UNIT_FONT, 0, 1);
      draw_UserUnit2();
      //draw_UserUnitIndicator();
      break;

    case 'C':  // RUN INPUT CUSTOM UNITS SCRIPT
      // viewln("NEXT INDEX2");
      usr_letter = 96;
      usr_index++;
      Serial.println(usr_index);
      if (usr_index > 3){
        usr_index = 0;
      }
      draw_UserUnitIndicator(0);
      break;

    case 'D':  // GO TO CONVERSION INPUT
      // viewln("CONVERT");
      transitionTo(UNITS_CONVERT);
      break;
  }
}

void set_convert() {  // change number in UNITS_CONVERT, display results

  switch (key) {
    case 'A': 
      viewln("SAVE");

      if (atof(currentValue) == 0) { // cancel if user conversion is zero. Zero will break the code (divide by zero)
        break;
      }

      saveNewUnit();
      resetCurrentValue();
      transitionTo(MAIN);     // dont forget to handle all zeros input!!!!!
      break;  
    case 'B':
      viewln("CANCEL");
      transitionTo(UNITS_MAIN);
      resetCurrentValue();
      break;

    case 'C':  // GO TO CONVERSION INPUT
      viewln("DELETE");
      resetCurrentValue();
      oledWriteString(&dispR, 0, numX, numY, clearNumbers, NUMBER_FONT, 0, 1);
      break;

    case 'D':  // LOOP BACK TO UNIT1
      viewln("BACK TO UNIT1");
      transitionTo(UNITS_UNIT1);
      break;

    case 'd': case 'l': case 'u': break;  // SKIP UNUSED KEYS
    case 'E': case 'S': break;  // not used

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
    case 'd': case 'l': case 'u': break;  // SKIP UNUSED KEYS
    case 'A': case 'B': case 'C': case 'D':
      setUnit();
      doMath();
      draw_Units();
      draw_Values();
      break;

    case 'E':
      draw_ResetValues();
      resetCurrentValue();
      break;

    case 'S':
      invertConversion();
      doMath();
      draw_Units();
      draw_Values();
      break;

    default:
      setCurrentValue();
      doMath();
      draw_Values();
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////////


//------------------------------------------------------//
//
//                    SETUP
//
//-----------------------------------------------------//

void setup() {

  STATE = BEGIN;

  startSerial;
  Serial.begin(9600);

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

  // initialize OLED displays
  // oledInit(SSOLED *, type, oled_addr, rotate180, invert, bWire, SDA_PIN, SCL_PIN, RESET_PIN, speed)
  oledInit(&dispL, displaySize_L, I2C_Address_L, flipDisplay_L, invertDisplay_L, USE_HW_I2C_L, SDA_L, SCL_L, displayResetPin, 400000L);  // 400kHz
  oledInit(&dispR, displaySize_R, I2C_Address_R, flipDisplay_R, invertDisplay_R, USE_HW_I2C_R, SDA_R, SCL_R, displayResetPin, 400000L);  // 400kHz
  oledFill(&dispL, 0, 1);
  oledFill(&dispR, 0, 1);

  // if (STATE == SETUP) {
  //   viewln("**************SETUP COMPLETE*****************");  
  // //   STATE = MAIN; 
  // // } else {
  // //   viewln("change to main");
  // //   viewln(STATE);
  // //   STATE = MAIN;
  // }

  // do wakeUp
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
      case SETTINGS_SYS:    system_menu();        break;
      case SETTINGS_RST:    fact_reset_menu();    break;
      case UNITS_MAIN:      units_menu();         break;
      case UNITS_OVERWRITE: confirm_overwrite();  break;
      case UNITS_UNIT1:     set_unit1();          break;
      case UNITS_UNIT2:     set_unit2();          break;
      case UNITS_CONVERT:   set_convert();        break;
    }

  }

  // do Sleep?
  if (checkIdle() || checkLowBattery() || checkForceSleep() || checkFactoryReset()) {
    resetKeyState();
    sleep();
  }

  delay(50);  // keep a pace
}




