#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "WiFi.h"
#include <HTTPClient.h>
#include <DHT.h> 

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Server details
const char* serverUrl = "http://your-api-endpoint.com/data";

// DHT11 sensor configuration
#define DHTPIN 4      // DHT11 data pin (GPIO4)
#define DHTTYPE DHT11 // DHT sensor type
DHT dht(DHTPIN, DHTTYPE);

// Task handles
TaskHandle_t DataCollectionTask;
TaskHandle_t ConnectionManagerTask;
TaskHandle_t DataSenderTask;

// Structure to hold DHT11 readings
typedef struct {
  float temperature;
  float humidity;
  unsigned long timestamp;
} SensorReading;

// Queue for passing sensor data between tasks
QueueHandle_t dataQueue;

// Storage for readings when offline
#define MAX_STORED_READINGS 100
SensorReading storedReadings[MAX_STORED_READINGS];
int storedReadingCount = 0;
bool isConnected = false;

// Mutex for protecting the stored readings array
SemaphoreHandle_t storageMutex;

// Task to collect sensor data
void dataCollectionTask(void *parameter) {
  while(1) {
    // Create a reading structure
    SensorReading reading;
    
    // Read temperature and humidity from DHT11
    reading.temperature = dht.readTemperature();
    reading.humidity = dht.readHumidity();
    reading.timestamp = millis();
    
    // Check if reading is valid
    if (isnan(reading.temperature) || isnan(reading.humidity)) {
      Serial.println("Failed to read from DHT sensor!");
    } else {
      Serial.print("Temperature: ");
      Serial.print(reading.temperature);
      Serial.print("°C, Humidity: ");
      Serial.print(reading.humidity);
      Serial.println("%");
      
      // Try to send to queue first (for immediate processing if online)
      if (xQueueSend(dataQueue, &reading, 0) != pdTRUE) {
        // Queue full or not available, store locally
        if (xSemaphoreTake(storageMutex, portMAX_DELAY) == pdTRUE) {
          if (storedReadingCount < MAX_STORED_READINGS) {
            storedReadings[storedReadingCount++] = reading;
            Serial.println("Reading stored locally");
          } else {
            Serial.println("Local storage full!");
          }
          xSemaphoreGive(storageMutex);
        }
      }
    }
    
    // Collect data every 5 seconds
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

// Task to manage WiFi connection
void connectionManagerTask(void *parameter) {
  while(1) {
    if (WiFi.status() != WL_CONNECTED) {
      isConnected = false;
      Serial.println("WiFi disconnected, attempting to reconnect...");
      
      WiFi.begin(ssid, password);
      
      // Try for 10 seconds to connect
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        attempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi reconnected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        isConnected = true;
      }
    } else {
      isConnected = true;
    }
    
    // Check connection every 30 seconds
    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

// Task to send data to server
void dataSenderTask(void *parameter) {
  SensorReading receivedReading;
  HTTPClient http;
  
  while(1) {
    if (isConnected) {
      // First try to send any stored readings
      if (xSemaphoreTake(storageMutex, portMAX_DELAY) == pdTRUE) {
        if (storedReadingCount > 0) {
          Serial.println("Sending stored readings...");
          
          for (int i = 0; i < storedReadingCount; i++) {
            // Create JSON payload with both temperature and humidity
            String payload = "{\"temperature\":" + String(storedReadings[i].temperature) + 
                           ",\"humidity\":" + String(storedReadings[i].humidity) + 
                           ",\"timestamp\":" + String(storedReadings[i].timestamp) + "}";
            
            http.begin(serverUrl);
            http.addHeader("Content-Type", "application/json");
            
            int httpResponseCode = http.POST(payload);
            if (httpResponseCode > 0) {
              Serial.println("Stored reading sent successfully");
            } else {
              Serial.println("Error sending stored reading: " + http.errorToString(httpResponseCode));
              // Put unsent readings back in front of the queue
              for (int j = i; j < storedReadingCount; j++) {
                storedReadings[j-i] = storedReadings[j];
              }
              storedReadingCount -= i;
              break;  // Stop trying if server is not responding
            }
            http.end();
            
            // Short delay between sends to not overwhelm the server
            vTaskDelay(100 / portTICK_PERIOD_MS);
          }
          
          // All stored readings sent successfully
          if (storedReadingCount == 0) {
            Serial.println("All stored readings sent!");
          }
        }
        xSemaphoreGive(storageMutex);
      }
      
      // Then check for new readings in the queue
      if (xQueueReceive(dataQueue, &receivedReading, 0) == pdTRUE) {
        // Create JSON payload with both temperature and humidity
        String payload = "{\"temperature\":" + String(receivedReading.temperature) + 
                       ",\"humidity\":" + String(receivedReading.humidity) + 
                       ",\"timestamp\":" + String(receivedReading.timestamp) + "}";
        
        http.begin(serverUrl);
        http.addHeader("Content-Type", "application/json");
        
        int httpResponseCode = http.POST(payload);
        if (httpResponseCode > 0) {
          Serial.println("Data sent successfully");
        } else {
          Serial.println("Error sending data: " + http.errorToString(httpResponseCode));
          
          // Store the reading that failed to send
          if (xSemaphoreTake(storageMutex, portMAX_DELAY) == pdTRUE) {
            if (storedReadingCount < MAX_STORED_READINGS) {
              storedReadings[storedReadingCount++] = receivedReading;
              Serial.println("Reading stored after send failure");
            }
            xSemaphoreGive(storageMutex);
          }
        }
        http.end();
      }
    }
    
    // Check for new data to send every second
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Initialize DHT11 sensor
  dht.begin();
  Serial.println("DHT11 Initialized");
  
  // Initialize WiFi in station mode
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  Serial.println("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  isConnected = true;
  
  // Create the queue
  dataQueue = xQueueCreate(10, sizeof(SensorReading));
  
  // Create mutex
  storageMutex = xSemaphoreCreateMutex();
  
  // Create tasks
  xTaskCreate(
    dataCollectionTask,
    "DataCollection",
    4096,
    NULL,
    1,
    &DataCollectionTask
  );
  
  xTaskCreate(
    connectionManagerTask,
    "ConnectionManager",
    4096,
    NULL,
    2,  // Higher priority for connection management
    &ConnectionManagerTask
  );
  
  xTaskCreate(
    dataSenderTask,
    "DataSender",
    4096,
    NULL,
    1,
    &DataSenderTask
  );
  
  Serial.println("All tasks created successfully");
}

void loop() {
  // Empty loop - FreeRTOS scheduler handles the tasks
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
