// DingKey Designs Control Board
// 2/7/2024
#define SW_VERSION "v1.1.3"

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Rotary.h>
#include <regex>
#include <eng_format.hpp>
#include <movingAvgFloat.h>
#include <ESP_EEPROM.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include "ESPDashPro.h"

//TODO new feature EEPROM non-volatile memory for cycles and run time
//TODO new feature estimate time remaining
//TODO new feature status card for which limit hit counter or timer
//TODO new feature multiple display configurations select
//TODO new feature totalizer for cycles

//Refresh intervals, performance impact with higher refresh rates
const int       disp_interval       = 200;  //millis OLED display update interval 4Hz
const int       dash_interval       = 333;  //millis Web dashboard update interval
const unsigned long   encoder_interval    = 5000; //micros, rpm calcuation interval
// const int       memory_interval     = 10000;  //millis EEPROM save interval

//Power LED
#define ENABLE_PIN D8
#define ledPin LED_BUILTIN  // the number of the LED pin

int eepromInt = 123;
char ssidList[512];

struct LocalLan {
  char localssid[32];
  char localpass[32];
} MyLocalLan;

//Screen Setup
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET);
unsigned long disp_millis = 0;
#define LOGO16_GLCD_HEIGHT 16
#define LOGO16_GLCD_WIDTH  16
#define SPLASH1_TIME 3000 //millis, delay function
#define SPLASH2_TIME 4000 //millis, delay function

//Rotary Encoder
#define ROTARY_PIN1	D6
#define ROTARY_PIN2	D5
#define DIR_CW 0x10
#define DIR_CCW 0x20
Rotary r = Rotary(ROTARY_PIN1, ROTARY_PIN2);

//Motor Configruation 1, 11 pulses per rotation, divide by 2 two actuations per rotation
//170rpm:  Gear ratio 176rpm 1:34
#define MOTOR_CONFIG "170rpm"
#define STEPS_ROTATION 187.00

//Motor Configruation 2, 11 pulses per rotation, divide by 2 two actuations per rotation
//280rpm:  Gear Ratio 281rpm 1:21.3
//#define MOTOR_CONFIG "280rpm"
//#define STEPS_ROTATION 117.15

volatile unsigned long MotorEncoderPos = 0;
volatile unsigned long totalEncoderPos = 0;
volatile unsigned long total_micros = 1;
unsigned long lastEncoderPos = 0;
unsigned long last_micros = 0;
unsigned long Encoder_delta = 0;
unsigned long micros_delta = 0;
float cps       = 0; // cycles per second
float cps_avg   = 0; // cycles per second, filtered
float rpm       = 0; // calculated from smoothed cps_avg
float cph       = 0; // calculated from smoothed cps_avg, cycles per hour
char rpm_str[6];
char cph_str[10];
volatile double Cycles_done = 0; // max count with 1.0 precision is 16M on float
unsigned long Run_time_total = 0;
movingAvgFloat cps_mov_avg(100); // cycles per second average window based on encoder_interval in micros, if 5000micros interval 20 readings per second

//Motor Control
#define MOTOR_PWM D7
int pwm_command = 0;

//State Machine
int state       = 1;
bool run_enable = 1; //start in running status
int u_request   = 1;
int u_speed_target              = 100; //percentage beteween 30-100
const int u_speed_target_lim1   = 30;
const int u_speed_target_lim2   = 100;
float u_progress = 0; //percentage completion between 0-100%
unsigned long u_actuations_target = 0; //requested number of cycles
unsigned long u_timer_target = 0; //requested timer
unsigned long lastSecond = 0;

// Timer Class
class Switchtimer {
    public:
        Switchtimer();
        void set(unsigned long time);
        void update();
        void reset();
        float now();
        String timestring();

    private:
        unsigned long _start_millis;
        float _now;
        uint8 _seconds;
        uint8 _minutes;
        uint8 _hours;
        uint16 _days;
        char _timestr[20];
};

Switchtimer::Switchtimer(){
    _start_millis = 0;
    _now = 0.0;
    _seconds = 0;
    _minutes = 0;
    _hours = 0;
    _days = 0;
};

void Switchtimer::set(unsigned long time){
    _start_millis = time;
}

void Switchtimer::update(){
    if(millis()-_start_millis >= 1000)
    //if(micros()-timer_last >= 1000) //displaytesting only
    {
        _start_millis += 1000;
        _seconds++;
        _now += 1.0;
        
        if(_seconds > 59)
        {
            _seconds = 0;
            _minutes++;
        }
        if(_minutes > 59)
        {
            _minutes = 0;
            _hours++;
        }
        if(_hours > 23)
        {
            _hours = 0;
            _days++;
        }
    }
};

void Switchtimer::reset(){
    _start_millis = millis();
    _now = 0.0;
    _seconds = 0;
    _minutes = 0;
    _hours = 0;
    _days = 0;
};

float Switchtimer::now(){
    return _now;
};

String Switchtimer::timestring(){
    if (_days>0){
        snprintf(_timestr,20, "%ud\n%u:%02u:%02u", _days, _hours, _minutes, _seconds);
    }
    else if (_hours>0)
    {
        snprintf(_timestr,20, "%u:%02u:%02u", _hours, _minutes, _seconds);
    }
    else{
        snprintf(_timestr,20, "%um %us", _minutes, _seconds);
    }
    return String(_timestr);
};

Switchtimer Timer_ON;  //timer 1, machine total on time
Switchtimer Timer_RUN; //timer 2, machine run time only

String displayLargeNum(double num){
     // Format number for user display
     // Set number of displayed digits without trailing zeros, down to precision of 1.0

    int display_prec = 1; // for number of cycles display precision

    if (num<10){
        display_prec = 1;
    }
    else if (num<100)
    {
        display_prec = 2;
    }
    else if (num<1000)
    {
        display_prec = 3;
    }
    else if (num<10000)
    {
        display_prec = 4;
    }
    else if (num<100000)
    {
        display_prec = 5;
    }
    else if (num<1000000)
    {
        display_prec = 6;
    }
    else
    {
        display_prec = 7;
    }
    std::string num_str = to_engineering_string(Cycles_done,display_prec,eng_prefixed);
    
    return String(num_str.c_str());
}

//Wifi Setup
#ifndef APSSID
    #define APSSID "DingKeyWifi"
    #define APPSK  "keyboard"
#endif
const char *password = APPSK; //const char *ssid = APSSID;
char ssidRand[25]; //String ssidRand = APSSID;
char localssid[32];
char localpass[32];
IPAddress myIP;
IPAddress local_IP(10,10,10,1);
IPAddress gateway(10,10,1,1);
IPAddress subnet(255,255,255,0);
AsyncWebServer server(80); //ESP8266WebServer

// Dashboard Interface
ESPDash dashboard(&server); //Attach ESP-DASH to AsyncWebServer
unsigned long dash_millis = 0;
unsigned long dash_millis_delta = 0;

Card start_stop(&dashboard, BUTTON_CARD, "Start/Stop");
Card motor_speed(&dashboard, GENERIC_CARD, "Motor Speed", "rpm");
Card cycle_speed(&dashboard, GENERIC_CARD, "Actuations Speed", "per hour");
Card motor_speed_target(&dashboard, SLIDER_CARD, "Motor Speed", "%", 30, 100);

Card actuations_progress(&dashboard, PROGRESS_CARD, "Progress", "%", 0, 100);
Card actuations_input(&dashboard, TEXT_INPUT_CARD, "Target Actuations");
Card timer_target(&dashboard, TEXT_INPUT_CARD, "Timer (Hours:minutes, HH:MM)");
//Card local_lan_SSID(&dashboard, TEXT_INPUT_CARD, "WiFi Network Name");
Card local_lan_SSIDlist(&dashboard, DROPDOWN_CARD, "WiFi Network Name", &ssidList[0]);
Card local_lan_pass(&dashboard, PASSWORD_CARD, "WiFi Password");
Card local_lan_IP(&dashboard, GENERIC_CARD, "LAN IP");
Tab totals_tab(&dashboard, "Totals");
Card Cycles_total(&dashboard, GENERIC_CARD, "Total Actuation Cycles");
Card Machine_run_time(&dashboard, GENERIC_CARD, "Machine Run Time ");
Card Machine_on_time(&dashboard, ENERGY_CARD, "Machine On Time");
Card Reset_total(&dashboard, BUTTON_CARD, "Reset Cycles and Run Time");

// Statistic stata1(&dashboard, "Machine On Time", "-");
// Statistic stata2(&dashboard, "Machine Run Time", "-");
// Statistic stata3(&dashboard, "Machine Cycles", "-");

// Statistic statb1(&dashboard, "Last On Time", "-");
// Statistic statb2(&dashboard, "Last Run Time", "-");
// Statistic statb3(&dashboard, "Last Cycles", "-");
// Statistic statb4(&dashboard, "Total Cycles", "-");

Statistic lanIP(&dashboard, "LAN IP", "Not Connected");

void dashboardUpdateValues(){
    dash_millis_delta =  millis()-dash_millis;
    if (dash_millis_delta >= dash_interval) {
        dash_millis =  millis();
        start_stop.update(run_enable); //dashboard update
        motor_speed.update(rpm_str);
        cycle_speed.update(cph_str);
        actuations_progress.update(u_progress);
        Machine_on_time.update(Timer_ON.timestring());
        Machine_run_time.update(Timer_RUN.timestring());
        Cycles_total.update(displayLargeNum(Cycles_done));
        dashboard.sendUpdates();
    }
}

// EEPROM Save Config
// unsigned long memory_millis =  0;
// float last_on = 0;
// float last_run = 0;
// double last_cycles = 0;
// double total_cycles = 0;

// void saveMemory(){
//     if (millis()-memory_millis >= memory_interval){
//         memory_millis = millis();
//         EEPROM.put(0, Timer_ON.now()); // float
//         EEPROM.put(4, Timer_RUN.now()); // float
//         double Cycles_done_save = Cycles_done;
//         EEPROM.put(8, Cycles_done_save); // double
//         bool ok_commit = EEPROM.commit();
//         Serial.println((ok_commit) ? "Memory Commit OK" : "Commit failed");
//     }
// }

// Encoder interrupt
IRAM_ATTR void doMotorEncoder() {
  unsigned char mresult = r.process();
  if (mresult == DIR_CW || mresult == DIR_CCW) {
    MotorEncoderPos++;
    totalEncoderPos++;
    
    if (MotorEncoderPos >= STEPS_ROTATION) {  // 374=11 pulses per rev X 34 (gear ratio of the motor)
      Cycles_done += 1; // I incremented in twos but you can reduce it to 1 if pulses/rev is even number like 374 is. For increments of 1, replace 374 with 187
      MotorEncoderPos = 0;
    }
  }
}

// Encoder Hardware
void counterSetup() {
    pinMode(ROTARY_PIN1, INPUT_PULLUP);
    pinMode(ROTARY_PIN2, INPUT_PULLUP);
    r.begin(ROTARY_PIN1, ROTARY_PIN2);
    attachInterrupt(digitalPinToInterrupt(ROTARY_PIN1), doMotorEncoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ROTARY_PIN2), doMotorEncoder, CHANGE);
    total_micros = micros(); //prevents divide by zero
}

void setup() {

    Serial.begin(115200);

    //NEW
    EEPROM.begin(512);

    if(EEPROM.percentUsed()>=0) {
    /*
        EEPROM.get(0, verifyEEPROM);
        Serial.print("read data");
        Serial.println(verifyEEPROM);
        eepromInt=verifyEEPROM;
    */
        EEPROM.get(0, MyLocalLan);
        Serial.print("MyLocalLan: ");
        Serial.println(MyLocalLan.localssid);
        Serial.println(MyLocalLan.localpass);
        strcpy(localssid, MyLocalLan.localssid);
        strcpy(localpass, MyLocalLan.localpass);
        Serial.println(localssid);
        Serial.println(localpass);
    }

/*
    eepromInt++;
    EEPROM.put(0, eepromInt);
    boolean ok = EEPROM.commit();
    Serial.println((ok) ? "Commit OK" : "Commit failed");

    EEPROM.get(0, verifyEEPROM);
    Serial.print("eeprom data");
    Serial.println(verifyEEPROM);
*/

    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW); // LOW turns LED on

    //Splash Screen 1
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("DingKey");
    display.println("Designs");
    display.println("");
    display.println(MOTOR_CONFIG);
    display.println(SW_VERSION);
    display.display();

    //Wifi Access Point
    int id_suffix = 1;
    Serial.print("\nConfiguring access point...");
    snprintf(ssidRand,25,"%s-%04d",APSSID,id_suffix);

    // Example from https://arduino.stackexchange.com/questions/43044/esp8266-check-if-a-ssid-is-in-range
    int n = WiFi.scanNetworks();
    Serial.println("scan done");
    if (n == 0)
    {
        Serial.println("No networks found");
    }
    else
    {
        Serial.print(n);
        Serial.println(" Networks found");
        strcpy(ssidList,WiFi.SSID(0).c_str());
        Serial.println(WiFi.SSID(0));
        // Print list of wifi networks
        for (int i = 1; i < n; ++i){ // iterate through list of WiFi names
            Serial.println(WiFi.SSID(i)); // Print SSID and RSSI for each network found, not in alphabetical order
            strcat(ssidList,",");
            strcat(ssidList,WiFi.SSID(i).c_str());
        }
        local_lan_SSIDlist.update("", &ssidList[0]);
        
        // Find availalbe wifi name
        while (true){
            int matches = 0; // number of WiFi name matches
            for (int i = 0; i < n; ++i){ // iterate through list of WiFi names
                if (WiFi.SSID(i) == ssidRand) {
                    matches++;
                }
            }
            if (matches == 0){
                break;
            }
            id_suffix++;
            snprintf(ssidRand,25,"%s-%04d",APSSID,id_suffix); //update with newest increment
        }
    }

    Serial.println("Device WiFi AP Name:");
    Serial.println(ssidRand);

    //NEW HERE
    WiFi.mode(WIFI_AP_STA);

    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(ssidRand, password);
    //sprintf(localssid, "");
    //sprintf(localpass, "");
    //NEW HERE
    
    myIP = WiFi.softAPIP();
    Serial.println("HTTP server started");
    Serial.print("AP IP address: ");
    Serial.println(myIP);
    server.begin();
    
    //display.println(myIP);
    display.print("WiFi #");
    display.println(id_suffix);
    display.display();

    //Dashboard Setup
    dashboard.setTitle("DingKey Designs");

    start_stop.attachCallback([&](int value){
        u_request = value;
        start_stop.update(value);
        dashboard.sendUpdates();
    });
    
    /*local_lan_SSID.attachCallback([&](const char* value){
        Serial.println("SSID input"+String(value));
        strcpy(localssid, value);
        local_lan_SSID.update(value);
        dashboard.sendUpdates();
    });*/
    local_lan_SSIDlist.update("");
    local_lan_SSIDlist.attachCallback([&](const char* value){
        Serial.println("SSID input"+String(value));
        strcpy(localssid, value);
        local_lan_SSIDlist.update(value);
        dashboard.sendUpdates();
    });

    local_lan_pass.attachCallback([&](const char* value){
        Serial.println("Pass input"+String(value));
        strcpy(localpass, value);
        //local_lan_pass.update(value);
        //dashboard.sendUpdates();
    });

    motor_speed_target.attachCallback([&](int value){
        u_speed_target = value;
        motor_speed_target.update(value);
        dashboard.sendUpdates();
    });
    motor_speed_target.update(100); //default speed
    dashboard.sendUpdates();
    
    // Cycles input needs to be whole number
    actuations_input.attachCallback([&](const char* value){
        std::string u_actuations_target_str = value;
        std::regex time_expr("^\\d+$");
        std::smatch base_match;
        if (std::regex_match(u_actuations_target_str, base_match, time_expr)){
            u_actuations_target = std::stoul(u_actuations_target_str);
            if (u_actuations_target > 1e9){  //actuations limit
                u_actuations_target = 1e9;
            }
            u_actuations_target_str = std::to_string(u_actuations_target);
            actuations_input.update(u_actuations_target_str.c_str());
        }
        else{
            actuations_input.update("Check Input, Whole Numbers");
        }
        dashboard.sendUpdates();
    });
    
    // Timer input needs to be in HH:MM format
    timer_target.attachCallback([&](const char* value){

        std::string u_timer_target_str = value;
        //std::regex time_expr("^([0-1]?[0-9]|2[0-3]):[0-5][0-9]$"); // up to 23 hours, 59 minutes
        std::regex time_expr("^([0-9]|[1-9][0-9]|[1-4][0-9]{2}|500):[0-5][0-9]$"); // up to 500 hours, 59 minutes
        std::smatch base_match;
        if (std::regex_match(u_timer_target_str, base_match, time_expr)){
            std::string::size_type pos = u_timer_target_str.find(":");
            std::string u_timer_target_str_h = u_timer_target_str.substr(0, pos);  // before deliminter token
            std::string u_timer_target_str_m = u_timer_target_str.substr(pos + 1); // after delimiter token
            int u_timer_target_h = std::stoi(u_timer_target_str_h);
            int u_timer_target_m = std::stoi(u_timer_target_str_m);
            if (u_timer_target_m<10){ //pad additional 0 in output
                u_timer_target_str = std::to_string(u_timer_target_h) + ":" + "0" + std::to_string(u_timer_target_m);
            }
            else{
                u_timer_target_str = std::to_string(u_timer_target_h) + ":" + std::to_string(u_timer_target_m);
            }
            timer_target.update(u_timer_target_str.c_str());
            u_timer_target = (u_timer_target_h*3600 + u_timer_target_m*60)*1; // in seconds
        }
        else{
            timer_target.update("Check Input Format HH:MM");
        }
        dashboard.sendUpdates();
    });
    
    Reset_total.attachCallback([&](int value){
        state=0;
        run_enable=0;
        u_request=0;
        Cycles_done = 0; //reset number of cycles
        
        // Reset timer
        //Timer_ON.reset(); // Machine on time cannot be reset
        Timer_RUN.reset();

        start_stop.update(0);
        Reset_total.update(0); //return to zero after values are reset

        dashboard.sendUpdates();
    });

    Cycles_total.setTab(&totals_tab);
    Machine_run_time.setTab(&totals_tab);
    Reset_total.setTab(&totals_tab);
    Machine_on_time.setTab(&totals_tab);

    start_stop.update((int) u_request); // initial state is machine running, without any user input
    dashboard.sendUpdates();

    // // EEPROM Setup
    // EEPROM.begin(24); // float, float, double, double
    // EEPROM.get(0, last_on);
    // EEPROM.get(4, last_run);
    // EEPROM.get(8, last_cycles);
    // EEPROM.get(16, total_cycles);
    
    // Serial.println(last_on);
    // Serial.println(last_run);
    // Serial.println(last_cycles);
    // Serial.println(total_cycles);
    
    // total_cycles = total_cycles + last_cycles; // totalizer
    // EEPROM.put(16, total_cycles);
    // bool ok_commit = EEPROM.commit();
    // Serial.println((ok_commit) ? "Totalzier Commit OK" : " Totalizer Commit failed");

    // statb1.set("Last On Time", (std::to_string(last_on)).c_str());
    // statb2.set("Last Run Time", (std::to_string(last_on)).c_str());
    // statb3.set("Last Cycles", (std::to_string(last_on)).c_str());
    // statb4.set("Total Cycles", (std::to_string(last_on)).c_str());

    //Splash Screen 2
    delay(SPLASH1_TIME); // non blocking for wifi initialization
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println(">Cycles");
    display.println(">RPM");
    display.println(">Cycles/hr");
    display.println(">Run Time");
    display.println(">Access IP");
    display.println(myIP);
    display.display();
    delay(SPLASH2_TIME); // non blocking for wifi initialization

    // Clear the buffer.
    display.clearDisplay();
    display.display();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);

    // Encoder Setup
    counterSetup();
    cps_mov_avg.begin();

    // Timer Setup
    Timer_ON.reset(); //reset ON timer to current millis()
    Timer_RUN.reset(); //reset ON timer to current millis()
}


void loop() {
    // Encoder Calculation
    total_micros = micros();
    micros_delta =  total_micros - last_micros; // uint subtraction overflow protection
    if (micros_delta > encoder_interval){
        Encoder_delta =  totalEncoderPos - lastEncoderPos; // uint subtraction overflow protection
        cps = abs(float(Encoder_delta) / float(STEPS_ROTATION) / (float(micros_delta)/1.00E6)); //cycles per second
        if  (cps <= 0.5){ // 1 cycle per second = 30 rpm, 0.5 cycle per second = 15 rpm
            cps_mov_avg.reset(); // No moving average under threshold, resets moving average numerical error build up
        }
        cps_avg = cps_mov_avg.reading(cps); // moving average filter
        cph = cps_avg * 3600.0; // cycles per second *60 *60 = cycles per hour
        rpm = cps_avg * 60.0 / 2.0; // cycles per second * 60 / 2 cycles per rotation
        
        lastEncoderPos = totalEncoderPos;
        last_micros = total_micros;
    }

    // Refresh OLED display
    if (millis()-disp_millis >= disp_interval) { //throttled for performance
        disp_millis = millis();
        display.clearDisplay();
        display.setCursor(0,0);
        
        display.println(displayLargeNum(Cycles_done));
        
        // Line 1
        display.print("RPM:");
        snprintf(rpm_str,6,"%.1f",abs(rpm));
        display.println(rpm_str);
        
        // Line 2
        display.print("Hr:");
        snprintf(cph_str,10,"%.0f",abs(cph));
        display.println(cph_str);
        
        // Line 3
        display.println(Timer_RUN.timestring());

        // Line 4
        if ( WiFi.status() == WL_CONNECTED ) {
            display.println(WiFi.localIP());
        } else {
            display.println(myIP);
        }
        
        display.display();
    };

    //State Machine Logic
    switch (state)
    {
    case 0: // Idle
        if (u_request){
            state=1;
            Timer_RUN.set(millis()); //set start time for timer on transition to run_enable
        }
        run_enable=0;
        break;
    
    case 1: // Machine Run
        if (u_actuations_target>0){
            state=2;
        }
        else if (u_timer_target>0){
            state=3;
        }
        if (!u_request){
            state=0;
        }
        run_enable=1;
        break;
    
    case 2: // Counter active
        u_progress = (float)Cycles_done / (float)u_actuations_target*100.0;
        if (u_progress>=100.0){u_progress = 100;}
        if (!u_request || Cycles_done>=u_actuations_target){
            state=0;
            run_enable=0;
            u_request=0;
            start_stop.update(u_request); //dashboard update
            dashboard.sendUpdates();
        }
        else if (u_timer_target>0){
            state=4;
        }
        run_enable=1;
        break;
    
    case 3: // Timer Active
        u_progress = Timer_RUN.now() / (float)u_timer_target*100.0;
        if (u_progress>=100.0){u_progress = 100;}
        if (!u_request || Timer_RUN.now()>=u_timer_target){
            state=0;
            run_enable=0;
            u_request=0;
            start_stop.update(u_request); //dashboard update
            dashboard.sendUpdates();
        }
        else if (u_actuations_target>0)
        {
            state=4;
        }
        run_enable=1;
        break;

    case 4: //Counter and Timer Active
        u_progress = std::max( ((float)Cycles_done/(float)u_actuations_target*100.0), (Timer_RUN.now()/(float)u_timer_target*100.0) );
        if (u_progress>=100.0){u_progress = 100;}
        if (!u_request || u_progress>=100.0){
            state=0;
            run_enable=0;
            u_request=0;
            start_stop.update(u_request); //dashboard update
            dashboard.sendUpdates();
        }
        if (u_timer_target <= 0){
            state = 2; //counter only
        }
        if (u_actuations_target <= 0){
            state = 3; //timer only
        }
        run_enable = 1;
        break;
    }
    
    // Motor Control
    // Speed targets between 30% and 100%
    if (u_speed_target < u_speed_target_lim1){
        u_speed_target = u_speed_target_lim1;
    }
    else if (u_speed_target > u_speed_target_lim2){
        u_speed_target = u_speed_target_lim2;
    }
    
    pwm_command = map(u_speed_target,0,100,0,255); // from 0-100 to 0-255
    analogWrite(MOTOR_PWM,pwm_command*run_enable); //multiply by run_enable to disable motor output when not enabled

    // Timer Update Values
    Timer_ON.update();
    if (run_enable){
        Timer_RUN.update();
    }

    if ( strlen(localssid) && strlen(localpass) ) {
        int retry = 0; 
        WiFi.begin(localssid, localpass);

        local_lan_IP.update((String)"WiFi Connecting...");
        dashboard.sendUpdates();
    
        while (WiFi.status() != WL_CONNECTED && retry < 20) {
            delay(500);
            Serial.print(".");
            retry++;
        }

        if ( WiFi.status() == WL_CONNECTED ) {
            Serial.println(WiFi.localIP());
            strcpy(MyLocalLan.localssid, localssid);
            strcpy(MyLocalLan.localpass, localpass);
            EEPROM.put(0, MyLocalLan);
            boolean ok = EEPROM.commit();
            if ( !ok ) {
                Serial.println("Commit failed");
            }     
            strcpy(localssid, "");
            strcpy(localpass, "");    

            lanIP.set((WiFi.localIP().toString()).c_str());
            local_lan_IP.update(WiFi.localIP().toString());
        } else {
            Serial.println("WiFi Connection Failed");     
            strcpy(MyLocalLan.localssid, "");
            strcpy(MyLocalLan.localpass, "");
            EEPROM.put(0, MyLocalLan);
            boolean ok = EEPROM.commit();
            if ( !ok ) {
                Serial.println("Commit failed");
            }  
            strcpy(localpass, "");    

            local_lan_IP.update((String)"WiFi Connection Failed");
        }

        
//        lanIP.set((WiFi.localIP().toString()).c_str());
//        local_lan_IP.update(WiFi.localIP().toString());
        dashboard.sendUpdates();
    }
    // Commit to flash memory for power-off
    // saveMemory();

    // Web Dashboard Update
    dashboardUpdateValues(); //interval controlled within function

    yield();

    // Debug commands
    // Serial.println("debug");
    // Serial.println(state);
    // Serial.println(run_enable);
    // Serial.println(Run_time);
    // Serial.println(u_timer_target);
    // Serial.println(now()); // returns the current time as seconds since Jan 1 1970
    // Serial.println(pwm_command);
    // Serial.println(pwm_command*run_enable);

    // Debug Simulated Cycles
    //if (run_enable){
    //   Cycles_done += 10; //displaytesting only, simulated cycles
    //}

    /*eepromInt++;
    EEPROM.put(0, eepromInt);
    boolean ok = EEPROM.commit();
    if ( !ok ) {
        Serial.println("Commit failed");
    }*/
/*
    if ( ok ) {
//        char display_str[20];
        int readBack;
//        sprintf(display_str, "%d", eepromInt);
//        display.println(display_str);
        EEPROM.get(0, readBack);
//        Serial.print("new data");
//        Serial.println(readBack);
    } else {
//        display.println("Commit failed");
        Serial.println("Commit failed");
    }
*/
}