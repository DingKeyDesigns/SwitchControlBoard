// DingKey Designs Control Board
// 12/31/2023
#define SW_VERSION "v1.1.0beta"

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Rotary.h>
#include <regex>
#include <eng_format.hpp>
#include <movingAvgFloat.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <ESPDashPro.h>
//TODO new feature EEPROM non-volatile memory for cycles and run time
//TODO new feature counter for run-time and on-time
//TODO new feature estimate time remaining
//TODO new feature status card for which limit hit counter or timer
//TODO new feature multiple display configurations select

//Refresh intervals, performance impact with higher refresh rates
const int       disp_interval       = 250;  //millis OLED display update interval 4Hz
const int       dash_interval       = 350;  //millis Web dashboard update interval
unsigned long   encoder_interval    = 5000; //micros, rpm calcuation interval

//Screen Setup
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET);
unsigned long disp_millis = 0;
#define LOGO16_GLCD_HEIGHT 16
#define LOGO16_GLCD_WIDTH  16

//Power LED
#define ENABLE_PIN D8
#define ledPin LED_BUILTIN  // the number of the LED pin

//Wifi Setup
#ifndef APSSID
    #define APSSID "DingKeyWifi"
    #define APPSK  "keyboard"
#endif
const char *password = APPSK; //const char *ssid = APSSID;
char ssidRand[25]; //String ssidRand = APSSID;
IPAddress myIP;
IPAddress local_IP(10,10,10,1);
IPAddress gateway(10,10,1,1);
IPAddress subnet(255,255,255,0);
AsyncWebServer server(80); //ESP8266WebServer

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

Tab totals_tab(&dashboard, "Totals");
Card Cycles_total(&dashboard, GENERIC_CARD, "Total Actuation Cycles");
Card Machine_run_time(&dashboard, GENERIC_CARD, "Machine Run Time ");
Card Reset_total(&dashboard, BUTTON_CARD, "Reset All Totals");
Card Machine_on_time(&dashboard, GENERIC_CARD, "Machine On Time");

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
    Serial.print("Configuring access point...");
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
        Serial.println("Networks found");
        for (int i = 0; i < n; ++i)
        {
        Serial.println(WiFi.SSID(i)); // Print SSID and RSSI for each network found
            if(WiFi.SSID(i) == ssidRand) //enter the ssid that matches assigned name
            {
                Serial.println("Existing DingKey network found");
                id_suffix++; // increment to next wifi suffix
                snprintf(ssidRand,25,"%s-%04d",APSSID,id_suffix); //update with newest increment
            }
        }
    }

    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(ssidRand, password);
    myIP = WiFi.softAPIP();
    Serial.println("HTTP server started");
    Serial.print("AP IP address: ");
    Serial.println(myIP);
    server.begin();
    display.println(myIP);
    display.display();

    //Dashboard Setup
    dashboard.setTitle("DingKey Designs");

    start_stop.attachCallback([&](int value){
        u_request = value;
        start_stop.update(value);
        dashboard.sendUpdates();
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

    //Splash Screen 2
    delay(2000);
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
    delay(5000);

    // Clear the buffer.
    display.clearDisplay();
    display.display();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);

    counterSetup();
    cps_mov_avg.begin();

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
        display.println(myIP);

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
    }

