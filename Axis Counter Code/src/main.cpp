// DingKey Designs Control Board
// V0.0.1
// 11/26/2023

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Rotary.h>
#include <regex>
#include <TimeLib.h>
#include <time.h>
#include <eng_format.hpp>
#include <movingAvgFloat.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <ESPDashPro.h>

/* Unused variables
// SCL GPIO5
// SDA GPIO4
#define NUMFLAKES 10
#define XPOS 0
#define YPOS 1
#define DELTAY 2
unsigned long Old_Cycles_done = 0;
unsigned long last_millis = 0;
byte percent = 0;
byte pwm = 255;
const char* PARAM_INPUT_1 = "pwm";
const char* PARAM_INPUT_2 = "cycles";
const char* PARAM_INPUT_3 = "input3";
String inputMessageFinal = "testMessage";
int requestFlag = 0;
volatile unsigned long requestedCycles = 0;
#define CLICKS_PER_STEP 1s
*/

//Screen Setup
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET);
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
/*
void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}
*/

//Rotary Encoder
#define ROTARY_PIN1	D6
#define ROTARY_PIN2	D5
//#define STEPS_ROTATION 187.00 //170rpm  11 pulses per rotation, Gear ratio 176rpm 1:34, Gear Ratio 281rpm 1:21.3, divide by 2 two actuations per rotation
#define STEPS_ROTATION 117.15 //281rpm 11 pulses per rotation, Gear ratio 176rpm 1:34, Gear Ratio 281rpm 1:21.3, divide by 2 two actuations per rotation
Rotary r = Rotary(ROTARY_PIN1, ROTARY_PIN2);
#define DIR_CW 0x10
#define DIR_CCW 0x20
volatile unsigned long MotorEncoderPos = 0;
volatile unsigned long totalEncoderPos = 0;
volatile unsigned long total_micros = 1;
unsigned long lastEncoderPos = 0;
unsigned long last_micros = 0;
unsigned long Encoder_delta = 0;
unsigned long micros_delta = 0;
float cps = 0; // cycles per second
float cps_avg = 0; // cycles per second, filtered
movingAvgFloat cps_mov_avg(50);
float rpm = 0; // calculated from smoothed cps_avg
char rpm_str[6];
float cph = 0; // calculated from additional smoothed cps_avg, cycles per hour
char cph_str[10];
volatile double Cycles_done = 0; // max count with 1.0 precision is 16M on float
//volatile double Cycles_done_total = 0; // not resettable unless powered down or reset buton on totals page TODO
unsigned long Run_time = 0;
unsigned long Run_time_total = 0;  // not resettable unless powered down
char Run_time_total_str[15];
unsigned long timer_start = 0;

//TODO eeprom non-volatile memeory for cycle time count

//Motor Control
#define MOTOR_PWM D7
int pwm_command = 0;

//State Machine
int state=0;
bool run_enable = 1; //start in running status

int u_request = 0;
int u_speed_target = 100; //percentage beteween 30-100
const int u_speed_target_lim1 = 30;
const int u_speed_target_lim2 = 100;
float u_progress = 0; //percentage completion between 0-100%
unsigned long u_actuations_target = 0; //requested number of cycles
unsigned long u_timer_target = 0; //requested timer

// Dashboard Interface
ESPDash dashboard(&server); //Attach ESP-DASH to AsyncWebServer
unsigned long dash_millis = 0;
unsigned long dash_millis_delta = 0;
const int dash_interval = 350;//update interval millis

Card start_stop(&dashboard, BUTTON_CARD, "Start/Stop");
Card motor_speed(&dashboard, GENERIC_CARD, "Motor Speed", "rpm");
//Card cycle_speed(&dashboard, GENERIC_CARD, "Actuations Speed", "per hour");
Card motor_speed_target(&dashboard, SLIDER_CARD, "Motor Speed", "%", 30, 100);

Card actuations_progress(&dashboard, PROGRESS_CARD, "Progress", "%", 0, 100);
//Card actuations_target(&dashboard, SLIDER_CARD, "Target Actuations", "", 0, 1000000);
Card actuations_target(&dashboard, SLIDER_CARD, "Target Actuations", "", 0, 1000); //displaytesting only
Card timer_target(&dashboard, TEXT_INPUT_CARD, "Timer (Hours:minutes, HH:MM)");

Tab totals_tab(&dashboard, "Totals");
Card Cycles_total(&dashboard, GENERIC_CARD, "Total Actuation Cycles");
Card Run_total(&dashboard, GENERIC_CARD, "Total Run Time");
Card Reset_total(&dashboard, BUTTON_CARD, "Reset All Totals");

IRAM_ATTR void doMotorEncoder() {
  unsigned char mresult = r.process();
  if (mresult == DIR_CW || mresult == DIR_CCW) {
    MotorEncoderPos++;
    totalEncoderPos++;
    //total_micros = micros(); // record time of measurement
    if (MotorEncoderPos >= STEPS_ROTATION) {  // 374=11 pulses per rev X 34 (gear ratio of the motor)
      Cycles_done += 1; // I incremented in twos but you can reduce it to 1 if pulses/rev is even number like 374 is. For increments of 1, replace 374 with 187
      //Cycles_done_total += 1;
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

void time_string(){
    if (day()-1>0){
        snprintf(Run_time_total_str,15, "%u:%u:%u:%u", day()-1, hour(), minute(), second());
    }
    else if (hour()>0)
    {
        snprintf(Run_time_total_str,15, "%u:%u:%u", hour(), minute(), second());
    }
    else{
        snprintf(Run_time_total_str,15, "%um %us", minute(), second());
    }
}

String displayLargeNum(double num){
     // Format number for user display
     // Set number of displayed digits without trailing zeros, down to precision of 1.0

    int display_prec = 1; // for number of cycles display precision

    if (num<10){
        display_prec = 1;
    }
    else if (num<100)
    {
        num = 2;
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
    std::string num_str = to_engineering_string(Cycles_done,display_prec, eng_prefixed);
    
    return String(num_str.c_str());
}

void setup() {
    Serial.begin(115200);

    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW); // LOW turns LED on

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
    //server.on("/", handleRoot);
    server.begin();
    
    //Dashboard Setup
    dashboard.setTitle("DingKey Designs");

    start_stop.attachCallback([&](int value){
        /* Print our new button value received from dashboard */
        //Serial.println("Button Triggered: "+String((value)?"true":"false"));
        /* Make sure we update our button's value and send update to dashboard */
        u_request = value;
        start_stop.update(value);
        dashboard.sendUpdates();
    });
    start_stop.update(1); // initial state is machine running, without any user input

    motor_speed_target.attachCallback([&](int value){
        //Serial.println("[Card1] Slider Callback Triggered: "+String(value));
        u_speed_target = value;
        motor_speed_target.update(value);
        dashboard.sendUpdates();
    });
    motor_speed_target.update(100); //default speed
    
    actuations_target.attachCallback([&](int value){
        //Serial.println("[Card1] Slider Callback Triggered: "+String(value));
        //value = value/1000*1000; // round to nearest 1000 disabled for displaytesting only
        u_actuations_target = value;
        actuations_target.update(value);
        dashboard.sendUpdates();
    });
    // Timer input needs to be in HH:MM format
    timer_target.attachCallback([&](const char* value){

        std::string u_timer_target_str = value;
        std::regex time_expr("^([0-1]?[0-9]|2[0-3]):[0-5][0-9]$");
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
            timer_target.update(String(u_timer_target_str.c_str()));
            u_timer_target = (u_timer_target_h*3600 + u_timer_target_m*60)*1000; // in millis
        }
        else{
            timer_target.update("Check Input Format HH:MM");
        }
        dashboard.sendUpdates();
    });

    Run_total.setTab(&totals_tab);
    Cycles_total.setTab(&totals_tab);
    Reset_total.setTab(&totals_tab);

    // by default, we'll generate the high voltage from the 3.3v line internally! (neat!)
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("DingKey");
    display.println("Designs");
    display.println("------");
    display.println("v0.0.1");
    display.println(myIP);
    display.display();
    delay(2000);

    // Clear the buffer.
    display.clearDisplay();
    display.display();
    counterSetup();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    
    cps_mov_avg.begin();
}

void loop() {
    //server.handleClient();
    
    total_micros = micros();
    Encoder_delta =  totalEncoderPos - lastEncoderPos; // uint subtraction overflow protection
    micros_delta =  total_micros - last_micros; // uint subtraction overflow protection

    cps = float(Encoder_delta) / float(STEPS_ROTATION) / (float(micros_delta)/1.00E6); //cycles per second
    cps_avg = cps_mov_avg.reading(cps); // moving average filter
    cph = cps_avg * 3600.0; // cycles per second *60 *60 = cycles per hour
    rpm = cps_avg * 60.0 / 2.0; // cycles per second * 60 / 2 cycles per rotation
    
    lastEncoderPos = totalEncoderPos;
    last_micros = total_micros;

    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    
    display.println(displayLargeNum(Cycles_done));

    display.print("RPM:");
    snprintf(rpm_str,6,"%.1f",rpm);
    display.println(rpm_str);
    snprintf(cph_str,10,"%.0f",cph);
    display.println(cph_str);
    //Serial.println(rpm);
    //Serial.println(total_micros);
    

    time_string(); //update display time string
    display.println(Run_time_total_str);

    display.println(myIP);
    display.display();

    display.clearDisplay();
    //analogWrite(D7, pwm);
    //r.loop();
    
    // Motor Control
    // Speed targets between 30% and 100%
    if (u_speed_target < u_speed_target_lim1){
        u_speed_target = u_speed_target_lim1;
    }
    else if (u_speed_target > u_speed_target_lim2){
        u_speed_target = u_speed_target_lim2;
    }
    pwm_command = map(u_speed_target,0,100,0,255);

    //State Machine Logic
    switch (state)
    {
    case 0: // Idle
        run_enable=0;
        if (u_request){
            state=1;
        }
        break;
    
    case 1: // Machine Run
        run_enable=1;
        if (u_actuations_target>0){
            state=2;
            //Cycles_done=0;
            //u_progress=0; //Reset progress upon entering state
        }
        else if (u_actuations_target>0){
            state=3;
            timer_start=millis();
            u_progress=0; //Reset progress upon entering state
        }
        if (!u_request){
            state=0;
        }
        break;
    
    case 2: // Counter active
        run_enable=1;
        u_progress = (float)Cycles_done / (float)u_actuations_target*100.0;
        if (u_progress>100.0){
            u_progress = 100;
        }
        if (!u_request || Cycles_done>=u_actuations_target){
            state=0;
            run_enable=0;
            u_request=0;
            start_stop.update(u_request); //dashboard update
            dashboard.sendUpdates();
        }
        else if (u_timer_target>0){
            state=3;
            timer_start=millis();
            u_actuations_target=0;
            u_progress=0; //Reset progress upon entering state
        }
        break;
    
    case 3: // Timer Active
        run_enable=1;
        Run_time = millis() - timer_start;
        u_progress = (float)Run_time / (float)u_timer_target*100.0;
        
        if (!u_request || Run_time>=u_timer_target){
            state=0;
            run_enable=0;
            u_request=0;
            start_stop.update(u_request); //dashboard update
            dashboard.sendUpdates();
        }
        else if (u_actuations_target>0)
        {
            state=2;
            u_timer_target=0;
            u_progress=0; //Reset progress upon entering state
        }
        break;
    }
    Serial.println("debug");
    Serial.println(state);
    Serial.println(run_enable);
    Serial.println(Run_time);
    Serial.println(u_timer_target);

    //Motor Command
    //Serial.println(pwm_command*run_enable);
    analogWrite(MOTOR_PWM,pwm_command*run_enable); //multiply by run_enable to disable motor output when not enabled
    //Simulated Cycles
    if (run_enable){
        Cycles_done += 101; //displaytesting only, simulated cycles
    }

    dash_millis_delta =  millis()-dash_millis;
    if (dash_millis_delta>= dash_interval) {
        dash_millis =  millis();
        start_stop.update(run_enable); //dashboard update
        motor_speed.update(rpm_str);
        //cycle_speed.update(cph_str);
        actuations_progress.update(u_progress);
        Run_total.update(Run_time_total_str);
        Cycles_total.update(displayLargeNum(Cycles_done));

        dashboard.sendUpdates();
    }

    yield();

    }

