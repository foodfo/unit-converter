
// Keypad Setup ------------------------------
// Display Setup -----------------------------
/////////////////////// KEYPAD SETUP ///////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------//
//
//                    xxxxxxx
//
//-----------------------------------------------------//


#include <Keypad.h>
#include <Wire.h>
#include <ss_oled.h>           // DONT FORGET TO DISABLE FONT_LARGE IF NOT IN USE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
#include <BitBang_I2C.h> 
#include <LowPower.h>

/////////////////////// INITIALIZE KEYPAD ///////////////////////

#define rows 5
#define cols 4

char keys[rows][cols] = {
  {'D','C','B','A'},
  {'u','3','2','1'},
  {'d','6','5','4'},
  {'S','9','8','7'},
  {'E','.','0','l'}
};

uint8_t pin_r[rows] = {11,10,9,8,7}; // pin assignment on board
uint8_t pin_c[cols] = {6,5,4,3};

Keypad tenKey = Keypad(makeKeymap(keys), pin_r, pin_c, rows, cols);

/////////////////////// INITIALIZE DISPLAYS ///////////////////////

// left display
#define displaySize_L OLED_128x64
#define SDA_L 0xC4            // default pro mini SDA pin   -- or -1
#define SCL_L 0xC5            // default SCL pin            -- or -1
#define I2C_Address_L 0x3C    // oled display I2C address   -- or -1
#define flipDisplay_L 0       // dont flip the diplay
#define invertDisplay_L 0     // dont invert the display
#define USE_HW_I2C_L 1        // use hardware I2C

// right display
#define displaySize_R OLED_128x64
#define SDA_R 0xC1            // A0/D14 Pin  
#define SCL_R 0xC0            // A1/D15 Pin
#define I2C_Address_R 0x3C    // oled display I2C address   -- or -1                 
#define flipDisplay_R 0       // dont flip the diplay
#define invertDisplay_R 0     // dont invert the display
#define USE_HW_I2C_R 0        // use bitbang'd software I2C

#define displayResetPin -1    // -1 is no reset pin

// initialize display structs
SSOLED dispL;   
SSOLED dispR;

//////////////////////////////////////////////////////////////////////////

enum modes {
  ROUTINE,
  UNITS_MENU,
  SETTINGS_MENU
} DEVICE_MODE;


/////////////////////////////////////////////////////////////////////////////


//------------------------------------------------------//
//
//           EEPROM & MEMORY FUNCTIONS
//
//-----------------------------------------------------//

#define unitStringLength 5 // max characters = 4. add +1 for null terminator '/0'

// SAVED SETTINGS - current selections
uint8_t selectedUnit = 0;
bool conversionDirection[4] = {1,1,1,1};   // true is multiply, false is divide
uint8_t activeUnitsIndex[4] = {0,1,5,6};   // MODIFY THIS ARRAY TO CHOOSE UNITS FROM STORED UNITS
bool longFormat = false;                   // false is short format, true is long format. short format is default

// UNIT MATRIX
struct UNITS_STRUCT {
  char unit1[unitStringLength];       
  char unit2[unitStringLength];
  float conversion;
};

UNITS_STRUCT storedUnits[30] = {  // dont forget to add a 0 if the user doesnt add a .0 to the end. otherwise math wont be float?  also, add leading spaces to units for formatting
  {"  in", "  mm", 25.4},     // 0   ---- 1 unit1 = 1 unit2 * conversion
  {"  yd", "  ft", 3.0},      // 1
  {"  kg", " lbf", 2.2},      // 2
  {"knot", " mph", 1.15078},  // 3
  {" ksi", " mPa", 6.89476},  // 4
  {"  Nm", "lbft", .7376},    // 5
  {"smol", " big", 91684}     // 6
};

//store values from eeprom locally for speed
UNITS_STRUCT activeUnits[4] = {
  storedUnits[activeUnitsIndex[0]],
  storedUnits[activeUnitsIndex[1]],
  storedUnits[activeUnitsIndex[2]],
  storedUnits[activeUnitsIndex[3]]
};


//------------------------------------------------------//
//
//               SLEEP / WAKE FUNCTIONS
//
//-----------------------------------------------------//

#define wakeUpPin 2       // hardware interrupt INT0
#define wakeUpPin_GND 12

unsigned long idleTime = millis();
volatile bool run_wakeUp;

unsigned long checkBattTime = millis();

struct power_settings {
  unsigned long
}

uint8_t sleepSelection = 2;
unsigned long sleepTimer[] = { // time in ms before idle device goes to sleep
  10000,        // 0: 10s
  30000,        // 1: 30s
  60000,        // 2: 1 min
  120000,       // 3: 2 min
  300000,       // 4: 5 min
  600000,       // 5: 10 min
  //4000000000,   // 6: NEVER (1111 hours functionally never)
};

bool checkTimeForSleep(){
  // returns TRUE for time to put device to sleep. Happens if you go too long without pressing an input
  // returns FALSE for stay awake

  // Serial.print(idleTime);
  // Serial.print("        ");
  // Serial.println(millis());
  if(millis() - idleTime > sleepTimer[sleepSelection]){    
    return 1;
  } else {
    return 0;
  }
}

void gotoSleep(){
  // saves settings to EEPROM
  // puts the device to sleep

  // oledFill(&dispL,0);
  // oledFill(&dispR,0);

  oledPower(&dispL,0);
  oledPower(&dispR,0);

  pinMode(wakeUpPin_GND,OUTPUT);
  digitalWrite(wakeUpPin_GND, LOW);

  Serial.println("*****POWER DOWN******");
  delay(100);

  attachInterrupt(digitalPinToInterrupt(wakeUpPin), wakeUp_ISR, LOW);
  LowPower.powerDown(SLEEP_FOREVER,ADC_OFF,BOD_OFF);
  detachInterrupt(digitalPinToInterrupt(wakeUpPin));

}

void wakeUp_ISR(){
  // wakeUp() is too slow to run as ISR, so flag it with run_wakeUp. detachInterrupt also too slow to be in ISR; move it elsewhere
  run_wakeUp = true;
}

void wakeUp(){
  // called by interrupt :: wakes the device up
  // turns off interrupt button, turns on the displays, continues back to loop
  // undimms displays if necessary
  // does math and prints result to "prime" the displays

  oledPower(&dispL,1); // turn displays on
  oledPower(&dispR,1);

  run_wakeUp = false; // reset run_wakeUp

  pinMode(wakeUpPin_GND,INPUT);

  Serial.println("*****WAKE UP******");

  // just after wake up, idle time is still high, so device would go right back to sleep in a death loop if keypad doesnt detect press for some reason
  idleTime = millis();  // reset idle time? n YOU SHOULD THIS ONLY WORKS BECAUSE WHEN YOU GO BACK TO LOOP KEYPAD DETECTS ENTER KEY PRESSED!
  checkBattTime = 0; // reset checkBattTime so it device can catch low voltage on startup. forces low battery check on first loop

  fillVoltageArr();

  DEVICE_MODE = ROUTINE;
  routine_Displays(); // currently needed so if gotosleep is called while still in a menu, the menu elements wont be on screen on startup. could fix this for the most part by temporarily forcing
  //checksleep millis counter to 10 minutes or something. then on exit, change the value back??? might be nice since you dont necessarily want the device sleepign every time you go to change settings

  //if(checkLowBattery()){gotoSleep();}

}

void dimDisplay(){    // NEED TO SEE IF DIM DISPLAY SAVES ANY POWER OR NOT BEFORE IMPLEMENTING!!!!
  // dimDisplay called whenever key pressed to undim display, or does it just set the DimDisplayFlag to be false??? or does dimDisplay take in a bool?
  // dimDisplay called every loop outside of key switch statement to determine if display should dim
  // dims display after XXXXX time --- roughly 30s before sleeping
  // sets dim flag true
  // checks if dim flag true when key pressed, then undims displayu
}

//------------------------------------------------------//
//
//              BATTERY MONITOR FUNCTIONS
//
//-----------------------------------------------------//

#define voltageSensePin A2
#define refVoltage 3.3
#define battCheckDelayTime 10000 // only check battery every 30 seconds! does less float math and lets battery voltage average out
#define volt_divider_R1 100.0    // kilo Ohms :: 100 kO
#define volt_divider_R2 100.0
#define num2Avg 5
#define voltage_correction_factor 94 // (real voltage / analog read voltage) * 100  -----> 93.6% = 94

float raw_batteryVoltage;
unsigned int battVolt_avg_arr[num2Avg];
unsigned int batteryVoltage;
uint8_t avg_index;

unsigned int warningVoltage = 330;   // centi-Volt :: 330 = 3.30V
unsigned int shutdownVoltage = 310;
//unsigned long checkBattTime = millis();    

void fillVoltageArr() {         // pre-fill rolling average array on startup/wakeup, otherwise you get artificially low values
  // Serial.println("-----------------------filling--------------------");
  // for(uint8_t i=0; i<num2Avg; i++){
  //   getBatteryVoltage();
  // }

  for(uint8_t i=0; i<num2Avg; i++){  // more efficient but repoduces code from below
    raw_batteryVoltage = ( (volt_divider_R1 + volt_divider_R2) / volt_divider_R2 ) * analogRead(voltageSensePin) * (refVoltage/1024.0) * (voltage_correction_factor / 100.0);
    batteryVoltage = raw_batteryVoltage * 100.0;   
    battVolt_avg_arr[i] = batteryVoltage;
  }
  // Serial.println("**************end filling***********************");
}

void getBatteryVoltage() {      // do rolling average

  // float  raw_voltage   =  [ voltage divider math ]  *  [ read voltage out from divider ]  *  [ convert 10bit ADC to voltage steps ]  *  [ apply correction factor as percentage ]
  raw_batteryVoltage = ( (volt_divider_R1 + volt_divider_R2) / volt_divider_R2 ) * analogRead(voltageSensePin) * (refVoltage/1024.0) * (voltage_correction_factor / 100.0);

  batteryVoltage = raw_batteryVoltage * 100.0;   // convert float voltage to 3 digit cV output :: 4.235645 ---->  424


  // IF I NEED MORE SPACE OR SPEED. DELETE EVERYTHIN BELOW THIS COMMENT AND REMOVE FILLVOLTAGEARR FROM SETUP AND WAKEUP

  battVolt_avg_arr[avg_index] = batteryVoltage;
  avg_index++;
  if (avg_index >= num2Avg) { avg_index = 0; }

  unsigned int avg = 0;    // for some reason you must initialize avg = 0 explicitly or the += in the for loop breaks

  // Serial.println("--------------");
  for (uint8_t i=0; i<num2Avg; i++){
    avg += battVolt_avg_arr[i];
    // Serial.print("Current Index: ");
    // Serial.print(i);
    // Serial.print("           Current Index Val:  ");
    // Serial.print(battVolt_avg_arr[i]);
    // Serial.print("           Current Average Val:  ");
    // Serial.println(avg);
  }
  //  Serial.println("--------------");

  batteryVoltage = avg / num2Avg;

  // Serial.print("AVG Volt:  ");
  // Serial.print(batteryVoltage);
  // Serial.print(" ----- RAW Volt:  ");
  // Serial.println(raw_batteryVoltage, 5);
}

bool checkLowBattery(){ 

 if(millis() - checkBattTime > battCheckDelayTime){ 

    checkBattTime = millis();
  
    getBatteryVoltage();

    if(batteryVoltage < shutdownVoltage){
        Serial.println("BATTERY LOW");
        return 1;
    } else if(batteryVoltage < warningVoltage){
        displayLowBatteryWarning(1);
        return 0;
    } else if(batteryVoltage > warningVoltage){
        displayLowBatteryWarning(0);
        return 0;
    } else {
        return 0;
    }

 } else {
   return 0;
 }
  // checks battery voltage for low voltage state to turn off device thru checkSleep
  // displays battery voltage on screen?
  // displays if battery voltage is low on screen (highlight? extra text?)
  // displays if battery critical
}

void displayLowBatteryWarning(bool print){
  // if (print) {
  //   oledWriteString(&dispL, 0,0,4, "LOW BATTERY", FONT_SMALL, 0, 1);
  // } else {
  //   oledWriteString(&dispL, 0,0,4, "           ", FONT_SMALL, 0, 1);
  // }
}



//------------------------------------------------------//
//
//               MATH FUNCTIONS
//
//-----------------------------------------------------//

#define inputStringLength 8    // max digits = 6. add +1 for decimal. add +1 for null terminator '/0'
#define outputStringLength 8
#define zeros "0.00000"        // zeroes output result for formatting. number of zeros = outputStringLength - 2

// INPUT 
char key;
char currentValue[inputStringLength];
uint8_t currentValueIndex = 0;
bool containsRadix = false;
uint8_t sigFigs = 0;

// OUTPUT
char result_char[outputStringLength];
float result_numeric;

void selectUnit () {            // sets current conversion to appropriate index in activeUnits struct
  if (key=='A'){
      selectedUnit = 0;
  } else if (key=='B'){
      selectedUnit = 1;
  } else if (key=='C'){
      selectedUnit = 2;
  } else {
      selectedUnit = 3;
  }

  //Serial.println(key);
  //Serial.print("Selected Unit Changed to: ");
  //Serial.println(selectedUnit);
}

void invertConversion(){        // changes conversion direction for selected unit and stores in array to be saved
  conversionDirection[selectedUnit] = !conversionDirection[selectedUnit];

  //Serial.print("Unti # ");
  //Serial.print(selectedUnit);
  //Serial.println(" inverted!");
}

void setCurrentValue(){         // adds key to next index of currentValue char array

  if(key == '.' && containsRadix){return;}   //return if theres already a radix present
  if(key == '.'){containsRadix = true;}      //set contains radix. used to flag return and change checkOverflow length

  if (checkOverflow()){
      currentValue[currentValueIndex] = key;
      currentValueIndex++;
      //ncrementIndex();
      if (containsRadix && (key!='.')){  // increments SigFigs if there is a radix, but not if the last key pressed was radix
      sigFigs++; 
      }
  }
}

void resetCurrentValue(){       // LATER ON ADD FUNCTIONALITY TO HIGHLIGHT CURRENT TEXT ON OLED TO SHOW ITS READY FOR DELETION
  memset(currentValue,'\0',inputStringLength);  //reset currentValue to null
  currentValueIndex=0;
  containsRadix=false;
  sigFigs=0;
}

bool checkOverflow(){           // checks to see if a new number would overflow the currentValue string. if so, do not add the number
  // output TRUE if string length has not overflown
  // output FALSE  if string length has overflown (if string length has reached max string length) 
 uint8_t allowedDigits;
 if (containsRadix){
   allowedDigits = inputStringLength - 1;
 } else {
   allowedDigits = inputStringLength - 2;
 }
 
 if (strlen(currentValue)==(allowedDigits)){ // Detect full string
    return 0;
 } else {
    return 1;
 }
}

void doMath(){                  // do multiply or do divide
  if(conversionDirection[selectedUnit]){
    result_numeric = atof(currentValue) * activeUnits[selectedUnit].conversion;
  } else {
    result_numeric = atof(currentValue) / activeUnits[selectedUnit].conversion;
  }
  resultChar(); // compute and format result         
  serialPrint();
}

void resultChar(){              // do all output formatting of result
  //resultChar handles the output formatting -- taking in the output float and displaying a result max 6 digits long or scientific if necessary
  //converting float output to char is necessary for clean max 6 digit formatting

  long leadingDigits = (long)result_numeric;
  bool longFormatOverride = longFormat;                          // there to override user selected short/long format. can remove if I dont want the option to choose

  // set 0.xxx = 1 leading digit and override to long format for precision
  if (leadingDigits == 0){
    leadingDigits = 1;
    longFormatOverride = true;
  }
  
  // count number of leading digits in result
  uint8_t countDigits = 0;
  while(leadingDigits){
    leadingDigits = leadingDigits/10;
    countDigits++;
  }  

  // determine output format; how many digits must be present at end of decimal -> depends on A: longFormat; B: number of sigfigs in input; C: number of leading digits in output
  uint8_t num_digits_after_decimal;
  int8_t remaining_digits_allowed = outputStringLength - countDigits - 2; // note that this value CAN go negative so cant be unsigned! subtract 2 for decimal and null terminator

  if (longFormat || longFormatOverride){                        // long formatting logic
      num_digits_after_decimal = remaining_digits_allowed;     
  } else {                                                      // short formatting logic
     if(remaining_digits_allowed > sigFigs) {
        num_digits_after_decimal = sigFigs + 1;
     } else if (remaining_digits_allowed >= 0) {
        num_digits_after_decimal = remaining_digits_allowed;
     } else {
        num_digits_after_decimal = 0;                           // this else statement is redundant since this line is implicit, but it improves clarity and shows design intent
     }
  }
  
  // format output: either scientific notation, or decimal notation
  if (countDigits > 6){ // do scientific notation  <-- consider improving this. currently output is only allowed 2 digits total.
    dtostre(result_numeric, result_char, outputStringLength-7, 0x04); // subtract 7: 1: /0, 2: '.', 3: '+/-', 4: 1st exp digit, 5: 2nd exp digit, 6: leading single digit, 7: 'E'
    //strcpy(result_char,"TOO BIG");
  } else { // do decimal notation
    dtostrf(result_numeric, 1, num_digits_after_decimal, result_char);
  }

  // format "0.00000" output to "0.0"
  if (!strcmp(result_char,zeros)) { // strcmp throws 0 when strings are the same
    strcpy(result_char, "0.0");
    }

  // fill remainder of result_char with the space character, 32 :: {'3','.','1','2',\0,\0,\0,\0} ---> {'3','.','1','2',32,32,32,\0}
  for(uint8_t j = strlen(result_char); j < outputStringLength-1; j++){  // this aids formatting since moving from strlen 7>6 wont delete old characters. this ensures all strlen are 7
    result_char[j] = ' ';
  }
}


//------------------------------------------------------//
//
//               DISPLAY FUNCTIONS
//
//-----------------------------------------------------//

#define NUMBER_SIZE FONT_LARGE
#define UNIT_SIZE FONT_STRETCHED
#define SETTINGS_SIZE FONT_NORMAL
#define numX 0
#define numY 0
#define unitX 60
#define unitY 4
#define clearUnits "    "         // {32,32,32,32,/0} 4 spaces char array :: reference unitStringLength 
#define clearNumbers "       "    // {32,32,32,32,32,32,32,/0} 7 spaces char array  :: reference inputStringLength & outputStringLength

void updateDisplayValues(){     // updates left and right displays with currentValue and result_char

  if(currentValueIndex == 1){
    oledWriteString(&dispL, 0,numX,numY, clearNumbers, NUMBER_SIZE, 0, 1);
  }

  oledWriteString(&dispL, 0,numX,numY, currentValue, NUMBER_SIZE, 0, 1);
  oledWriteString(&dispR, 0,numX,numY, result_char, NUMBER_SIZE, 0, 1);

}

void updateDisplayUnits(){      // updates left and right displays with correct units based on conversion direction

  if(conversionDirection[selectedUnit]){  // true is multiply; unit1 = unit2 * conversion; unit1 ---> unit2
    oledWriteString(&dispL, 0,unitX,unitY, activeUnits[selectedUnit].unit1, UNIT_SIZE, 0, 1);
    oledWriteString(&dispR, 0,unitX,unitY, activeUnits[selectedUnit].unit2, UNIT_SIZE, 0, 1);
  } else {
    oledWriteString(&dispL, 0,unitX,unitY, activeUnits[selectedUnit].unit2, UNIT_SIZE, 0, 1);
    oledWriteString(&dispR, 0,unitX,unitY, activeUnits[selectedUnit].unit1, UNIT_SIZE, 0, 1);
  }
}

void updateDisplayResetValues(){// highlights currentValue to show user it is ready to be overwritten
  oledWriteString(&dispL, 0,numX,numY, currentValue, NUMBER_SIZE, 1, 1);
}

void clearDisplays() {
  oledFill(&dispL, 0, 1);
  oledFill(&dispR, 0, 1);
}

void routine_Displays(){ // clears displays and goes back to math routine
  clearDisplays();
  doMath();
  updateDisplayValues();
  updateDisplayUnits();
}

void serialPrint() {            // print input, result, and units to serial monitor

  if (conversionDirection[selectedUnit]) {  // true is multiply
    Serial.print(currentValue);
    Serial.print(" ");
    Serial.print(activeUnits[selectedUnit].unit1);
    Serial.print("  --->   ");
    Serial.print(result_char);
    Serial.print(" ");
    Serial.print(activeUnits[selectedUnit].unit2);
    Serial.print("  ");
    Serial.println(result_numeric,6);
  } else {
    Serial.print(currentValue);
    Serial.print(" ");
    Serial.print(activeUnits[selectedUnit].unit2);
    Serial.print("  --->   ");
    Serial.print(result_char);
    Serial.print(" ");
    Serial.print(activeUnits[selectedUnit].unit1);
    Serial.print("  ");
    Serial.println(result_numeric,6);
  }
}

void serialPrintReset(){        // indicate value has been reset
  Serial.print(currentValue);
  Serial.println("*");
}

void drawSettingsMenu() {
  drawsleepSelection();
  drawDimSelection();
  drawFormatSelection();
  drawButtonRow_Settings();
  drawBatteryStatus();
}

void drawButtonRow_Settings(){
  oledWriteString(&dispL, 0,0,6, "SLEEP", SETTINGS_SIZE, 0, 1);  
  oledWriteString(&dispL, 0,65,6, "DIM", SETTINGS_SIZE, 0, 1);  
  oledWriteString(&dispR, 0,0,6, "FORMAT", SETTINGS_SIZE, 0, 1);  
  oledWriteString(&dispR, 0,65,6, "EXIT", SETTINGS_SIZE, 0, 1);  
}

void drawsleepSelection(){
  // could have done this as a struct but im lazy
  // 10000,        // 0: 10s
  // 30000,        // 1: 30s
  // 60000,        // 2: 1 min
  // 120000,       // 3: 2 min
  // 300000,       // 4: 5 min
  // 600000,       // 5: 10 min

  #define xpos 0
  #define ypos 3

  switch (sleepSelection) {
    case 0: oledWriteString(&dispL, 0,xpos,ypos, "10 sec", SETTINGS_SIZE, 0, 1);break;
    case 1: oledWriteString(&dispL, 0,xpos,ypos, "30 sec", SETTINGS_SIZE, 0, 1);break;
    case 2: oledWriteString(&dispL, 0,xpos,ypos, " 1 min", SETTINGS_SIZE, 0, 1);break;
    case 3: oledWriteString(&dispL, 0,xpos,ypos, " 2 min", SETTINGS_SIZE, 0, 1);break;
    case 4: oledWriteString(&dispL, 0,xpos,ypos, " 5 min", SETTINGS_SIZE, 0, 1);break;
    case 5: oledWriteString(&dispL, 0,xpos,ypos, "10 min", SETTINGS_SIZE, 0, 1);break;
  }
}

void drawDimSelection(){
// empty
}

void drawFormatSelection(){
  if (longFormat) {
  oledWriteString(&dispR, 0,0,3, "LONG ", SETTINGS_SIZE, 0, 1); 
  } else {
  oledWriteString(&dispR, 0,0,3, "SHORT", SETTINGS_SIZE, 0, 1); 
  }
}

void drawBatteryStatus(){

}


//------------------------------------------------------//
//
//                    MENUS
//
//------------------------------------------------------//

// enum modes {
//   ROUTINE,
//   UNITS_MENU,
//   SETTINGS_MENU
// } DEVICE_MODE;


void enter_menus(KeypadEvent key){
  if (tenKey.getState() == HOLD && DEVICE_MODE == ROUTINE) {
    switch(key) {
      case 'A': case 'B': case 'C': case 'D':
        Serial.println("UNITS");
        DEVICE_MODE = UNITS_MENU;
        break;
      case '.':
        Serial.println("SETTINGS");
        DEVICE_MODE = SETTINGS_MENU;
        clearDisplays();
        drawSettingsMenu();
        break;
    }
  }
  if (tenKey.getState() == HOLD && DEVICE_MODE == SETTINGS_MENU && key == 'D'){
    gotoSleep();
  }
}

void settings_menu(){


  switch (key){
    case 'A':
      Serial.println("SLEEP TIMER");
      sleepSelection++;
      if (sleepSelection > 5){sleepSelection = 0;}
      drawsleepSelection();
      Serial.println(sleepTimer[sleepSelection]);
      break;
    case 'B':
      // Serial.println("DIM TIMER");
      Serial.println("DIM");
      //gotoSleep();
      break;      
    case 'C':
      Serial.println("FORMAT");
        longFormat = !longFormat;
        drawFormatSelection();
        if (!longFormat) {
          Serial.println("Short Format Selected");
        } else {
          Serial.println("Long Format Selected");
        }
      break;

    case 'D':
      Serial.println("EXIT");
      DEVICE_MODE = ROUTINE;
      routine_Displays();
      break;
  }

}


/////////////////////////////////////////////////////////////////////////////////////////////////


//------------------------------------------------------//
//
//                    SETUP
//
//-----------------------------------------------------//


void setup() {
  Serial.begin(9600);

  ///////// CONSIDER CHANGING SETUP SO IT RUNS WAKEUP INSTEAD OF DUPLICATING COMMANDS BETWEEN SETUP AND WAKEUP ////////////

  // initialize Pin 2 for use as external interrupt
  pinMode(wakeUpPin, INPUT_PULLUP);
  pinMode(voltageSensePin, INPUT);

  // something funny happens when attachInterrupt is done only in setup. move back to gotoSleep and surround lowPower.powerDown. maybe investigate further later if it breaks here, it could break there too. 
  // appears to be an issue in oled library????
  // pinMode(wakeUpPin_GND,INPUT);
  // attachInterrupt(digitalPinToInterrupt(wakeUpPin), wakeUp_ISR, LOW);

  // initialize OLED displays
  //oledInit(SSOLED *, type, oled_addr, rotate180, invert, bWire, SDA_PIN, SCL_PIN, RESET_PIN, speed)
  oledInit(&dispL, displaySize_L, I2C_Address_L, flipDisplay_L, invertDisplay_L, USE_HW_I2C_L, SDA_L, SCL_L, displayResetPin, 400000L); // 400kHz
  oledInit(&dispR, displaySize_R, I2C_Address_R, flipDisplay_R, invertDisplay_R, USE_HW_I2C_R, SDA_R, SCL_R, displayResetPin, 400000L); // 400kHz
  oledFill(&dispL, 0, 1);
  oledFill(&dispR, 0, 1);

  // // initialize display with units and math
  doMath();
  updateDisplayValues();
  updateDisplayUnits(); 

  fillVoltageArr();

  tenKey.addEventListener(enter_menus);
  tenKey.setHoldTime(1000);
  
  DEVICE_MODE = ROUTINE;

}

//------------------------------------------------------//
//
//                    LOOP
//
//-----------------------------------------------------//


void loop() {

  if(run_wakeUp){wakeUp();}

  key = tenKey.getKey();

  if(key){

    idleTime = millis();

    switch (DEVICE_MODE){

      case SETTINGS_MENU:
        settings_menu();
        break;

      case UNITS_MENU:
        break;

      case ROUTINE:
          switch (key){

                case 'd': case 'l': break; // SKIP UNUSED KEYS
                case 'A': case 'B': case 'C': case 'D':
                  selectUnit(); 
                  doMath();        
                  updateDisplayUnits();
                  updateDisplayValues();
                  break;

                case 'E':  
                  updateDisplayResetValues(); // do reset highlight format BEFORE currentValue is cleared
                  serialPrintReset();
                  resetCurrentValue();
                  break;

                case 'S':
                  invertConversion();
                  doMath();
                  updateDisplayUnits();
                  updateDisplayValues();
                  break;

                case 'u': // remove later


                  break;

                default:
                  setCurrentValue();
                  doMath();
                  updateDisplayValues();
                }




          break;



    }

   


 
 
 
 
  }

  if(checkTimeForSleep() || checkLowBattery()){
    delay(500);  // not so fast
    gotoSleep();
  }

  delay(50);
}



  // sleep occurs on low battery voltage
  // sleep occurs XXXXX time after last key press; key press resets timer
  // check current time compared to WakeTime
  // check current battery voltage
  // attach interrupt if its time to put the device to sleep
  // put the displays in low power mode
  // turn off the displays
  // put the device to sleep, call gotoSleep()
  // await interrupt to turn the device back on


/*

  switch (key){
    case 'A':
      break;
    case 'B':
      break;
    case 'C':
      break;
    case 'D':
      break;
  }

/*/












// bool checkLowBattery(){
//   // returns FALSE if battery voltage is good
//   // returns TRUE if battery voltage is below threshold

//   // voltage divider cuts batt voltage in half :: 4.2V battery = 2.1V to analog input
//   // sensor reading is output from 0-1023
//   // scale reading to output :: 3.3V over 1023 steps
//   // correction factor for voltage :: difference between true voltage and sensed voltage :: 2.2 sensed vs 2.06 true
 
//  if(idleTime - wakeTime > 5000){ // only check battery every 5 seconds! does less float math and lets battery voltage average out
 

//     batteryVoltage = 2 * analogRead(voltageSensePin) * (logicLevel/1023.0) * (voltageCorrection / 100.0);

//     // Serial.print(analogRead(A2));
//     // Serial.print("       ");
//     Serial.println(batteryVoltage);
//     // Serial.print("       ");
//     // Serial.println((uint8_t)batteryVoltage);

  

//     if(batteryVoltage < shutdownVoltage){
//       Serial.println("BATTERY LOW");
//       return 1;
//     } else {
//       //Serial.println("BATTERY GOOD");
//       return 0;
//     }
//  }
//   // checks battery voltage for low voltage state to turn off device thru checkSleep
//   // displays battery voltage on screen?
//   // displays if battery voltage is low on screen (highlight? extra text?)
//   // displays if battery critical
// }

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


// #define voltageSensePin A2
// #define logicLevel 3.3
// #define battCheckDelayTime 500 // only check battery every 30 seconds! does less float math and lets battery voltage average out

// uint8_t voltageCorrection = 94; // (real voltage / analog read voltage) * 100  ----- 93.6% = 94
// float batteryVoltage;
// float warningVoltage = 3.300;
// float voltageNoiseFilter = 0.1; // battery voltage is noisy. Specify a range around target value to look for.
// float shutdownVoltage = 3.100;
// unsigned long checkBattTime = millis();    // dont set to millis() because if statent checks for 0
// int battVolt;

// bool checkLowBattery(){
//   // returns FALSE if battery voltage is good
//   // returns TRUE if battery voltage is below threshold

//   // voltage divider cuts batt voltage in half :: 4.2V battery = 2.1V to analog input
//   // sensor reading is 10-bit :: 1024 discrete values
//   // scale reading to output :: 3.3V over 1024 steps
//   // correction factor for voltage :: difference between battery voltage and sensed voltage :: 3.2 sensed vs 3.06 true

// //Serial.println(checkBattTime);

// // if (!checkBattTime){ // set battery time check delay if checkBattTime has been reset to 0
// //   checkBattTime = millis();
// // }


// unsigned int getBatteryVoltage() {

  
// }



//  if(millis() - checkBattTime > battCheckDelayTime){ 

//     checkBattTime = millis();
  
//     batteryVoltage = 2 * analogRead(voltageSensePin) * (logicLevel/1024.0) * (voltageCorrection / 100.0);

//     battVolt = batteryVoltage * 1000;

//     Serial.print("----");
//     Serial.print(batteryVoltage, 5);
//     Serial.print("----");
//     Serial.print(battVolt);
//     Serial.println("----");

//     if(batteryVoltage < shutdownVoltage){
//         Serial.println("BATTERY LOW");
//         return 1;
//     } else if(batteryVoltage < warningVoltage){
//         displayLowBatteryWarning(1);
//         return 0;
//     } else if(batteryVoltage > warningVoltage){
//         displayLowBatteryWarning(0);
//         return 0;
//     } else {
//         return 0;
//     }

//  } else {
//    return 0;
//  }

