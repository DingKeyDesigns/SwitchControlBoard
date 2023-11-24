#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <Rotary.h>

// SCL GPIO5
// SDA GPIO4
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET);

#define NUMFLAKES 10
#define XPOS 0
#define YPOS 1
#define DELTAY 2

#define LOGO16_GLCD_HEIGHT 16
#define LOGO16_GLCD_WIDTH  16

#ifndef APSSID
#define APSSID "DingKeyWifi"
#define APPSK  "deeznuts"
#endif

#define ROTARY_PIN1	D6
#define ROTARY_PIN2	D5
#define CLICKS_PER_STEP 1
#define STEPS_ROTATION 234.0

#define ENABLE_PIN D8
#define ledPin LED_BUILTIN  // the number of the LED pin

//ESPRotary r;
Rotary r = Rotary(ROTARY_PIN1, ROTARY_PIN2);
// Clockwise step.
#define DIR_CW 0x10
// Counter-clockwise step.
#define DIR_CCW 0x20

unsigned long last_micros = 0;
unsigned long lastEncoderPos = 0;
float rpm = 0;

volatile unsigned long MotorEncoderPos = 0;
volatile unsigned long totalEncoderPos = 0;
volatile float Cycles_done = 0;

unsigned long Encoder_delta =  0;
unsigned long micros_delta =  0;

unsigned long Old_Cycles_done = 0;
volatile unsigned long requestedCycles = 0;
byte percent = 0;
int requestFlag = 0;

//Ticker t;
byte pwm = 20;

unsigned long last_millis = 0;

/* Set these to your desired credentials. */
//const char *ssid = APSSID;
const char *password = APPSK;
//String ssidRand = APSSID;
char ssidRand[25];
IPAddress myIP;
IPAddress local_IP(10,10,10,1);
IPAddress gateway(10,10,1,1);
IPAddress subnet(255,255,255,0);

const char* PARAM_INPUT_1 = "pwm";
const char* PARAM_INPUT_2 = "cycles";
const char* PARAM_INPUT_3 = "input3";

String inputMessageFinal = "testMessage";

// HTML web page to handle 3 input fields (input1, input2, input3)
/*const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP Input Form</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  <form action="/get">
    pwmInput <input type="text" name="pwm">
    <input type="submit" value="Submit">
  </form><br>
  <form action="/get">
    cyclesInput <input type="text" name="cycles">
    <input type="submit" value="Submit">
  </form><br>
  <form action="/get">
    input3: <input type="text" name="input3">
    <input type="submit" value="Submit">
  </form>
</body></html>)rawliteral";
*/
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP Input Form</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  <form action="/get">
    Speed Percentage (0-100)<input type="number" name="pwm" min="0" max="100"><br>
    Cycles to Run<input type="number" name="cycles" min="1"><br>
    placeholder<input type="number" name="input3"><br><br>
    <input type="submit" value="Submit">
  </form>
</body></html>)rawliteral";

//ESP8266WebServer server(80);
AsyncWebServer server(80);

/* Just a little test message.  Go to http://192.168.4.1 in a web browser
   connected to this access point to see it.
*/
/*void handleRoot() {
  server.send(200, "text/html", "< h1 >You are connected");
}*/

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
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
  Serial.println("\n\nSimple Counter");
}

void setup() {
  pinMode(ROTARY_PIN1, INPUT_PULLUP);
  pinMode(ROTARY_PIN2, INPUT_PULLUP);
  delay(1000);
  Serial.begin(115200);
  Serial.println();

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW); // LOW turns LED on
  
  Wire.begin();
  
  /*pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, HIGH);
  delay(2000);
  digitalWrite(ENABLE_PIN, LOW);
  delay(2000);
  digitalWrite(ENABLE_PIN, HIGH);
  delay(2000);
  digitalWrite(ENABLE_PIN, LOW);
  delay(2000);*/

  Serial.print("Configuring access point...");
  int randSSID = random(1,9999);
  snprintf(ssidRand,25,"%s-%04d",APSSID,1);
  Serial.println(ssidRand);
  //ssidRand = ssidRand + randSSID;
  //ssid = ssidRand;
  /* You can remove the password parameter if you want the AP to be open. */
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssidRand, password);

  myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);
  //server.on("/", handleRoot);
  //server.begin();
  Serial.println("HTTP server started");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  // Send a GET request to <ESP_IP>/get?input1=<inputMessage>
  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String inputA;
    String inputB;
    String inputC;
    String inputParam;
    String inputParam1;
    String inputParam2;
    String inputParam3;
    // GET input1 value on <ESP_IP>/get?input1=<inputMessage>
    if (request->hasParam(PARAM_INPUT_1)) {
      inputA = request->getParam(PARAM_INPUT_1)->value();
      if (inputA[0] != '\0') {
        percent = inputA.toInt();
        inputParam1 = PARAM_INPUT_1;
        requestFlag = 1;
        //pwm = (float)percent / 100 * 255;
        pwm = (1 - (float)percent / 100) * 127;
      }
      //analogWrite(D7, pwm);
      Wire.beginTransmission(0x2E);
      Wire.write(byte(0x00));
      Wire.write(pwm);
      Wire.endTransmission();

    }
    // GET input2 value on <ESP_IP>/get?input2=<inputMessage>
    if (request->hasParam(PARAM_INPUT_2)) {
      inputB = request->getParam(PARAM_INPUT_2)->value();
      if (inputB[0] != '\0') {
        inputParam2 = PARAM_INPUT_2;
        requestFlag = 1;
        requestedCycles = inputB.toInt();
      }
    }
    // GET input3 value on <ESP_IP>/get?input3=<inputMessage>
    if (request->hasParam(PARAM_INPUT_3)) {
      inputC = request->getParam(PARAM_INPUT_3)->value();
      inputParam3 = PARAM_INPUT_3;
      requestFlag = 1;
    }
    else {
      inputA = "No message sent";
      inputParam = "none";
    }
    Serial.println(inputA);
    inputMessageFinal = inputA;
    inputParam1 = "";
    request->send(200, "text/html", "HTTP GET request sent to your ESP on input field <br>Speed Percentage: " 
                                     + inputParam1 + percent + "<br>" 
                                     + "Cycles to Run" + ": " + requestedCycles + "<br>"
                                     + inputParam3 + ": " + inputC + "<br>"
                                     + "<br><a href=\"/\">Return to Home Page</a>");
  });
  server.onNotFound(notFound);
  server.begin();

  // by default, we'll generate the high voltage from the 3.3v line internally! (neat!)
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)
  // init done

  //display.display();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("abcdef");
  display.println("------");
  display.println(myIP);
  display.display();
  delay(2000);

  // Clear the buffer.
  display.clearDisplay();
  display.display();
  counterSetup();
  // text display tests
  /*display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println(inputMessageFinal);
  display.display();
  delay(2000);
  display.clearDisplay();*/
}

void loop() {
  //server.handleClient();
  // text display tests

  // Original code:
  // rpm = (((float)totalEncoderPos - (float)lastEncoderPos) / 234) *60.0 * (1000/(millis()-last_millis));
  // lastEncoderPos = totalEncoderPos;
  // last_millis = millis();

  Encoder_delta =  totalEncoderPos - lastEncoderPos; // uint subtraction overflow protection
  micros_delta =  micros()-last_micros; // uint subtraction overflow protection

  rpm = float(Encoder_delta) / float(STEPS_ROTATION) / (float(micros_delta)/1.00E6) * 60.0;
  lastEncoderPos = totalEncoderPos;
  last_micros = micros();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  String displayCycles = "Cyc:";
  //displayCycles = displayCycles + Cycles_done;
  display.print(displayCycles);
  display.println(Cycles_done);
  display.print("rpm:");
  display.println(rpm);
  display.println(totalEncoderPos);
  /*display.println("abcdef");
  display.println("------");*/
  display.println(myIP);
  display.display();
  //delay(100);
  display.clearDisplay();
  //analogWrite(D7, pwm);
  //r.loop();
  if (requestFlag == 1 && Cycles_done >= requestedCycles) {
    analogWrite(D7, 0);
    requestFlag = 0;
    Cycles_done = 0;
  }
  delay(10);
  yield();
}