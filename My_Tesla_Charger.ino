
/*
  Tesla Gen 2 Charger Control Program
  2017-2019
  T de Bree
  D.Maguire
  Additional work by C. Kidder
  Runs on OpenSource Logic board V3 in Gen2 charger. Commands all modules.

  D.Maguire 2019 Mods :
  -Stop sending power module can messages when charger not running - Working.
  -Correct reading of charger Fault and Enable feedback signals - Working.
  -Correct AC present flag so only sets if more than 70V AC is on each module - Working.
  -Reset charger on detection of power module fault - Testing.
  -Shutdown on exceeding a preset HV Battery voltage - Working.
  -Evse read routine now in 500ms loop to prevent false triggering -Working.
  -Added counter/timer to autoshutdown to prevent false triggering on transients -Working.
  -Added manual control mode for use of charger without EVSE. Digital one in when brought to +12v commands the charger to start
  and when brought low commands charger off. This mode also control HVDC via digital out 1 and AC mains via a contactor via Digital out 2.-Untested.

  joromy 2020 Mods:
  Added proximity status and more on CAN1 in external_can section
  Added void HeatCoolMode(), will ativate EVSE when IN2 is high. (AC only, All DC contactors off!)

*/

#include "config.h"
#include <can_common.h>
#include <due_can.h>
#include <due_wire.h>
#include <Wire_EEPROM.h>
#include <DueTimer.h>
#include <rtc_clock.h> ///https://github.com/MarkusLange/Arduino-Due-RTC-Library

#define Serial SerialUSB
template<class T> inline Print &operator <<(Print &obj, T arg)
{
  obj.print(arg);
  return obj;
}

//RTC_clock rtc_clock(XTAL);

int watchdogTime = 8000;

char* daynames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};


//*********GENERAL VARIABLE   DATA ******************
int evsedebug = 1; // 1 = show Proximity status and Pilot current limmits
int debug = 1; // 1 = show phase module CAN feedback

// Test DEBUG int
int data7 = 0;

int setting = 1;
int incomingByte = 0;
int state;
unsigned long slavetimeout, tlast, tcan, tboot = 0;
bool bChargerEnabled;


//*********EVSE VARIABLE   DATA ******************
byte Proximity = 0;
uint16_t ACvoltIN = 240; // AC input voltage 240VAC for EU/UK and 110VAC for US
//proximity status values for type 1
#define Unconnected 0 // 3.3V
#define Buttonpress 1 // 2.3V
#define Connected 2 // 1.35V

volatile uint32_t pilottimer = 0;
volatile uint16_t duration = 0;
volatile uint16_t accurlim = 0;

uint16_t cablelim = 0; // Type 2 cable current limit

//*********Single or Three Phase Config VARIABLE   DATA ******************

//proximity status values
#define Singlephase 0 // all parrallel on one phase Type 1
#define Threephase 1 // one module per phase Type 2

//*********Charger Control VARIABLE   DATA ******************
uint16_t modulelimcur, dcaclim = 0;
uint16_t maxaccur = 16000; //maximum AC current in mA
uint16_t maxdccur = 45000; //max DC current output in mA
int activemodules, slavechargerenable = 0;



//*********Feedback from charge VARIABLE   DATA ******************
uint16_t dcvolt[3] = {0, 0, 0};//1 = 1V
uint16_t dccur[3] = {0, 0, 0};
uint16_t totdccur = 0;//1 = 0.005Amp
uint16_t acvolt[3] = {0, 0, 0};//1 = 1V
uint16_t accur[3] = {0, 0, 0};//1 = 0.06666 Amp
byte inlettarg [3] = {0, 0, 0}; //inlet target temperatures, should be used to command cooling.
byte curtemplim [3] = {0, 0, 0};//current limit due to temperature
byte templeg[2][3] = {{0, 0, 0}, {0, 0, 0}}; //temperatures reported back
bool ACpres [3] = {0, 0, 0}; //AC present detection on the modules
bool ModEn [3] = {0, 0, 0}; //Module enable feedback on the modules
bool ModFlt [3] = {0, 0, 0}; //module fault feedback
bool LockOut = false; //lockout on termination voltage reached. Reset by evse plug recycle.
byte ModStat [3] = {0, 0, 0};//Module Status
int newframe = 0;

ChargerParams parameters;

//*********cantx_test Messages VARIABLE   DATA ******************
bool cantx_test = 1; // turn on can messages for cantx_test.

//*********Charger Messages VARIABLE   DATA ******************
int ControlID = 0x300;
int StatusID = 0x410;
unsigned long ElconID = 0x18FF50E5;
unsigned long ElconControlID = 0x1806E5F4;

uint16_t LockOutCnt = 0; // lockout counter

int candebug = 0;
int menuload = 0;

// this function has to be present, otherwise watchdog won't work
void watchdogSetup(void)
{
  // do what you want here
}

void setup()
{
  Serial.begin(115200);  //Initialize our USB port which will always be redefined as SerialUSB to use the Native USB port tied directly to the SAM3X processor.

  attachInterrupt(EVSE_PILOT, Pilotread , CHANGE);
  watchdogEnable(watchdogTime);
  Wire.begin();
  EEPROM.read(0, parameters);
  if (parameters.version != EEPROM_VERSION)
  {
    parameters.version = EEPROM_VERSION;
    parameters.currReq = 0; //max input limit per module 1500 = 1A
    parameters.enabledChargers = 123; // enable per phase - 123 is all phases - 3 is just phase 3
    parameters.can0Speed = 500000;
    parameters.can1Speed = 500000;
    parameters.voltSet = 32000; //1 = 0.01V
    parameters.tVolt = 34000;//1 = 0.01V
    parameters.autoEnableCharger = 0; //disable auto start, proximity and pilot control
    parameters.canControl = 0; //0 disabled can control, 1 master, 3 slave
    parameters.phaseconfig = Threephase; //AC input configuration
    parameters.type = 2; //Socket type1 or 2
    EEPROM.write(0, parameters);
  }
  /*
    ////rtc clock start///////
    rtc_clock.init();
    rtc_clock.set_time(19, 35, 0);
    rtc_clock.set_date(16, 11, 2018);
  */

  // Initialize CAN1
  if (Can1.begin(parameters.can1Speed, 255)) //can1 external bus
  {
    Serial.println("Using CAN1 - initialization completed.\n");
  }
  else Serial.println("CAN1 initialization (sync) ERROR\n");


  // Initialize CAN0
  if (Can0.begin(parameters.can0Speed, 255)) //can0 charger modules
  {
    Serial.println("Using CAN0 - initialization completed.\n");
  }
  else Serial.println("CAN0 initialization (sync) ERROR\n");

  int filter;
  //extended
  for (filter = 0; filter < 3; filter++)
  {
    Can0.setRXFilter(filter, 0, 0, true);
    Can1.setRXFilter(filter, 0, 0, true);
  }
  //standard
  for (int filter = 3; filter < 7; filter++)
  {
    Can0.setRXFilter(filter, 0, 0, false);
    Can1.setRXFilter(filter, 0, 0, false);
  }
  ///////////////////CHARGER ENABLE AND ACTIVATE LINES///////////////////////////////////
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(CHARGER1_ENABLE, OUTPUT); //CHG1 ENABLE
  pinMode(CHARGER2_ENABLE, OUTPUT);  //CHG2 ENABLE
  pinMode(CHARGER3_ENABLE, OUTPUT); //CHG3 ENABLE
  pinMode(CHARGER1_ACTIVATE, OUTPUT); //CHG1 ACTIVATE
  pinMode(CHARGER2_ACTIVATE, OUTPUT);  //CHG2 ACTIVATE
  pinMode(CHARGER3_ACTIVATE, OUTPUT); //CHG3 ACTIVATE
  //////////////////////////////////////////////////////////////////////////////////////
  pinMode(DIG_IN_1, INPUT); //CHARGE ENABLE
  pinMode(DIG_IN_2, INPUT); //BATTERY HEAT/COOL ON AC ONLY
  //////////////////////////////////////////////////////////////////////////////////////

  //////////////DIGITAL OUTPUTS MAPPED TO X046. 10 PIN CONNECTOR ON LEFT//////////////////////////////////////////
  pinMode(DIG_OUT_1, OUTPUT); //OP1 - X046 PIN 6
  pinMode(DIG_OUT_2, OUTPUT); //OP2
  pinMode(DIG_OUT_3, OUTPUT); //OP2
  pinMode(DIG_OUT_4, OUTPUT); //OP3
  pinMode(EVSE_ACTIVATE, OUTPUT); //pull Pilot to 6V
  ///////////////////////////////////////////////////////////////////////////////////////

  dcaclim = maxaccur;
  bChargerEnabled = false; //are we supposed to command the charger to charge?
}

void loop()
{
  CAN_FRAME incoming;

  if (Can0.available())
  {
    Can0.read(incoming);
    candecode(incoming);
  }

  if (Can1.available())
  {
    Can1.read(incoming);
    canextdecode(incoming);
  }

  if (Serial.available())
  {
    menu();
  }

  if (parameters.canControl > 1)
  {
    if (state != 0)
    {
      if (millis() - tcan > 2000)
      {
        state = 0;
        Serial.println();
        Serial.println("CAN time-out");
      }
    }
  }

  if (digitalRead(DIG_IN_1) == LOW)
  {
    state = 0;
  }

  switch (state)
  {
    case 0: //Charger off
      Timer3.detachInterrupt(); // stop sending charger power module CAN messages
      if (bChargerEnabled == true)
      {
        bChargerEnabled = false;
      }
      digitalWrite(DIG_OUT_1, LOW);         //HV OFF
      digitalWrite(EVSE_ACTIVATE, LOW);
      // Is this needed???? Works start and stop heat
      if (digitalRead(DIG_IN_2) == LOW)     //JOMY
        digitalWrite(EVSE_ACTIVATE, LOW);
      else
        digitalWrite(EVSE_ACTIVATE, HIGH);   //JOMY
      digitalWrite(DIG_OUT_2, LOW);//AC OFF when in manual mode.
      digitalWrite(CHARGER1_ACTIVATE, LOW); //chargeph1 deactivate
      digitalWrite(CHARGER2_ACTIVATE, LOW); //chargeph2 deactivate
      digitalWrite(CHARGER3_ACTIVATE, LOW); //chargeph3 deactivate
      digitalWrite(CHARGER1_ENABLE, LOW);//disable phase 1 power module
      digitalWrite(CHARGER2_ENABLE, LOW);//disable phase 2 power module
      digitalWrite(CHARGER3_ENABLE, LOW);//disable phase 3 power module
      break;

    case 1://Charger on
      if (digitalRead(DIG_IN_1) == HIGH)
      {
        if (bChargerEnabled == false)
        {
          bChargerEnabled = true;
          switch (parameters.enabledChargers)
          {
            case 1:
              digitalWrite(CHARGER1_ACTIVATE, HIGH);
              activemodules = 1;
              break;
            case 2:
              digitalWrite(CHARGER2_ACTIVATE, HIGH);
              activemodules = 1;
              break;
            case 3:
              digitalWrite(CHARGER3_ACTIVATE, HIGH);
              activemodules = 1;
              break;
            case 12:
              digitalWrite(CHARGER1_ACTIVATE, HIGH);
              digitalWrite(CHARGER2_ACTIVATE, HIGH);
              activemodules = 2;
              break;
            case 13:
              digitalWrite(CHARGER1_ACTIVATE, HIGH);
              digitalWrite(CHARGER3_ACTIVATE, HIGH);
              activemodules = 2;
              break;
            case 123:
              digitalWrite(CHARGER1_ACTIVATE, HIGH);
              digitalWrite(CHARGER2_ACTIVATE, HIGH);
              digitalWrite(CHARGER3_ACTIVATE, HIGH);
              activemodules = 3;
              break;
            case 23:
              digitalWrite(CHARGER2_ACTIVATE, HIGH);
              digitalWrite(CHARGER3_ACTIVATE, HIGH);
              activemodules = 2;
              break;
            default:
              // if nothing else matches, do the default
              // default is optional
              break;
          }
          delay(100);
          digitalWrite(DIG_OUT_1, HIGH);//HV ON
          digitalWrite(EVSE_ACTIVATE, HIGH); //evse on
          digitalWrite(DIG_OUT_2, HIGH);//AC on in manual mode
        }
      }
      else
      {
        bChargerEnabled == false;
        state = 0;
      }
      break;

    case 2:
      Timer3.attachInterrupt(Charger_msgs).start(90000); // start sending charger power module CAN messages
      switch (parameters.enabledChargers)
      {
        case 1:
          digitalWrite(CHARGER1_ENABLE, HIGH);//enable phase 1 power module
          break;
        case 2:
          digitalWrite(CHARGER2_ENABLE, HIGH);//enable phase 2 power module
          break;
        case 3:
          digitalWrite(CHARGER3_ENABLE, HIGH);//enable phase 3 power module
          break;
        case 12:
          digitalWrite(CHARGER1_ENABLE, HIGH);//enable phase 1 power module
          digitalWrite(CHARGER2_ENABLE, HIGH);//enable phase 2 power module
          break;
        case 13:
          digitalWrite(CHARGER1_ENABLE, HIGH);//enable phase 1 power module
          digitalWrite(CHARGER3_ENABLE, HIGH);//enable phase 3 power module
          break;
        case 123:
          digitalWrite(CHARGER1_ENABLE, HIGH);//enable phase 1 power module
          digitalWrite(CHARGER2_ENABLE, HIGH);//enable phase 2 power module
          digitalWrite(CHARGER3_ENABLE, HIGH);//enable phase 3 power module
          break;
        case 23:
          digitalWrite(CHARGER2_ENABLE, HIGH);//enable phase 2 power module
          digitalWrite(CHARGER3_ENABLE, HIGH);//enable phase 3 power module
          break;

        default:
          // if nothing else matches, do the default
          break;
      }
      if (tboot <  (millis() - 1000)) //delay in ms before moving to state 1.
      {
        state = 1;
      }

    default:
      // if nothing else matches, do the default
      break;
  }
  if (tlast <  (millis() - 500))
  {
    tlast = millis();
    evseread();
    external_can();
    autoShutdown();
    watchdogReset();
    manualMode();
    // Is this needed???? Works start and stop heat without!!
    // Next will stop charging to work!!!!
    if (digitalRead(DIG_IN_2) == HIGH)
      HeatCoolMode();
    if (debug != 0)
    {
      Serial.println();

      Serial.print("State:");
      Serial.print(state);
      Serial.print(", Phases:");
      Serial.print(parameters.phaseconfig);
      Serial.print(", Modules Active:");
      Serial.print(activemodules);
      if (bChargerEnabled)
      {
        Serial.print(", CHG:ON, ");
      }
      else
      {
        Serial.print(", CHG:OFF, ");
      }
      if (digitalRead(DIG_IN_1) == HIGH)
      {
        Serial.print("IN1:HIGH BMS_CHG enable, ");
      }
      else
      {
        Serial.print("IN1:LOW BMS_CHG disable, ");
      }
      if (digitalRead(DIG_IN_2) == HIGH)
      {
        Serial.print("IN2:HIGH EVSE enable, ");
      }
      else
      {
        Serial.print("IN2:LOW EVSE disable, ");
      }

      if (digitalRead(EVSE_ACTIVATE) == HIGH)
      {
        Serial.print("EVSE_ACTIVATE:HIGH ");
      }
      else
      {
        Serial.print(" EVSE_ACTIVATE:LOW ");
      }


      if (bChargerEnabled)
      {
        Serial.println();
        for (int x = 0; x < 3; x++)
        {
          Serial.print("  Phase ");
          Serial.print(x + 1);
          Serial.print(" Feebback //  AC present: ");
          Serial.print(ACpres[x]);
          Serial.print("  AC volt:");
          Serial.print(acvolt[x]);
          Serial.print("  AC cur:");
          Serial.print((accur[x] * 0.06666), 2);
          Serial.print("  DC volt:");
          Serial.print(dcvolt[x]);
          Serial.print("  DC cur: ");
          Serial.print(dccur[x] * 0.000839233, 2);
          Serial.print("  Inlet Targ:");
          Serial.print(inlettarg[x]);
          Serial.print("  Temp Lim Cur:");
          Serial.print(curtemplim[x]);
          Serial.print("  ");
          Serial.print(templeg[0][x]);
          Serial.print("  ");
          Serial.print(templeg[1][x]);
          Serial.print(" EN:");
          Serial.print(ModEn[x]);
          Serial.print(" Flt:");
          Serial.print(ModFlt[x]);
          Serial.print(" Stat:");
          Serial.print(ModStat[x], BIN);
          Serial.println();
        }
      }
      else
      {
        Serial.println();
        Serial.print("Modules Turned OFF");
        Serial.println();
      }
    }
    if (debug == 1)
    {
      if (evsedebug != 0)
      {
        Serial.println();
        Serial.print("millis:");
        Serial.print(millis());
        Serial.println();
        Serial.print("Proximity Status:");
        switch (Proximity)
        {
          case Unconnected:
            Serial.print("Unconnected,");
            break;
          case Buttonpress:
            Serial.print("Button Pressed,");
            break;
          case Connected:
            Serial.print("Connected,");
            break;

        }
        Serial.print(" AC limit:");
        Serial.print(accurlim);
        Serial.print(", Cable Limit:");
        Serial.print(cablelim);
        Serial.println();
        Serial.print("Module Cur Request:");
        Serial.print(modulelimcur / 1.5, 0);
        Serial.print(", DC total Cur:");
        Serial.print(totdccur * 0.005, 2);
        Serial.print(", DC Setpoint:");
        Serial.print(parameters.voltSet * 0.01, 0);
        Serial.print(", DC tVolt:");
        Serial.print(parameters.tVolt * 0.01, 0);
        Serial.print(", DC driven AC Cur Lim:");
        Serial.print(dcaclim);
      }
    }
  }
  DCcurrentlimit();
  ACcurrentlimit();

  resetFaults();


  if (parameters.autoEnableCharger == 1)
  {
    if ((Proximity == Connected) && (LockOut == false)) //check if plugged in and not locked out
    {
      //digitalWrite(EVSE_ACTIVATE, HIGH);//pull pilot low to indicate ready - NOT WORKING freezes PWM reading
      if (accurlim > 1400) // one amp or more active modules
      {
        if (state == 0)
        {
          if (digitalRead(DIG_IN_1) == HIGH)
          {
            state = 2;// initialize modules
            tboot = millis();
          }
        }
      }
      digitalWrite(DIG_OUT_2, HIGH); //enable AC present indication
    }
    else // unplugged or buton pressed stop charging
    {
      state = 0;
      digitalWrite(DIG_OUT_2, LOW); //disable AC present indication
      if (digitalRead(DIG_IN_2) == LOW)     //JOMY
        digitalWrite(EVSE_ACTIVATE, LOW);
      else
        digitalWrite(EVSE_ACTIVATE, HIGH);   //JOMY check this!!!!!!!!
    }
  }
}//end of void loop


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//If a charger power module is running and faults out we want to reset and go again otherwise charger can just sit there and not finish a charge.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void resetFaults()
{
  if ((bChargerEnabled == true) && (ACpres[0] == true) && (ModFlt[0] == true) && ((parameters.enabledChargers == 1) || (parameters.enabledChargers == 12) || (parameters.enabledChargers == 13) || (parameters.enabledChargers == 123)))
  {
    //if these conditions are met then phase one is enabled, has ac present and has entered a fault state so we want to reset.
    state = 0;
    digitalWrite(DIG_OUT_2, LOW); //disable AC present indication;
    //  digitalWrite(EVSE_ACTIVATE, LOW);
  }
  if ((bChargerEnabled == true) && (ACpres[1] == true) && (ModFlt[1] == true) && ((parameters.enabledChargers == 2) || (parameters.enabledChargers == 12) || (parameters.enabledChargers == 23) || (parameters.enabledChargers == 123)))
  {
    //if these conditions are met then phase two is enabled, has ac present and has entered a fault state so we want to reset.
    state = 0;
    digitalWrite(DIG_OUT_2, LOW); //disable AC present indication;
    //digitalWrite(EVSE_ACTIVATE, LOW);
  }
  if ((bChargerEnabled == true) && (ACpres[2] == true) && (ModFlt[2] == true) && ((parameters.enabledChargers == 3) || (parameters.enabledChargers == 13) || (parameters.enabledChargers == 23) || (parameters.enabledChargers == 123)))
  {
    //if these conditions are met then phase three is enabled, has ac present and has entered a fault state so we want to reset.
    state = 0;
    digitalWrite(DIG_OUT_2, LOW); //disable AC present indication;
    //digitalWrite(EVSE_ACTIVATE, LOW);
  }


}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//If the HV voltage exceeds the tVolt setpoint we want to shut down the charger and not re enable until the charge plug
//is removed and re connected. For now we just read the voltage on phase module one.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void autoShutdown() {
  if ((bChargerEnabled == true) && (LockOut == false)) //if charger is running and we are not locked out ...
  {
    if (dcvolt[0] > (parameters.tVolt * 0.01)) //and if we exceed tVolt...
    {
      LockOutCnt++; //increment the lockout counter
      //      LockOut=true; //lockout and shutdown
      //    state=0;
    }
    else
    {
      LockOutCnt = 0; //other wise we reset the lockout counter
    }

  }
  if (Proximity == Unconnected && (parameters.autoEnableCharger == 1)) LockOut = false; //re set the lockout flag when the evse plug is pulled only if we are in evse mode.

  if (LockOutCnt > 10)
  {
    state = 0; //if we are above our shutdown targer for 10 consecutive counts we lockout
    LockOut = true; //lockout and shutdown
    LockOutCnt = 0;
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///Manual control mode via Digital input 1. Special case for A.Rish.
/////////////////////////////////////////////////////////////////////////////////////
void manualMode()
{

  if (parameters.autoEnableCharger == 0)//if we are not in auto mode ...
  {
    if (state == 0)//....and if we are currently turned off....
    {
      if (digitalRead(DIG_IN_1) == HIGH && (LockOut == false)) //...and if digital one is high....
      {
        state = 2;// initialize modules. Fire up the charger.
        tboot = millis();
      }
    }



    if (digitalRead(DIG_IN_1) == LOW)//...if brought low then we shutoff the charger.
    {
      state = 0;//
      LockOut = false; //release lockout when dig 1 in is brought low.
    }
  }
}

// Is this needed???? Works start and stop heat without!!
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///Battery heat/cool mode via Digital input 2. (AC only, All DC contactors off!)
/////////////////////////////////////////////////////////////////////////////////////
void HeatCoolMode()
{
  if (state == 0)//....and if we are currently turned off....
  {
    if (digitalRead(DIG_IN_2) == HIGH && (LockOut == false)) //...and if digital two is high....
    {
      digitalWrite(EVSE_ACTIVATE, HIGH); //evse on, pull Pilot to 6V
      digitalWrite(DIG_OUT_2, HIGH); //enable AC present indication
    }
  }

  if (digitalRead(DIG_IN_2) == LOW)//...if brought low then we shutoff EVSE.
  {
    state = 0;//
    LockOut = false; //release lockout when dig 2 in is brought low.
  }
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void candecode(CAN_FRAME & frame)
{
  int x = 0;
  switch (frame.id)
  {
    case 0x217: //phase 1 Status message
      ModStat[0] = frame.data.bytes[0];
      break;

    case 0x219: //phase 2 Status message
      ModStat[1] = frame.data.bytes[0];
      break;

    case 0x21B: //phase 3 Status message
      ModStat[2] = frame.data.bytes[0];
      break;

    case 0x24B: //phase 3 temp message 2
      curtemplim[2] = frame.data.bytes[0] * 0.234375;
      newframe = newframe | 1;
      break;

    case 0x23B: //phase 3 temp message 1
      templeg[0][2] = frame.data.bytes[0] - 40;
      templeg[1][2] = frame.data.bytes[1] - 40;
      inlettarg[2] = frame.data.bytes[5] - 40;
      newframe = newframe | 1;
      break;

    case 0x239: //phase 2 temp message 1
      templeg[0][1] = frame.data.bytes[0] - 40;
      templeg[1][1] = frame.data.bytes[1] - 40;
      inlettarg[1] = frame.data.bytes[5] - 40;
      newframe = newframe | 1;
      break;
    case 0x249: //phase 2 temp message 2
      curtemplim[1] = frame.data.bytes[0] * 0.234375;
      newframe = newframe | 1;
      break;

    case 0x237: //phase 1 temp message 1
      templeg[0][0] = frame.data.bytes[0] - 40;
      templeg[1][0] = frame.data.bytes[1] - 40;
      inlettarg[0] = frame.data.bytes[5] - 40;
      newframe = newframe | 1;
      break;
    case 0x247: //phase 2 temp message 2
      curtemplim[0] = frame.data.bytes[0] * 0.234375;
      newframe = newframe | 1;
      break;

    case 0x207: //phase 2 msg 0x209. phase 3 msg 0x20B
      acvolt[0] = frame.data.bytes[1];
      accur[0] = uint16_t((frame.data.bytes[6] & 0x03) * 256 + frame.data.bytes[5]) >> 1 ;
      x = frame.data.bytes[1];// & 12;
      if (x > 0x46) //say 0x46 = 70V
      {
        ACpres[0] = true;
      }
      else
      {
        ACpres[0] = false;
      }
      x = frame.data.bytes[2] & 0x02; //was 40
      if (x != 0)
      {
        ModEn[0] = true;
      }
      else
      {
        ModEn[0] = false;
      }
      x = frame.data.bytes[2] & 0x04; //was 20
      if (x != 0)
      {
        ModFlt[0] = true;
      }
      else
      {
        ModFlt[0] = false;
      }
      newframe = newframe | 1;
      break;
    case 0x209: //phase 2 msg 0x209. phase 3 msg 0x20B
      acvolt[1] = frame.data.bytes[1];
      accur[1] = uint16_t((frame.data.bytes[6] & 0x03) * 256 + frame.data.bytes[5]) >> 1 ;
      x = frame.data.bytes[1];// & 12;
      if (x > 0x46) //say 0x46 = 70V)
      {
        ACpres[1] = true;
      }
      else
      {
        ACpres[1] = false;
      }
      x = frame.data.bytes[2] & 0x02; //was 40
      if (x != 0)
      {
        ModEn[1] = true;
      }
      else
      {
        ModEn[1] = false;
      }
      x = frame.data.bytes[2] & 0x04; //was 20
      if (x != 0)
      {
        ModFlt[1] = true;
      }
      else
      {
        ModFlt[1] = false;
      }
      newframe = newframe | 1;
      break;
    case 0x20B: //phase 2 msg 0x209. phase 3 msg 0x20B
      acvolt[2] = frame.data.bytes[1];
      accur[2] = uint16_t((frame.data.bytes[6] & 0x03) * 256 + frame.data.bytes[5]) >> 1 ;
      x = frame.data.bytes[1];// & 12;
      if (x > 0x46) //say 0x46 = 70V)
      {
        ACpres[2] = true;
      }
      else
      {
        ACpres[2] = false;
      }
      x = frame.data.bytes[2] & 0x02; //was 40
      if (x != 0)
      {
        ModEn[2] = true;
      }
      else
      {
        ModEn[2] = false;
      }
      x = frame.data.bytes[2] & 0x04; //was 20
      if (x != 0)
      {
        ModFlt[2] = true;
      }
      else
      {
        ModFlt[2] = false;
      }
      newframe = newframe | 1;
      break;
    case 0x227: //dc feedback. Phase 1 measured DC battery current and voltage Charger phase 2 msg : 0x229. Charger phase 3 mesg : 0x22B
      dccur[0] = ((frame.data.bytes[5] << 8) + frame.data.bytes[4]) ;//* 0.000839233 convert in rest of code
      dcvolt[0] = ((frame.data.bytes[3] << 8) + frame.data.bytes[2]) * 0.0105286; //we left shift 8 bits to make a 16bit uint.
      newframe = newframe | 2;
      break;
    case 0x229: //dc feedback. Phase 2 measured DC battery current and voltage Charger phase 2 msg : 0x229. Charger phase 3 mesg : 0x22B
      dccur[1] = ((frame.data.bytes[5] << 8) + frame.data.bytes[4]) ;//* 0.000839233 convert in rest of code
      dcvolt[1] = ((frame.data.bytes[3] << 8) + frame.data.bytes[2]) * 0.0105286; //we left shift 8 bits to make a 16bit uint.
      newframe = newframe | 2;
      break;
    case 0x22B: //dc feedback. Phase 3 measured DC battery current and voltage Charger phase 2 msg : 0x229. Charger phase 3 mesg : 0x22B
      dccur[2] = ((frame.data.bytes[5] << 8) + frame.data.bytes[4]) ;//* 0.000839233 convert in rest of code
      dcvolt[2] = ((frame.data.bytes[3] << 8) + frame.data.bytes[2]) * 0.01052856; //we left shift 8 bits to make a 16bit uint.
      newframe = newframe | 2;
      break;

    default:
      // if nothing else matches, do the default
      break;
  }
}

void Charger_msgs()
{
  CAN_FRAME outframe;  //A structured variable according to due_can library for transmitting CAN data.
  /////////////////////This msg addresses all modules/////////////////////////////////////////////////
  outframe.id = 0x045c;            // Set our transmission address ID
  outframe.length = 8;            // Data payload 8 bytes
  outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
  outframe.rtr = 0;                 //No request
  outframe.data.bytes[0] = lowByte(parameters.voltSet);  //Voltage setpoint
  outframe.data.bytes[1] = highByte(parameters.voltSet);//Voltage setpoint
  outframe.data.bytes[2] = 0x14;
  if (bChargerEnabled)
  {
    outframe.data.bytes[3] = 0x2e;
  }
  else outframe.data.bytes[3] = 0x0e;
  outframe.data.bytes[4] = 0x00;
  outframe.data.bytes[5] = 0x00;
  outframe.data.bytes[6] = 0x90;
  outframe.data.bytes[7] = 0x8c;
  Can0.sendFrame(outframe);
  //////////////////////////////////////////////////////////////////////////////////////////////////////

  //////////////////////////////////////Phase 1 command message////////////////////////////////////////
  outframe.id = 0x042c;            // Set our transmission address ID
  outframe.length = 8;            // Data payload 8 bytes
  outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
  outframe.rtr = 0;                 //No request
  outframe.data.bytes[0] = 0x42;
  outframe.data.bytes[2] = lowByte(modulelimcur); //AC Current setpoint
  outframe.data.bytes[3] = highByte(modulelimcur); //AC Current setpoint
  if (bChargerEnabled)
  {
    outframe.data.bytes[1] = 0xBB;
    outframe.data.bytes[4] = 0xFE; //FE dont clear faults. FF do clear faults.
  }
  else
  {
    outframe.data.bytes[1] = lowByte(uint16_t(ACvoltIN / 1.2));
    outframe.data.bytes[4] = 0x64;
  }
  outframe.data.bytes[5] = 0x00;
  outframe.data.bytes[6] = 0x00;
  outframe.data.bytes[7] = 0x00;
  Can0.sendFrame(outframe);
  //////////////////////////////Phase 2 command message//////////////////////////////////////////////
  outframe.id = 0x43c;        //phase 2 and 3 are copies of phase 1 so no need to set them up again
  Can0.sendFrame(outframe);
  ///////////////////////////////Phase 3 command message/////////////////////////////////////////////
  outframe.id = 0x44c;
  Can0.sendFrame(outframe);

  ///////////Static Frame every 100ms///////////////////////////////////////////////////////////////////
  outframe.id = 0x368;            // Set our transmission address ID
  outframe.length = 8;            // Data payload 8 bytes
  outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
  outframe.rtr = 0;                 //No request
  outframe.data.bytes[0] = 0x03;
  outframe.data.bytes[1] = 0x49;
  outframe.data.bytes[2] = 0x29;
  outframe.data.bytes[3] = 0x11;
  outframe.data.bytes[4] = 0x00;
  outframe.data.bytes[5] = 0x0c;
  outframe.data.bytes[6] = 0x40;
  outframe.data.bytes[7] = 0xff;
  Can0.sendFrame(outframe);
}


/*////////////////////////////////////////////////////////////////////////////////////////////////////////
  External CAN
  ////////////////////////////////////////////////////////////////////////////////////////////////////////*/
void external_can()
{
  CAN_FRAME outframe;             // A structured variable according to due_can library for transmitting CAN data.

  uint16_t y, z = 0;
  outframe.id = StatusID;
  if (parameters.canControl == 3)
  {
    outframe.id = StatusID + 1;
  }
  outframe.length = 8;            // Data payload 8 bytes
  outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
  outframe.rtr = 0;                 //No request
  outframe.data.bytes[0] = 0x00;
  for (int x = 0; x < 3; x++)
  {
    y = y +  dcvolt[x] ;
  }
  outframe.data.bytes[0] = y / 3;

  if (parameters.phaseconfig == Singlephase)
  {
    for (int x = 0; x < 3; x++)
    {
      z = z + (accur[x] * 66.66) ;
    }
  }
  else
  {
    z = accur[2] * 66.66;
  }

  outframe.data.bytes[1] = lowByte (z);
  outframe.data.bytes[2] = highByte (z);

  outframe.data.bytes[3] = lowByte (uint16_t (totdccur)); //0.005Amp
  outframe.data.bytes[4] = highByte (uint16_t (totdccur));  //0.005Amp
  outframe.data.bytes[5] = lowByte (uint16_t (modulelimcur * 0.66666));
  outframe.data.bytes[6] = highByte (uint16_t (modulelimcur * 0.66666));
  outframe.data.bytes[7] = 0x00;
  outframe.data.bytes[7] = Proximity << 6;
  outframe.data.bytes[7] = outframe.data.bytes[7] || (parameters.type << 4);
  Can1.sendFrame(outframe);

  /////////Elcon Message////////////

  outframe.id = ElconID;
  if ( parameters.canControl == 3)
  {
    outframe.id = ElconID + 1;
  }
  outframe.id = ElconID;
  outframe.length = 8;            // Data payload 8 bytes
  outframe.extended = 1;          // Extended addresses - 0=11-bit 1=29bit
  outframe.rtr = 0;                 //No request


  outframe.data.bytes[0] = highByte (y * 10 / 3);
  outframe.data.bytes[1] = lowByte (y * 10 / 3);
  outframe.data.bytes[2] = highByte (uint16_t (totdccur * 20)); //0.005Amp conv to 0.1
  outframe.data.bytes[3] = lowByte (uint16_t (totdccur * 20)); //0.005Amp conv to 0.1
  outframe.data.bytes[4] = 0x00;
  outframe.data.bytes[5] = 0x00;
  outframe.data.bytes[6] = 0x00;
  outframe.data.bytes[7] = 0x00;
  Can1.sendFrame(outframe);


  ///CAN SEND TEST//////////////////////////////////////////////////////////////////////
  if (cantx_test)
  {
    //    delay(1000);
    outframe.id = 0x3D8;
    outframe.length = 8;            // Data payload 8 bytes
    outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
    outframe.rtr = 0;               // No request
    outframe.data.bytes[0] = 0x01;
    outframe.data.bytes[1] = 0x02;
    outframe.data.bytes[2] = 0x03;
    outframe.data.bytes[3] = 0x04;
    outframe.data.bytes[4] = 0x05;
    outframe.data.bytes[5] = (Proximity);
    outframe.data.bytes[6] = (parameters.canControl);
    outframe.data.bytes[7] = (data7);
    Can1.sendFrame(outframe);
    data7 = (data7 + 1);            // Test if all packets are received
    if (data7 > 255)
      data7 = 0;
  }


  ////////////////////////////////////////////////////////////////////

  if (parameters.canControl == 1)
  {
    outframe.id = ControlID;
    outframe.length = 8;            // Data payload 8 bytes
    outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
    outframe.rtr = 0;                 //No request

    outframe.data.bytes[0] = 0;

    if (state != 0)
    {
      if ( slavechargerenable == 1)
      {
        outframe.data.bytes[0] = 0x01;
      }
    }

    outframe.data.bytes[1] = highByte(parameters.voltSet);
    outframe.data.bytes[2] = lowByte(parameters.voltSet);
    outframe.data.bytes[3] = highByte(maxdccur);
    outframe.data.bytes[4] = lowByte(maxdccur);
    outframe.data.bytes[5] = highByte(modulelimcur);
    outframe.data.bytes[6] = lowByte(modulelimcur);
    outframe.data.bytes[7] = 0;

    Can1.sendFrame(outframe);
  }
}

void evseread()
{
  uint16_t val = 0;
  val = analogRead(EVSE_PROX);     // read the input pin






  if ( parameters.type == 2)
  {
    if ( val > 950)
    {
      Proximity = Unconnected;
    }
    else
    {
      Proximity = Connected;
      if ( val < 950 && val > 800)
      {
        cablelim = 13000;
      }
      if ( val < 800 && val > 700)
      {
        cablelim = 20000;
      }
      if ( val < 600 && val > 450)
      {
        cablelim = 32000;
      }
      if ( val < 400 && val > 250)
      {
        cablelim = 63000;
      }
    }
  }

  if ( parameters.type == 1)
  {
    if ( val > 800)
    {
      Proximity = Unconnected;
    }
    else
    {
      if ( val > 550)
      {
        Proximity = Buttonpress;
      }
      else
      {
        Proximity = Connected;
      }
    }
  }
}

void Pilotread()
{
  Pilotcalc();
}

void Pilotcalc()
{
  if (  digitalRead(EVSE_PILOT ) == HIGH)
  {
    duration = micros() - pilottimer;
    pilottimer = micros();
  }
  else
  {
    accurlim = (micros() - pilottimer) * 100 / duration * 600; //Calculate the duty cycle then multiply by 600 to get mA current limit
  }
}

void ACcurrentlimit()
{
  if (parameters.autoEnableCharger == 1)
  {
    if (micros() - pilottimer > 1200) //too big a gap in pilot signal kills means signal error or disconnected so no current allowed.
    {
      accurlim = 0;
    }
    if (parameters.phaseconfig == 0)
    {
      modulelimcur = (accurlim / 3) * 1.5 ; // all module parallel, sharing AC input current
    }
    else
    {
      modulelimcur = accurlim * 1.5; // one module per phase, EVSE current limit is per phase
      if (modulelimcur > (cablelim * 1.5))
      {
        modulelimcur = cablelim * 1.5;
      }
    }
  }
  else
  {
    if (parameters.phaseconfig == 0)
    {
      modulelimcur = (parameters.currReq / 3); // all module parallel, sharing AC input current
    }
    else
    {
      modulelimcur = parameters.currReq;
    }
  }
  if (parameters.canControl == 1 | parameters.canControl == 2)
  {
    if (accurlim * 1.5 > (16000 * 1.5)) //enable second charger if current available >15A
    {
      modulelimcur = modulelimcur * 0.5;
      slavechargerenable = 1;

    }
    else
    {
      slavechargerenable = 0;
    }
  }

  if (parameters.phaseconfig == 1)
  {
    if (modulelimcur > (dcaclim * 1.5)) //if more current then max per module or limited by DC output current
    {
      // modulelimcur = (dcaclim * 1.5);
    }
    if (modulelimcur > parameters.currReq) //if evse allows more current then set in parameters limit it
    {
      modulelimcur = parameters.currReq;
    }
  }
  if (parameters.phaseconfig == 0)
  {
    if (modulelimcur > (dcaclim * 0.5)) //if more current then max per module or limited by DC output current
    {
      //modulelimcur = (dcaclim * 0.5);
    }
    if (modulelimcur > (parameters.currReq / activemodules)) //if evse allows more current then set in parameters limit it
    {
      modulelimcur = (parameters.currReq / activemodules);
    }
  }
  if (parameters.phaseconfig != 0 && parameters.phaseconfig != 1)
  {
    modulelimcur =  0;
  }
}

void DCcurrentlimit()
{
  totdccur = 0;
  for (int x = 0; x < 3; x++)
  {
    totdccur = totdccur + (dccur[x] * 0.1678466) ;
  }
  dcaclim = 0;
  int x = 0;
  if (totdccur > 0.2)
  {
    dcaclim = (((float)dcvolt[x] / (float)acvolt[x]) * (maxdccur * 1.2)) ; /// activemodules
  }
  else
  {
    dcaclim = 5000;
  }
}

void canextdecode(CAN_FRAME & frame)
{
  int x = 0;
  if (parameters.canControl == 2)
  {
    if (ElconControlID == frame.id) //Charge Control message
    {
      parameters.voltSet = ((frame.data.bytes[0] << 8) + frame.data.bytes[1]) * 0.1;
      maxdccur = (frame.data.bytes[2] << 8) + frame.data.bytes[3];

      if (frame.data.bytes[4] & 0x01 == 1)
      {
        if (state == 0)
        {
          state = 2;
          tboot = millis();
        }
      }
      else
      {
        state = 0;
      }
      if (debug == 1)
      {
        if (candebug == 1)
        {
          Serial.println();
          Serial.print( state);
          Serial.print(" ");
          Serial.print(parameters.voltSet);
          Serial.print(" ");
          Serial.print(modulelimcur);
          Serial.println();
        }
        tcan = millis();
      }
    }
  }

  if (parameters.canControl == 3)
  {
    if (ControlID == frame.id) //Charge Control message
    {
      if (frame.data.bytes[0] & 0x01 == 1)
      {
        if (state == 0)
        {
          state = 2;
          tboot = millis();
        }
      }
      else
      {
        if (millis() - slavetimeout > 1000)
        {
          slavetimeout = millis();
        }
        if (millis() - slavetimeout > 500)
        {
          state = 0;
        }
      }
      parameters.voltSet = (frame.data.bytes[1] << 8) + frame.data.bytes[2];
      maxdccur = (frame.data.bytes[3] << 8) + frame.data.bytes[4];
      modulelimcur  = (frame.data.bytes[5] << 8) + frame.data.bytes[6];
      if (debug == 1)
      {
        if (candebug == 1)
        {
          Serial.println();
          Serial.print( state);
          Serial.print(" ");
          Serial.print(parameters.voltSet);
          Serial.print(" ");
          Serial.print(modulelimcur);
          Serial.println();
        }
      }
      tcan = millis();
    }

  }

}

void menu()
{
  incomingByte = Serial.read(); // read the incoming byte:
  if (menuload == 1)
  {
    switch (incomingByte)
    {
      case 'q': //q for quit
        debug = 1;
        menuload = 0;
        break;

      case 'a'://a for auto enable
        candebug ++;
        if (candebug > 1)
        {
          candebug = 0;
        }
        menuload = 0;
        incomingByte = 'd';
        break;

      case 'b'://a for auto enable
        evsedebug ++;
        if (evsedebug > 1)
        {
          evsedebug = 0;
        }
        menuload = 0;
        incomingByte = 'd';
        break;


      case '1'://a for auto enable
        parameters.autoEnableCharger ++;
        if (parameters.autoEnableCharger > 1)
        {
          parameters.autoEnableCharger = 0;
        }
        menuload = 0;
        incomingByte = 'd';
        break;

      case '2'://e for enabling chargers followed by numbers to indicate which ones to run
        if (Serial.available() > 0)
        {
          parameters.enabledChargers = Serial.parseInt();
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '3'://a for can control enable
        if (Serial.available() > 0)
        {
          parameters.canControl = Serial.parseInt();
          if (parameters.canControl > 3)
          {
            parameters.canControl = 0;
          }
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '4'://t for type
        if (Serial.available() > 0)
        {
          parameters.type = Serial.parseInt();
          if (parameters.type > 2)
          {
            parameters.type = 2;
          }
          if (parameters.type == 0)
          {
            parameters.type = 2;
          }
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '5'://a for can control enable
        if (Serial.available() > 0)
        {
          parameters.phaseconfig = Serial.parseInt() - 1;
          if ( parameters.phaseconfig == 2)
          {
            parameters.phaseconfig = 1;
          }
          if (parameters.phaseconfig == 0)
          {
            parameters.phaseconfig = 0;
          }
          menuload = 0;
          incomingByte = 'd';
        }
        break;
      case '6'://v for voltage setting in whole numbers
        if (Serial.available() > 0)
        {
          parameters.voltSet = (Serial.parseInt() * 100);
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '7': //c for current setting in whole numbers
        if (Serial.available() > 0)
        {
          parameters.currReq = (Serial.parseInt() * 1500);
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '8': //c for current setting in whole numbers
        if (Serial.available() > 0)
        {
          parameters.can0Speed = long(Serial.parseInt() * 1000);
          Can1.begin(parameters.can0Speed);
          menuload = 0;
          incomingByte = 'd';
        }
        break;

      case '9': //c for current setting in whole numbers
        if (Serial.available() > 0)
        {
          parameters.can1Speed = long(Serial.parseInt() * 1000);

          Can1.begin(parameters.can1Speed);
          menuload = 0;
          incomingByte = 'd';
        }
        break;
      /*
        case 'c': //c for time
        if (Serial.available() > 0)
        {
        rtc_clock.set_time(Serial.parseInt(), Serial.parseInt(), Serial.parseInt());
        menuload = 0;
        incomingByte = 'd';
        }
        break;
        case 'd': //c for time
        if (Serial.available() > 0)
        {
        rtc_clock.set_date(Serial.parseInt(), Serial.parseInt(), Serial.parseInt());
        menuload = 0;
        incomingByte = 'd';
        }
        break;
      */
      case 't'://t for termaintion voltage setting in whole numbers
        if (Serial.available() > 0)
        {
          parameters.tVolt = (Serial.parseInt() * 100);
          menuload = 0;
          incomingByte = 'd';
        }
        break;





    }
  }

  if (menuload == 0)
  {
    switch (incomingByte)
    {
      case 's'://s for start AND stop
        digitalWrite(LED_BUILTIN, HIGH);
        state = 2;// initialize modules
        tboot = millis();
        break;

      case 'o':
        if (state > 0)
        {
          digitalWrite(LED_BUILTIN, LOW);
          state = 0;// initialize modules
        }
        break;

      case 'q': //q for quit
        EEPROM.write(0, parameters);
        debug = 1;
        menuload = 0;
        break;

      case 'd'://d for display
        debug = 0;
        menuload = 1;
        Serial.println();
        Serial.println();
        Serial.println();
        Serial.println();
        Serial.println("Settings Menu");
        Serial.print("1 - Auto Enable : ");
        if (parameters.autoEnableCharger == 1)
        {
          Serial.println("ON");
        }
        else
        {
          Serial.println("OFF");
        }
        Serial.print("2 - Modules Enabled : ");
        Serial.println(parameters.enabledChargers);
        Serial.print("3 - Can Mode : ");
        if (parameters.canControl == 0)
        {
          Serial.println(" Off ");
        }
        if (parameters.canControl == 1)
        {
          Serial.println(" Master ");
        }
        if (parameters.canControl == 2)
        {
          Serial.println(" Master Elcon ");
        }
        if (parameters.canControl == 3)
        {
          Serial.println(" Slave ");
        }
        Serial.print("4 - Port Type : ");
        Serial.println(parameters.type);
        Serial.print("5 - Phase Wiring : ");
        Serial.println(parameters.phaseconfig + 1);
        Serial.print("6 - DC Charge Voltage : ");
        Serial.print(parameters.voltSet * 0.01, 0);
        Serial.println("V");
        Serial.print("7 - AC Current Limit : ");
        Serial.print(parameters.currReq / 1500);
        Serial.println("A");
        Serial.print("8 - CAN0 Speed : ");
        Serial.println(parameters.can0Speed * 0.001, 0);
        Serial.print("9 - CAN1 Speed : ");
        Serial.println(parameters.can1Speed * 0.001, 0);
        Serial.print("a - Can Debug : ");
        if (candebug == 1)
        {
          Serial.println("ON");
        }
        else
        {
          Serial.println("OFF");
        }
        Serial.print("b - EVSE Debug : ");
        if (evsedebug == 1)
        {
          Serial.println("ON");
        }
        else
        {
          Serial.println("OFF");
        }
        /*
          Serial.print("c - time : ");
          Serial.print(rtc_clock.get_hours());
          Serial.print(":");
          Serial.print(rtc_clock.get_minutes());
          Serial.print(":");
          Serial.println(rtc_clock.get_seconds());
          Serial.print("d - date : ");
          Serial.print(daynames[rtc_clock.get_day_of_week() - 1]);
          Serial.print(": ");
          Serial.print(rtc_clock.get_days());
          Serial.print(".");
          Serial.print(rtc_clock.get_months());
          Serial.print(".");
          Serial.println(rtc_clock.get_years());
        */
        Serial.print("t - termination voltage : ");
        Serial.print(parameters.tVolt * 0.01, 0);
        Serial.println("V");
        Serial.println("q - To Quit Menu");
        break;
    }
  }
}
