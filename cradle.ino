#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ====== Wi-Fi Credentials ======
const char* ssid = "network";
const char* password = "12345678";

// ====== Firebase Configuration ======
const char* firebaseHost = "https://babycradle-46b7e-default-rtdb.firebaseio.com/";
const char* firebaseAuth = "yUhViMVYwF7xyObOOffDrbVWIdmLPATogkw26qV66"; // Optional for public database
String deviceId = "motor_controller_001"; // Unique device identifier

// ====== Hardware Pin Definitions ======
const int relayPin = 18;      // Relay (active HIGH)
const int trigPin = 5;        // Ultrasonic TRIG
const int echoPin = 4;        // Ultrasonic ECHO
const int buzzerPin = 2;      // Buzzer
const int buttonPin = 19;     // Manual push button (GPIO 19)

// ====== Threshold ======
const int stopDistance = 30; // cm - 30cm detection distance

// ====== System State ======
bool systemEnabled = false;
float currentDistance = 0;
String systemStatus = "System OFF";
bool manualButtonPressed = false;

// ====== Button State Management ======
bool lastButtonState = HIGH;
bool buttonPressed = false;
unsigned long lastButtonPress = 0;
const unsigned long buttonDebounceDelay = 50; // 50ms debounce

// ====== Distance Filtering ======
float distanceReadings[5] = {0, 0, 0, 0, 0}; // Store last 5 readings
int readingIndex = 0;
bool validDistance = false;

// ====== Firebase & Timing ======
unsigned long lastFirebaseUpdate = 0;
unsigned long lastFirebaseCommand = 0;
const unsigned long firebaseUpdateInterval = 2000; // Update every 2 seconds
const unsigned long firebaseCommandInterval = 1000; // Check commands every 1 second

// ====== Web Server Setup ======
WebServer server(80);

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Pin setup
  pinMode(relayPin, OUTPUT);     
  digitalWrite(relayPin, LOW);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(buzzerPin, OUTPUT);    
  digitalWrite(buzzerPin, LOW);
  pinMode(buttonPin, INPUT_PULLUP);

  // Initialize distance readings array
  for (int i = 0; i < 5; i++) {
    distanceReadings[i] = 999; // Initialize with large values
  }

  Serial.println("=== ESP32 Motor Control System with Firebase ===");
  Serial.println("Button Test - Press button now for 3 seconds:");
  
  // Button test
  unsigned long testStart = millis();
  while (millis() - testStart < 3000) {
    if (digitalRead(buttonPin) == LOW) {
      Serial.println("Button press detected!");
      delay(200);
    }
    delay(50);
  }

  // Wi-Fi connection
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); 
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Web server routes
  server.on("/", handleRoot);
  server.on("/on", handleSystemOn);
  server.on("/off", handleSystemOff);
  server.on("/status", handleStatus);

  server.begin();
  Serial.println("Web server started");
  Serial.println("System ready - Manual button: GPIO 19");
  
  // Initialize Firebase
  initializeFirebase();
}

void loop() {
  server.handleClient();
  
  // Handle button first
  handleButton();
  
  // Get accurate distance reading
  currentDistance = getAccurateDistance();
  
  // Check Firebase commands
  if (millis() - lastFirebaseCommand > firebaseCommandInterval) {
    checkFirebaseCommands();
    lastFirebaseCommand = millis();
  }
  
  // Update Firebase status
  if (millis() - lastFirebaseUpdate > firebaseUpdateInterval) {
    updateFirebaseStatus();
    lastFirebaseUpdate = millis();
  }
  
  // System control logic
  if (!systemEnabled) {
    digitalWrite(relayPin, LOW);
    digitalWrite(buzzerPin, LOW);
    systemStatus = "System OFF";
    delay(20);
    return;
  }

  // System is ON - check for obstacles
  if (validDistance && currentDistance <= stopDistance) {
    // Object detected within 30cm
    digitalWrite(relayPin, LOW);  // Stop motor
    systemStatus = "OBSTACLE DETECTED at " + String(currentDistance, 1) + "cm - Motor Stopped";
    
    // Warning beep every 1 second
    static unsigned long lastWarningBeep = 0;
    if (millis() - lastWarningBeep > 1000) {
      digitalWrite(buzzerPin, HIGH);
      delay(150);
      digitalWrite(buzzerPin, LOW);
      lastWarningBeep = millis();
    }
  } else {
    // Path is clear
    digitalWrite(relayPin, HIGH);  // Motor ON
    digitalWrite(buzzerPin, LOW);
    if (validDistance) {
      systemStatus = "Motor Running - Clear Path (" + String(currentDistance, 1) + "cm)";
    } else {
      systemStatus = "Motor Running - Sensor Reading...";
    }
  }

  delay(20); // Small delay for stability
}

void initializeFirebase() {
  Serial.println("Initializing Firebase connection...");
  
  // Register device in Firebase
  HTTPClient http;
  http.begin(String(firebaseHost) + "devices/" + deviceId + ".json");
  http.addHeader("Content-Type", "application/json");
  
  String deviceInfo = "{";
  deviceInfo += "\"deviceId\":\"" + deviceId + "\",";
  deviceInfo += "\"name\":\"Motor Controller\",";
  deviceInfo += "\"type\":\"ESP32\",";
  deviceInfo += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  deviceInfo += "\"status\":\"online\",";
  deviceInfo += "\"lastSeen\":\"" + String(millis()) + "\"";
  deviceInfo += "}";
  
  int httpResponseCode = http.PUT(deviceInfo);
  if (httpResponseCode > 0) {
    Serial.println("Device registered in Firebase");
  } else {
    Serial.println("Failed to register device in Firebase");
  }
  http.end();
}

void checkFirebaseCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(String(firebaseHost) + "commands/" + deviceId + "/command.json");
  
  int httpResponseCode = http.GET();
  if (httpResponseCode == 200) {
    String response = http.getString();
    
    if (response != "null" && response.length() > 2) {
      StaticJsonDocument<200> doc;
      deserializeJson(doc, response);
      
      String command = doc["action"];
      String timestamp = doc["timestamp"];
      
      // Check if command is new (to avoid executing same command multiple times)
      static String lastCommandTimestamp = "";
      if (timestamp != lastCommandTimestamp) {
        lastCommandTimestamp = timestamp;
        
        if (command == "turn_on") {
          systemEnabled = true;
          Serial.println("System ON - Firebase Command");
        } else if (command == "turn_off") {
          systemEnabled = false;
          digitalWrite(relayPin, LOW);
          digitalWrite(buzzerPin, LOW);
          Serial.println("System OFF - Firebase Command");
        }
        
        // Clear the command after execution
        clearFirebaseCommand();
      }
    }
  }
  http.end();
}

void clearFirebaseCommand() {
  HTTPClient http;
  http.begin(String(firebaseHost) + "commands/" + deviceId + "/command.json");
  int httpResponseCode = http.DELETE();
  http.end();
}

void updateFirebaseStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(String(firebaseHost) + "devices/" + deviceId + "/status.json");
  http.addHeader("Content-Type", "application/json");
  
  String statusJson = "{";
  statusJson += "\"systemEnabled\":" + String(systemEnabled ? "true" : "false") + ",";
  statusJson += "\"distance\":" + String(currentDistance, 1) + ",";
  statusJson += "\"systemStatus\":\"" + systemStatus + "\",";
  statusJson += "\"buttonPressed\":" + String(manualButtonPressed ? "true" : "false") + ",";
  statusJson += "\"validDistance\":" + String(validDistance ? "true" : "false") + ",";
  statusJson += "\"lastUpdate\":\"" + String(millis()) + "\",";
  statusJson += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  statusJson += "}";
  
  int httpResponseCode = http.PUT(statusJson);
  if (httpResponseCode > 0) {
    // Serial.println("Status updated to Firebase");
  }
  http.end();
}

void handleButton() {
  int currentButtonState = digitalRead(buttonPin);
  
  // Detect button press (HIGH to LOW transition)
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    unsigned long currentTime = millis();
    if (currentTime - lastButtonPress > buttonDebounceDelay) {
      // Valid button press - toggle system and button status
      systemEnabled = !systemEnabled;
      manualButtonPressed = !manualButtonPressed; // Toggle button display status
      lastButtonPress = currentTime;
      
      Serial.println("=== MANUAL BUTTON PRESSED ===");
      Serial.print("System: ");
      Serial.println(systemEnabled ? "ON" : "OFF");
      Serial.print("Button Status: ");
      Serial.println(manualButtonPressed ? "PRESSED" : "NOT PRESSED");
      
      if (!systemEnabled) {
        digitalWrite(relayPin, LOW);
        digitalWrite(buzzerPin, LOW);
        systemStatus = "System OFF - Manual Button";
      } else {
        systemStatus = "System ON - Manual Button";
      }
      
      // Confirmation beep
      digitalWrite(buzzerPin, HIGH);
      delay(100);
      digitalWrite(buzzerPin, LOW);
      delay(50);
      digitalWrite(buzzerPin, HIGH);
      delay(100);
      digitalWrite(buzzerPin, LOW);
      
      // Immediately update Firebase when manual button is pressed
      updateFirebaseStatus();
    }
  }
  
  lastButtonState = currentButtonState;
}

float getAccurateDistance() {
  // Take multiple readings and filter them
  float newReading = measureSingleDistance();
  
  // Store the new reading
  distanceReadings[readingIndex] = newReading;
  readingIndex = (readingIndex + 1) % 5;
  
  // Calculate median of the 5 readings (more accurate than average)
  float sortedReadings[5];
  for (int i = 0; i < 5; i++) {
    sortedReadings[i] = distanceReadings[i];
  }
  
  // Simple bubble sort for 5 elements
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4 - i; j++) {
      if (sortedReadings[j] > sortedReadings[j + 1]) {
        float temp = sortedReadings[j];
        sortedReadings[j] = sortedReadings[j + 1];
        sortedReadings[j + 1] = temp;
      }
    }
  }
  
  // Return median value
  float medianDistance = sortedReadings[2];
  
  // Check if reading is valid
  validDistance = (medianDistance > 0 && medianDistance < 400);
  
  return medianDistance;
}

float measureSingleDistance() {
  // Ensure clean trigger
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  // Measure echo time with timeout
  long duration = pulseIn(echoPin, HIGH, 25000); // 25ms timeout
  
  if (duration == 0) {
    return -1; // No echo
  }
  
  // Calculate distance: Speed of sound = 343 m/s = 0.0343 cm/μs
  float distance = (duration * 0.0343) / 2.0;
  
  // Filter unrealistic readings
  if (distance < 2 || distance > 400) {
    return -1;
  }
  
  return distance;
}

void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<title>ESP32 Motor Control Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<link href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css' rel='stylesheet'>";
  // Include Firebase SDK
  html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/firebase/9.22.0/firebase-app-compat.js'></script>";
  html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/firebase/9.22.0/firebase-database-compat.js'></script>";
  html += "<style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; ";
  html += "background: linear-gradient(135deg, #667eea 0%, #764ba2 50%, #ff6b6b 100%); ";
  html += "min-height: 100vh; padding: 20px; color: white; }";
  html += ".dashboard { max-width: 900px; margin: 0 auto; }";
  html += ".header { text-align: center; margin-bottom: 40px; }";
  html += ".header h1 { font-size: 2.5em; margin-bottom: 10px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }";
  html += ".header .subtitle { font-size: 1.2em; opacity: 0.9; }";
  html += ".share-section { background: rgba(255,255,255,0.15); backdrop-filter: blur(20px); ";
  html += "border-radius: 20px; padding: 25px; margin-bottom: 30px; text-align: center; }";
  html += ".share-link { background: rgba(255,255,255,0.2); padding: 15px; border-radius: 10px; ";
  html += "margin: 15px 0; font-family: monospace; word-break: break-all; font-size: 0.9em; }";
  html += ".copy-btn { background: #4CAF50; color: white; border: none; padding: 10px 20px; ";
  html += "border-radius: 25px; cursor: pointer; margin: 10px; }";
  // ... (keep existing styles)
  html += ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 25px; margin-bottom: 30px; }";
  html += ".card { background: rgba(255,255,255,0.15); backdrop-filter: blur(20px); ";
  html += "border-radius: 20px; padding: 25px; box-shadow: 0 15px 35px rgba(0,0,0,0.1); ";
  html += "border: 1px solid rgba(255,255,255,0.2); transition: transform 0.3s ease; }";
  html += ".card:hover { transform: translateY(-5px); }";
  html += ".card-header { display: flex; align-items: center; margin-bottom: 20px; }";
  html += ".card-header i { font-size: 1.5em; margin-right: 10px; color: #4CAF50; }";
  html += ".card-header h3 { font-size: 1.3em; }";
  html += ".status-item { display: flex; justify-content: space-between; align-items: center; ";
  html += "padding: 12px 0; border-bottom: 1px solid rgba(255,255,255,0.1); }";
  html += ".status-item:last-child { border-bottom: none; }";
  html += ".status-label { font-weight: 500; opacity: 0.9; }";
  html += ".status-value { font-weight: bold; font-size: 1.1em; }";
  html += ".system-on { color: #4CAF50; }";
  html += ".system-off { color: #f44336; }";
  html += ".distance-display { text-align: center; padding: 20px; ";
  html += "background: rgba(255,255,255,0.1); border-radius: 15px; margin: 20px 0; }";
  html += ".distance-value { font-size: 3em; font-weight: bold; margin-bottom: 10px; }";
  html += ".distance-unit { font-size: 1.2em; opacity: 0.8; }";
  html += ".controls-card { text-align: center; }";
  html += ".control-buttons { display: flex; gap: 15px; justify-content: center; flex-wrap: wrap; }";
  html += ".btn { padding: 15px 30px; border: none; border-radius: 50px; font-size: 1.1em; ";
  html += "font-weight: bold; cursor: pointer; transition: all 0.3s ease; ";
  html += "text-decoration: none; display: inline-flex; align-items: center; gap: 8px; }";
  html += ".btn i { font-size: 1.2em; }";
  html += ".btn-on { background: linear-gradient(45deg, #4CAF50, #45a049); color: white; ";
  html += "box-shadow: 0 4px 15px rgba(76,175,80,0.4); }";
  html += ".btn-on:hover { background: linear-gradient(45deg, #45a049, #4CAF50); ";
  html += "transform: translateY(-2px); box-shadow: 0 8px 25px rgba(76,175,80,0.6); }";
  html += ".btn-off { background: linear-gradient(45deg, #f44336, #e53935); color: white; ";
  html += "box-shadow: 0 4px 15px rgba(244,67,54,0.4); }";
  html += ".btn-off:hover { background: linear-gradient(45deg, #e53935, #f44336); ";
  html += "transform: translateY(-2px); box-shadow: 0 8px 25px rgba(244,67,54,0.6); }";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='dashboard'>";
  html += "<div class='header'>";
  html += "<h1><i class='fas fa-cogs'></i> ESP32 Motor Control</h1>";
  html += "<p class='subtitle'>Advanced Motor Control & Obstacle Detection System</p>";
  html += "</div>";
  
  // Share Section
  html += "<div class='share-section'>";
  html += "<h3><i class='fas fa-share-alt'></i> Share Control Access</h3>";
  html += "<p>Share this link to allow others to control the motor:</p>";
  html += "<div class='share-link' id='shareLink'>Loading share link...</div>";
  html += "<button class='copy-btn' onclick='copyShareLink()'>Copy Link</button>";
  html += "<button class='copy-btn' onclick='generateQR()'>Generate QR Code</button>";
  html += "</div>";
  
  // ... (keep rest of existing HTML structure)
  html += "<div class='grid'>";
  // ... (existing cards)
  html += "</div>";
  
  html += "</div>"; // End dashboard
  
  html += "<script>";
  // Firebase Configuration
  html += "const firebaseConfig = {";
  html += "  apiKey: 'your-api-key',";
  html += "  authDomain: 'your-project-id.firebaseapp.com',";
  html += "  databaseURL: 'https://your-project-id-default-rtdb.firebaseio.com/',";
  html += "  projectId: 'your-project-id',";
  html += "  storageBucket: 'your-project-id.appspot.com',";
  html += "  messagingSenderId: '123456789',";
  html += "  appId: 'your-app-id'";
  html += "};";
  
  html += "firebase.initializeApp(firebaseConfig);";
  html += "const database = firebase.database();";
  
  // Generate share link
  html += "function generateShareLink() {";
  html += "  const deviceId = '" + deviceId + "';";
  html += "  const shareUrl = 'https://your-domain.com/control?device=' + deviceId;";
  html += "  document.getElementById('shareLink').textContent = shareUrl;";
  html += "}";
  
  html += "function copyShareLink() {";
  html += "  const shareLink = document.getElementById('shareLink').textContent;";
  html += "  navigator.clipboard.writeText(shareLink).then(() => {";
  html += "    alert('Link copied to clipboard!');";
  html += "  });";
  html += "}";
  
  html += "function generateQR() {";
  html += "  const shareLink = document.getElementById('shareLink').textContent;";
  html += "  const qrUrl = 'https://api.qrserver.com/v1/create-qr-code/?size=300x300&data=' + encodeURIComponent(shareLink);";
  html += "  window.open(qrUrl, '_blank');";
  html += "}";
  
  // Function to toggle system via Firebase
  html += "function toggleSystem(state) {";
  html += "  const command = {";
  html += "    action: state ? 'turn_on' : 'turn_off',";
  html += "    timestamp: Date.now().toString()";
  html += "  };";
  html += "  database.ref('commands/" + deviceId + "/command').set(command);";
  html += "}";
  
  // Initialize on page load
  html += "window.onload = function() {";
  html += "  generateShareLink();";
  html += "};";
  
  // ... (keep existing auto-refresh and other scripts)
  html += "</script>";
  html += "</body>";
  html += "</html>";
  
  server.send(200, "text/html", html);
}

void handleSystemOn() {
  systemEnabled = true;
  Serial.println("System ON - Web Interface");
  updateFirebaseStatus(); // Update Firebase immediately
  server.send(200, "text/plain", "System ON");
}

void handleSystemOff() {
  systemEnabled = false;
  digitalWrite(relayPin, LOW);
  digitalWrite(buzzerPin, LOW);
  Serial.println("System OFF - Web Interface");
  updateFirebaseStatus(); // Update Firebase immediately
  server.send(200, "text/plain", "System OFF");
}

void handleStatus() {
  String json = "{";
  json += "\"system\":\"" + String(systemEnabled ? "ON" : "OFF") + "\",";
  json += "\"distance\":" + String(currentDistance, 1) + ",";
  json += "\"status\":\"" + systemStatus + "\",";
  json += "\"buttonPressed\":" + String(manualButtonPressed ? "true" : "false") + ",";
  json += "\"validReading\":" + String(validDistance ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}