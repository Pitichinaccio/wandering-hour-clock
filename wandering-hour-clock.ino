
/*
  4096  Steps/revolution
  1092  steps/second  (motor is guaranteed 500 steps/second)
  According to a datasheet, "Frequency" is 100Hz, which results in 1 rev in 40.96 sec.
  Max pull-in frequency = 500 Hz (8.1 sec/rev, 7.32 rpm),
  Max pull-out frequency = 900 hz (4.55 sec/rev, 13.2 rpm).
*/

/* It seems that the max RPM these motors can do is ~ 13 RPM, = ~ 1100
   mSec per step. It also depends on the amount of current your power
   supply can provide.
*/

/*  todo 
  c - Alternativen für SSID und Passwort
  - alternatives Soundschema ?
  - bestimmter Sound zur Frühstückszeit? konfigurierbar ?
  c - automatische Umstellung auf Sommer/Winterzeit?  - experimental, ABSCHALTBAR
  - Weckerfunktion?
  c - erst Bewegung, dann sound
  c - easteregg:  special sound zum Geburtstag
  - Kommentare in Englisch, code aufräumen. html wieder auf Englisch


*/
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>  // https://github.com/PaulStoffregen/Time
#include <WebServer.h>
#include <DFRobotDFPlayerMini.h>   // for audio aoutput via DFPlayerMini-module
#include <Preferences.h>

#include <StepperWidle.h>

#include "secrets.h"

// Replace the ssid and password in secrets.h
const char* ssid = SECRET_SSID;
const char* password = SECRET_PASSWORD;

// Hostname
const char* hostname = "wandering-hour-clock";

// Preferences library namespace and keys. The library
// limits the namespace and attrib length to 16 characters max.
const char* pref_namespace = "whc"; // "Wandering Hour Clock"
const char* attrib_tzhours = "tzhours";
const char* attrib_tzmins = "tzmins";
const char* attrib_isdst = "isdst";
const char* attrib_volume = "volume";
const char* attrib_sonoff = "sonoff";
const char* attrib_msteps = "msteps";

const int stepsPerRev = 2048; /* steps / rev for stepper motor */
const int maxSpeed = 4;   /* max speed stepper RPM, conservative    -    original value was 8*/
const int steps_correction = 0;  // steps correction value, added every minute
const int led = 13;   /* built-in led */

#define IN1 19
#define IN2 18
#define IN3 5
#define IN4 23  // 17 now used for DFPlayer

#define RXD2 16 // serial port for DFPLayer
#define TXD2 17


int stepDelay;      /* minimum delay / step in uSec */
int nstrikes;  // number of strikes every full hour, for sound output
int msteps;  // manual steps for debugging
bool setidle = true;  // sets stepper idle after each movement if true
int delay_until_idle = 500;   // delay in milliseconds whereafter the stepper is set to idle
unsigned int updateIntervalMinutes = 1;
unsigned long pMinute = 0;
unsigned long cMinute;
unsigned long cHour;
unsigned long pHour;


bool qsound = false;  // true, if sound for every quarter of an hour has already been played
bool hsound = false;  // ... same for full hours soundoutput

bool autoDST = true;  // automatic setting for DST switched on/off


unsigned long easteregg_month = 2;   // "easter egg"  -  plays a certain sound at a specific date, e.g. a birthday song
unsigned long easteregg_day = 1;



boolean summertime_EU(int year, byte month, byte day, byte hour, byte tzHours)
// European Daylight Savings Time calculation by "jurs" for German Arduino Forum
// input parameters: "normal time" for year, month, day, hour and tzHours (0=UTC, 1=MEZ)
// return value: returns true during Daylight Saving Time, false otherwise
{ 
  if (month<3 || month>10) return false; // keine Sommerzeit in Jan, Feb, Nov, Dez
  if (month>3 && month<10) return true; // Sommerzeit in Apr, Mai, Jun, Jul, Aug, Sep
  if (month==3 && (hour + 24 * day)>=(1 + tzHours + 24*(31 - (5 * year /4 + 4) % 7)) || month==10 && (hour + 24 * day)<(1 + tzHours + 24*(31 - (5 * year /4 + 1) % 7))) 
    return true; 
  else 
    return false;
}


// initialize web server library
WebServer server(80); // Create a WebServer object that listens on port 80

// initialize the stepper library
StepperWidle myStepper(stepsPerRev, IN1, IN3, IN2, IN4);

// initialize library for DFPlayer
DFRobotDFPlayerMini myDFPlayer;

Preferences preferences;

// initialize UDP library
WiFiUDP udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
int retryCount = 0;             // Wifi connection retry count
const int maxRetryCount = 10;  // Maximum number of retry attempts

// Define the NTP server and timezone offset
//static const char ntpServerName[] = "us.pool.ntp.org";
//static const char ntpServerName[] = "time.nist.gov";
static const char ntpServerName[] = "ptbtime1.ptb.de";   // PTB Braunschweig, Germany

long timeZoneOffsetHours = 1;
long timeZoneOffsetMins = 0;
bool isDst = false;

bool soundOn = true;  // no sound at all if set to false;
long volume = 25; // volume for DFPlayer  0 .. 30

time_t getNtpMinute();
void sendNTPpacket(IPAddress &address);
void handleDialAdjustments(int, int);




void setupWiFi() {
  // For arduino-esp32 V2.0.14, calling setHostname(...) followed by
  // config(...) and prior to both mode() and begin() will correctly
  // set the hostname.

  // The above ordering shouldn't really be required; in an ideal
  // world, calling setHostname() any time before begin() should be ok.
  // I am hopeful this will be true in the future.  But in any case,
  // this is what works for me now.

  // Note that calling getHostname() isn't a reliable way to verify
  // the hostname, because getHostname() reads the current internal
  // variable, which may NOT have been the name sent in the actual
  // DHCP request. Thus the result from getHostname() may be out of
  // sync with the DHCP server.

  // For a little more info, please see:
  // https://github.com/tzapu/WiFiManager/issues/1403

  WiFi.setHostname(hostname);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
    retryCount++;
    if (retryCount >= maxRetryCount/2) {  // try to login using the alternative SSID and password
      Serial.println("Failed to connect to WiFi. Using alternative SSID and password ....");
      ssid = SECRET_SSID2;
      password = SECRET_PASSWORD2;
    }
    if (retryCount >= maxRetryCount) {
        Serial.println("Failed to connect to WiFi. Restarting...");
        ESP.restart();  // If maximum retry count is reached, restart the board
    }
  }
  retryCount = 0;  // Reset retry count on successful connection
  Serial.println("Connected to WiFi");
  Serial.println("IP address: " + WiFi.localIP().toString());
}

void setupTz() {
  preferences.begin(pref_namespace, true); // Readonly mode
  // Default to UTC
  timeZoneOffsetHours = preferences.getLong(attrib_tzhours, 0);
  timeZoneOffsetMins = preferences.getLong(attrib_tzmins, 0);
  isDst = preferences.getBool(attrib_isdst, false);
  preferences.end();
}

void setupSound()  {
  preferences.begin(pref_namespace, true); // Readonly mode
  volume = preferences.getLong(attrib_volume, 10);
  soundOn = preferences.getBool(attrib_sonoff, true);
  myDFPlayer.volume(volume);  // setting volume
  Serial.print("DFPlayer volume set to ");
  Serial.println(volume);
  preferences.end();
}

void setupmsteps() {     // manual steps, for fine tuning the position or debugging
  preferences.begin(pref_namespace, true); // Readonly mode
  msteps = preferences.getLong(attrib_msteps, 0);
  Serial.print("manual steps movement: ");
  Serial.println(msteps);
  myStepper.step(msteps);
  setIdle();
  preferences.end();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");
  
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);  // setup DFPlayer
  myDFPlayer.begin(Serial2);
  if(Serial2.available() >0) {
     Serial.println("Serial is available."); 
     Serial.println("DFPlayer connected");
  } else {
      Serial.println("Serial is not available.");
      Serial.println("DFPlayer not connected.");
  }

  // test DFPlayer
  if (soundOn == true) {
  myDFPlayer.volume(volume);  //0..30
  myDFPlayer.playFolder(15, 4);  //play specific mp3 in SD:/15/004.mp3; Folder Name(1~99); File Name(1~255)
  //while(myDFPlayer.readType() != DFPlayerPlayFinished) {
  //delay(4500); 
  //myDFPlayer.playFolder(15, 5);
  }


  // Setup Wi-Fi connection
  setupWiFi();

  // Setup time zone variables
  setupTz();

  // Setup sound preference variables
  setupSound();

  // Initialize the NTP client and sync with the NTP server
  udp.begin(localPort);
  Serial.print("NTP UDP Local port: ");
  Serial.println(localPort);

  Serial.println("waiting for sync");
  setSyncProvider(getNtpMinute);
  setSyncInterval(300);
  while (timeStatus() == timeNotSet) {
    delay(5000);
  }

  // Set up Arduino OTA

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();

  // Set up the web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/submit", HTTP_POST, handleFormSubmit);
  server.on("/forward-5", HTTP_POST, handleFormForward5);
  server.on("/backward-5", HTTP_POST, handleFormBackward5);
  server.on("/recycle", HTTP_POST, handleFormRecycle);
  server.on("/demo", HTTP_POST, handleFormDemo);
  server.on("/set-preferences", HTTP_POST, handleFormSetPreferences);

  server.begin(); // Start the server

  pinMode(led, OUTPUT);

  myStepper.setSpeed(maxSpeed);

  time_t currentTime = now();
  // Convert the Unix time to local time
  tm localTime = *localtime(&currentTime);

  cMinute = pMinute = localTime.tm_min;
  cHour = pHour = localTime.tm_hour % 12;

  // Start up cycle
  handleFormRecycle();
}


void setIdle () {
 if (setidle) {
  delay(delay_until_idle); 
  myStepper.idle();
 }
}



void loop() {
  //  Handle Arduino OTA upload requests
  ArduinoOTA.handle();

  // Handle incoming client requests
  server.handleClient();

  // Check Wi-Fi connection and reconnect if necessary
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    setupWiFi();
  }

  // Get the current time in seconds since January 1, 1970 (Unix time)
  time_t currentTime = now();

  // Convert the Unix time to local time
  tm localTime = *localtime(&currentTime);

  long cStep = 0;     /* current motor step count */

  /* We go 1 rev = 2048 steps / hour */
  /* Every updateIntervalMinutes minutes, do a little move */
  cMinute = localTime.tm_min;;



  if (cMinute != pMinute && cMinute >= (pMinute + updateIntervalMinutes) % 60) { /* time for update? - every updateIntervalMinutes minutes */
    /*
    Calculation for minute difference
    * linear advance
      pMinute = 1
      cMinute = 5
      diffMinute = (5 - 1 + 60) % 60 = 4

    * hour wrap
      pMinute = 59
      cMinute = 4
      diffMinute = (4 - 59 + 60) % 60 = 5
    */
    int diffMinute = (cMinute - pMinute + 60) % 60; /* minutes since last step */
    pMinute = cMinute;
    cStep = (stepsPerRev * diffMinute) / 60;  /* desired motor position - 170 steps every 5 minutes */

    cStep = cStep + steps_correction;   // correction value !
    
    if (cMinute % 7 == 0  && cMinute != 0) {
      Serial.println("at minutes 7, 14, 21, 28, 35, 42, 49, 56 :  1 additional step ! ");
      cStep = cStep + 1;
    }


    // Debug prints
    Serial.print(diffMinute);
    Serial.print(" minute(s), ");
    Serial.print(cStep);
    Serial.print(" steps");
    Serial.println();

    myStepper.setSpeed(maxSpeed/2);
    myStepper.step(cStep);
    setIdle();
    myStepper.setSpeed(maxSpeed);

    // sound output routines

    if (soundOn == true) {
      // sound output - strikes every full hour 

      if (cMinute == 0 && hsound ==false) {    
        if (day() == easteregg_day && month() == easteregg_month) { 
          myDFPlayer.playFolder(15, 10); 
          hsound = true;  
        }                             
        nstrikes = localTime.tm_hour % 12;
        if (nstrikes == 0) nstrikes = 12;
        if (hsound == false) for (int y=1; y <= nstrikes; y++) {myDFPlayer.playFolder(1, 1); delay(2000); } 
        hsound = true;
      }  
      if (cMinute > 0) hsound = false;
      
      // ... and sound output every 15 minutes
      if (cMinute == 15 && qsound == false)      { myDFPlayer.playFolder(4, 15); qsound = true; }
      if (cMinute == 30 && qsound == false)      { myDFPlayer.playFolder(4, 30); qsound = true; }
      if (cMinute == 45 && qsound == false)      { myDFPlayer.playFolder(4, 45); qsound = true; }
      if (cMinute % 15  == 1) qsound = false;  // eine Minute später wieder auf false setzen
      }

    return;
  }

  // Handle hour offsets    ..... this is the original code, this is done now by 1 additional step every 7 minutes
  /*
  cHour = localTime.tm_hour % 12;
  if (cHour != pHour && cHour >= ((pHour + 1) % 12)) {
    int stepsPerMinute = stepsPerRev / 60; // 1 rev = 60 minutes = 2048 steps => 34 (int) steps per minute instead of 34.133333
    int stepsPerHour = 60 * stepsPerMinute; // 34 * 60 = 2040 steps in 1 hour. => we are missing 8 steps every hour

    int missingSteps = stepsPerRev - stepsPerHour;  // 2048 - 2040 = 8
    int diffHour = (cHour - pHour + 12) % 12;
    Serial.print("Hour complete: ");
    Serial.print(missingSteps*diffHour);
    Serial.print("recovering missed steps: ");
    Serial.println(missingSteps);
    if (missingSteps > 0) {
      myStepper.setSpeed(maxSpeed/2);
      myStepper.step(missingSteps);
      myStepper.idle();
      myStepper.setSpeed(maxSpeed);
    }
    pHour = cHour;
  }
  */

 
}

void handleRoot() {
  String html = "<html><head>";
  html += "<title>Wandering Hour Clock</title>";
  html += "  <meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "  <style>";
  html += "    body {";
  html += "      font-family: Arial, Helvetica, sans-serif;";
  html += "    }";
  html += "";
  html += "    .container {";
  html += "      width: 100%;";
  html += "      max-width: 400px;";
  html += "      margin: 0 auto;";
  html += "      padding: 20px;";
  html += "    }";
  html += "";
  html += "    label {";
  html += "      display: block;";
  html += "      margin-bottom: 10px;";
  html += "    }";
  html += "";
  html += "    input[type='number'] {";
  html += "      width: 100%;";
  html += "      padding: 10px;";
  html += "      margin-bottom: 20px;";
  html += "      border: 1px solid #ccc;";
  html += "      border-radius: 4px;";
  html += "      box-sizing: border-box;";
  html += "    }";
  html += "";
  html += "    button {";
  html += "      background-color: #4CAF50;";
  html += "      color: white;";
  html += "      padding: 10px 20px;";
  html += "      border: none;";
  html += "      border-radius: 4px;";
  html += "      cursor: pointer;";
  html += "      width: 100%;";
  html += "    }";
  html += "";
  html += "    button:hover {";
  html += "      background-color: #45a049;";
  html += "    }";
  html += "  </style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h2>WANDERING HOUR CLOCK</h2>";
  html += "<h2>Zeit einstellen</h2>";

  html += "<h3>Stelle die Zeit ein, die die Uhr jetzt anzeigt. Klicke dann auf 'Set Time'</h3>";
  html += "<form method='POST' action='/submit'>";
  html += "<label for='hour'>Hour (1-12):</label><input type='number' id='hour' name='hour' min='1' max='12' required><br>";
  html += "<label for='minute61813414'>Minute (0-59):</label><input type='number' id='minute' name='minute' min='0' max='59' required><br>";
  html += "<button type='submit'>Set Time</button></form>";
  html += "<form method='POST' action='/forward-5'><button type='submit'>+5m</button></form></body></html>";
  html += "<form method='POST' action='/backward-5'><button type='submit'>-5m</button></form></body></html>";
  html += "<form method='POST' action='/recycle'><button type='submit'>Recycle</button></form></body></html>";
  html += "<form method='POST' action='/demo'><button type='submit'>Demo</button></form></body></html>";

  html += "<h2>Einstellungen</h2>";
  html += "<form method='POST' action='/set-preferences'>";
  html += "<label for='TZ hour offset'>Zeitzone - Korrektur Stunden:</label><input type='number' id='hour_offset' name='hour_offset' min='-14' max='12' value='" + String(timeZoneOffsetHours) + "' required><br>";
  html += "<label for='TZ minute offset'>Zeitzone - Korrektur Minuten:</label><input type='number' id='minute_offset' name='minute_offset' min='0' max='59' value='" + String(timeZoneOffsetMins) + "' required><br>";
  html += "<label for='DST'>Daylight Savings Time currently in effect?</label><input type='checkbox' id='dst' name='dst'" + String( isDst ? "checked" : "") + "><br><br>";
  html += "<label for='soundonoff'>Sound ein</label><input type='checkbox' id='sonoff' name='sonoff'" + String( soundOn ? "checked" : "") + "><br><br>";
  html += "<label for='Volume'>Lautstaerke:</label><input type='number' id='lautstaerke' name='lautstaerke' min='1' max='30' value='" + String(volume) + "' required><br>";
  html += "<label for='manualSteps'>manuelle Schritte:</label><input type='number' id='manuelleschritte' name='manuelleschritte' min='-4000' max='4000' value='" + String(msteps) + "' required><br>";
  html += "<button type='submit'>Save preferences</button></form>";

  html += "<h2>Debug Info</h2>";
  html += "<div>cHour: cMinute = " + String(cHour) + ":" + (cMinute < 10 ? "0" : "") + String(cMinute) + "</div>";
  html += "<br/><div>pHour: pMinute = " + String(pHour) + ":" + (pMinute < 10 ? "0" : "") + String(pMinute) + "</div>";
  html += "<br/><div>timeZoneOffsetHours : timeZoneOffsetMins = " + String(timeZoneOffsetHours) + ":" + (timeZoneOffsetMins < 10 ? "0" : "") + String(timeZoneOffsetMins) + "</div>";
  html += "<br/><div>isDst = " + String(isDst ? "true" : "false") + "</div>";
  html += "<br/><div>soundOn = " + String(soundOn ? "true" : "false") + "</div>";
  html += "<br/><div>Laustaerke = " + String(volume);

  html += "<br/><div>hostname = " + String(hostname) + "</div>";

  server.send(200, "text/html", html);
}

void handleFormForward5() {
  Serial.println("full rotation counterclockwise");
  myStepper.step(-stepsPerRev);
  setIdle();

  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev);
  setIdle();

  Serial.println("Jump 5m");
  myStepper.step((stepsPerRev * 5) / 60);
  setIdle();

  server.send(200, "text/plain", "Moved 5 minutes Forward");
}

void handleFormBackward5() {
  Serial.println("full rotation counterclockwise");
  myStepper.step(-stepsPerRev);
  setIdle();

  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev);
  setIdle();

  Serial.println("Jump 5m"); 
  myStepper.step(-1 * (stepsPerRev * 5) / 60);
  setIdle();


  server.send(200, "text/plain", "Moved 5 minutes Backward");
}

void handleFormRecycle() {
  Serial.println("full rotation counterclockwise");
  myStepper.step(-stepsPerRev);
  setIdle();

  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev);
  setIdle();

  server.send(200, "text/plain", "Cycle complete");
}


void handleFormDemo() {
  Serial.println("full rotation clockwise");
  myStepper.step(stepsPerRev*12);
  setIdle();

  server.send(200, "text/plain", "Demo 12h Cycle complete");
}

void handleDialAdjustments(int iHour, int iMinute) {

  time_t currentTime = now();
  // Convert the Unix time to local time
  tm localTime = *localtime(&currentTime);

  cMinute = pMinute = localTime.tm_min;
  cHour = pHour = localTime.tm_hour % 12;

  // Print the local time to the serial monitor
  Serial.print("Current time (PST): ");
  Serial.print(cHour);
  Serial.print(":");
  Serial.println(localTime.tm_min);

  // Parse the input time in hours and minutes
  Serial.print("Input time (PST): ");
  Serial.print(iHour);
  Serial.print(":");
  Serial.println(iMinute);


  // Calculate the time difference in minutes
  int hourMinDiff = (cHour - iHour) * 60;
  Serial.print("Time difference in minutes from hour: ");
  Serial.println(hourMinDiff);
  int minuteDiff = cMinute - iMinute;
  Serial.print("Time difference from minutes: ");
  Serial.println(minuteDiff);

  int minuteDifference = hourMinDiff + minuteDiff;
  // calculate shortest way to correct position
  if (minuteDifference < -360) minuteDifference = 720 + minuteDifference;
  if (minuteDifference > 360) minuteDifference = 720 - minuteDifference;



  // Print the time difference in seconds
  Serial.print("Time difference: ");
  Serial.print(minuteDifference);
  Serial.println(" minutes");

  // Handle adjustments
  int steps = minuteDifference * stepsPerRev / 60; // 60 minutes = stepsPerRev => timeDiff * stepsPerRev / 60 offset steps required
  Serial.print(steps);
  Serial.println(" adjusting steps");
  myStepper.setSpeed(maxSpeed/2);
  myStepper.step(steps);
  setIdle();
  myStepper.setSpeed(maxSpeed);

}

void handleFormSetPreferences() {
  if (server.hasArg("hour_offset") && server.hasArg("minute_offset")) {
    int tmp_hour_offset = server.arg("hour_offset").toInt();
    int tmp_minute_offset = server.arg("minute_offset").toInt();
    bool tmp_dst = server.hasArg("dst");
    bool tmp_sonoff = server.hasArg("sonoff");
    int tmp_volume = server.arg("lautstaerke").toInt();
    int tmp_msteps = server.arg("manuelleschritte").toInt();

    if (tmp_hour_offset >= -14 && tmp_hour_offset <= 12 && tmp_minute_offset >= 0 && tmp_minute_offset <= 59) {
      Serial.print("Setting hour offset: ");
      Serial.println(tmp_hour_offset);
      Serial.print("Setting minute offset: ");
      Serial.println(tmp_minute_offset);
      Serial.print("Setting DST: ");
      Serial.println(tmp_dst ? "true" : "false");

      preferences.begin(pref_namespace, false); // read/write mode
      preferences.putLong(attrib_tzhours, tmp_hour_offset);
      preferences.putLong(attrib_tzmins, tmp_minute_offset);
      preferences.putBool(attrib_isdst, tmp_dst);
      preferences.putBool(attrib_sonoff, tmp_sonoff);
      preferences.putLong(attrib_volume, tmp_volume);
      preferences.putLong(attrib_msteps, tmp_msteps);
      preferences.end();

      // reread and sync the global variables with the preferences values
      setupTz();

      // do manual steps
      setupmsteps();

      // also with sound prefs
      setupSound();

      // force a time sync now
      setTime(getNtpMinute());

      // Return a success message to the client
      server.send(200, "text/plain", "Preferences set successfully");

    } else {
      server.send(400, "text/plain", "Invalid values");
    }
  } else {
    server.send(400, "text/plain", "Missing fields");
  }
}

void handleFormSubmit() {
  // Check if the form was submitted
  if (server.hasArg("hour") && server.hasArg("minute")) {
    // Parse the hour and minute values from the form
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();

    // Validate the values
    if (hour >= 1 && hour <= 12 && minute >= 0 && minute <= 59) {
      // Print the values to the Serial monitor
      Serial.print("Hour: ");
      Serial.println(hour);
      Serial.print("Minute: ");
      Serial.println(minute);

      handleDialAdjustments(hour, minute);

      // Return a success message to the client
      server.send(200, "text/plain", "Time set successfully");
    } else {
      // Return an error message to the client
      server.send(400, "text/plain", "Invalid values");
    }
  } else {
    // Return an error message to the client
    server.send(400, "text/plain", "Missing fields");
  }
}

/*-------- NTP code ----------*/
// Ref: https://github.com/PaulStoffregen/Time/blob/master/examples/TimeNTP_ESP8266WiFi/TimeNTP_ESP8266WiFi.ino

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpMinute()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];

      // Adjust from UTC to local time zone
      unsigned long secs = secsSince1900 - 2208988800UL;
      secs += timeZoneOffsetHours * SECS_PER_HOUR;
      secs += (timeZoneOffsetHours < 0 ? -timeZoneOffsetMins : timeZoneOffsetMins) * SECS_PER_MIN;
      secs += (isDst ? SECS_PER_HOUR : 0);

      Serial.print("time received from NTP is "); Serial.print(hour()); Serial.print(":"); Serial.print(minute()); Serial.print("  ");
      Serial.print(day()); Serial.print("."); Serial.print(month()); Serial.print("."); Serial.println(year());
      if (autoDST) isDst =  summertime_EU(year(), month(), day(), hour(), 1);    // calculate if it is DST or not
      if (isDst == true) { Serial.println("actual is daylight saving time"); } else { Serial.println("actual is NO daylight saving time"); }

      return secs;

    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}
