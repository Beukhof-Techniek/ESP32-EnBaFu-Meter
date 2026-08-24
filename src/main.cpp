// ESP32 Engine, Battery & Fuel Meter for Signal K
//
// This application demonstrates core SensESP concepts in a very
// concise manner. You can build and upload the application as is
// and observe the value changes on the serial port monitor.
//
// You can use this source file as a basis for your own projects.
// Remove the parts that are not relevant to you, and add your own code
// for external hardware libraries.

#include "sensesp_app_builder.h"
#include "sensesp/sensors/analog_input.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/system/lambda_consumer.h"
#include "sensesp/transforms/linear.h"
#include "sensesp/transforms/moving_average.h"
#include <Preferences.h>
#include "includes/EnBaFu_SSD1306.h"
#include <Adafruit_INA219.h>

// *************************************************************************************
// ************************************ PARAMETERS *************************************
// *************************************************************************************

// Replace with your network credentials
const char *wifi_ssid = "your-wifi-devices-ssid";
const char *wifi_password = "your-wifi-devices-password";
const char *hostname = "esp32-enbafu-meter"; // ESP32 Engine, Battery & Fuel Meter (or whatever name you think is valid)

// Replace with your level meter measurements and tank volume
const float tank_voltage_full = 3.316;  // Measured input voltage at full tank (maximum level, use debug info)
const float tank_voltage_empty = 0.016; // Measured input voltage at empty tank (minimum level, use debug info)
const char *tank_type = "diesel";       // Type of tank (diesel, petrol, rum, ...)
const float tank_capacity = 0.060;      // Tank capacity in m3

// Replace with your alternator measurements
// Note: It can be the case that there is a certain voltage on the alternator while the Engine is stopped.
//       This happens for instance when the Alternator is connected to a Victron ArgoFET or similar.
const float alternator_voltage_min = 6.0;      // The minimum voltage from the alternator at which we can safely assume the engine is running (see note above)
unsigned long initial_engine_running_time = 0; // Set this to whatever value you like the runtime counter to start at
                                               // (only needed at the first use or when flash has been cleared). You
                                               // can safely keep it at 0.

// Signal K client config
// Important note: when changing the Signal K server address/connecting to a different Signal K server
// and therefore changing the sk_server_address value below, you need to CLEAR THE FLASH from the ESP
// as well because SensESP stores this value in its config path and prefers that value over whatever
// you feed the builder constructor below. After clearing flash, you might want to re-enter the last known
// engine runTime as the initial_engine_running_time value above to omit the counter starting from 0 again.
const char *sk_server_address = "192.168.20.1";
const uint16_t sk_server_port = 3000;

// *************************************************************************************
// ********************************** END PARAMETERS ***********************************
// *************************************************************************************

#define INA219_FUEL_I2C_ADDRESS (0x40) // Fuel level measurement device
#define INA219_ALTR_I2C_ADDRESS (0x41) // Alternator voltage measurement device
#define INA219_BAT0_I2C_ADDRESS (0x44) // Battery 0 voltage measurement device
#define INA219_BAT1_I2C_ADDRESS (0x45) // Battery 1 voltage measurement device

using namespace sensesp;

Preferences preferences;

// Instantiate the oled display
EnBaFu_SSD1306 oledDisplay(&Wire);

// Instantiate the Vin measurement chips
Adafruit_INA219 ina219_fuel(INA219_FUEL_I2C_ADDRESS); // Fuel level measurement device
Adafruit_INA219 ina219_altr(INA219_ALTR_I2C_ADDRESS); // Alternator voltage measurement device
Adafruit_INA219 ina219_bat0(INA219_BAT0_I2C_ADDRESS); // Battery 0 voltage measurement device
Adafruit_INA219 ina219_bat1(INA219_BAT1_I2C_ADDRESS); // Battery 1 voltage measurement device

String secToHMS(unsigned long seconds){
  if (seconds == 0) {
    return "parameter error";
  } else {
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    char secondsBuff[3];
    sprintf(secondsBuff, "%02d", seconds%60);
    char minutesBuff[3];
    sprintf(minutesBuff, "%02d", minutes%60);
    return (String)hours + ":" + minutesBuff + ":" + secondsBuff;
  }
}

unsigned long previous_time = 0;
unsigned long engine_running_time = 0;

/*
 * The setup function performs one-time application initialization.
 */
void setup() {
  SetupLogging();

  /*
   * Set up the OLED display, clears the display and draws the splash screen
   */
  oledDisplay.begin(SSD1306_SWITCHCAPVCC, OLEDDISPLAY_I2C_ADDRESS);
  oledDisplay.clearDisplay(); // Not really necessary because drawSplashScreen() also takes care of this (just in case the splash screen is omitted)
  oledDisplay.setTextSize(1);
  oledDisplay.setTextColor(WHITE);
  oledDisplay.drawSplashScreen();
  delay(2000);  // Give people time to read the splash screen ;)

  /*
   * Set up the INA219 hardware
   */
  ina219_fuel.begin(); // Fuel level measurement device
  ina219_altr.begin(); // Alternator voltage measurement device
  ina219_bat0.begin(); // Battery 0 voltage measurement device
  ina219_bat1.begin(); // Battery 1 voltage measurement device

  /*
   * Construct the global SensESPApp() object
   */
  SensESPAppBuilder builder;
  sensesp_app = builder.set_hostname(hostname)
                      ->set_wifi_client(wifi_ssid, wifi_password)
                      ->set_sk_server(sk_server_address, sk_server_port)
                      ->get_app();

  #ifndef ONLY_SETUP

  // Retrieve the last known engine_running_time value
  if (preferences.begin("propulsion", false)) {
    // When opening the preferences returns a false, the value of engine_running_time will remain 0
    // which will "block" the storage of the parameter further on. Also it will be kept at 0 and shown
    // as such (and the text "parameter error") in the displays so the user will know there is something
    // wrong with the device.
    engine_running_time = preferences.getULong("main_runTime", 1); // Set to default at 1 to get it off the "error" value of 0
  }

  /**
   * Create a series of new Repeat Sensors that obtain data (from the I2C sources) periodically.
   */
  const unsigned int sensor_read_interval = 5000; // Define how often (in milliseconds) new samples are acquired
  auto *tank_voltage = new RepeatSensor<float>(sensor_read_interval, []() { return ina219_fuel.getBusVoltage_V(); });
  auto* alternator_voltage = new RepeatSensor<float>(sensor_read_interval, []() { return ina219_altr.getBusVoltage_V(); });
  auto* battery0_voltage = new RepeatSensor<float>(sensor_read_interval, []() { return ina219_bat0.getBusVoltage_V(); });
  auto* battery1_voltage = new RepeatSensor<float>(sensor_read_interval, []() { return ina219_bat1.getBusVoltage_V(); });
  ESP_LOGD(__FILE__, "tank_voltage, alternator_voltage and batteryx_voltage Instantiated!");

  /**
   * Add observers that print out and display the current value of the obtained data and some calculations every time it changes.
   */
  tank_voltage->attach([tank_voltage]() {
    char levelBuffer[5];
    sprintf(levelBuffer, "%d%%", (int)(((tank_voltage->get() - tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)) * 100));
    oledDisplay.printAt(35, 42, levelBuffer);
    ESP_LOGI(__FILE__, "\n\nAvailable tank_level values:\ntank_voltage_empty = %f\ntank_voltage_full = %f\nvoltage measured = %f\ncalculated level = %f\n", tank_voltage_empty, tank_voltage_full, tank_voltage->get(), ((tank_voltage->get() - tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));
  });
  alternator_voltage->attach([alternator_voltage]() {
    char alternatorBuffer[7];
    sprintf(alternatorBuffer, "%.2fV", alternator_voltage->get());
    oledDisplay.printAt(35, 3, alternatorBuffer);
    oledDisplay.printAt(35, 55, secToHMS(engine_running_time));
    ESP_LOGI(__FILE__, "\n\nAvailable alternator_voltage value:\nvoltage measured = %f\n", alternator_voltage->get());
  });
  battery0_voltage->attach([battery0_voltage]() {
    char battery0Buffer[7];
    sprintf(battery0Buffer, "%.2fV", battery0_voltage->get());
    oledDisplay.printAt(35, 16, battery0Buffer);
    ESP_LOGI(__FILE__, "\n\nAvailable battery0_voltage value:\nvoltage measured = %f\n", battery0_voltage->get());
  });
  battery1_voltage->attach([battery1_voltage]() {
    char battery1Buffer[7];
    sprintf(battery1Buffer, "%.2fV", battery1_voltage->get());
    oledDisplay.printAt(35, 29, battery1Buffer);
    ESP_LOGI(__FILE__, "\n\nAvailable battery1_voltage value:\nvoltage measured = %f\n", battery1_voltage->get());
  });
  ESP_LOGD(__FILE__, "tank_level, alternator_voltage and batteryx_voltage Attached!");

  /**
   * Lambda transformer for registering and storing the Engine Running Time and returning the Engine State
   */
  auto measuredAlternatorVoltageToEngineStateTransformer = new LambdaTransform<float, String>([](float input) -> String {
    unsigned long current_time = millis();
    if (input < alternator_voltage_min) { // Below alternator_voltage_min it is unsure if the engine is running
      previous_time = current_time;
      return "stopped";
    } else if (engine_running_time > 0) {
      if (engine_running_time < initial_engine_running_time) {
        // Set engine_running_time to a certain initial value to start counting from
        engine_running_time = initial_engine_running_time;
      }
      // Calculate runTime in seconds
      engine_running_time += (ulong)(current_time-previous_time)/1000;
      // Store runTime
      preferences.putULong("main_runTime", engine_running_time);
      previous_time = current_time;
      return "running";
    } else { // A value of 0 is an indication that there is something wrong
      previous_time = current_time;
      return "parameter error";
    }
  });

  /**
   * Linear transformers for calibrating output values
   * 
   * linearTankLevelTransformer takes the tank voltage at minimum level (empty) and maximum level (full) in consideration to create a decent %
   * linearTankVolumeTransformer alse takes the tank capacity into consideration to create a decent volume (m3)
   */
  Linear *linearTankLevelTransformer = new Linear((1.0 / (tank_voltage_full - tank_voltage_empty)), ((-1.0 * tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));
  Linear *linearTankVolumeTransformer = new Linear((tank_capacity / (tank_voltage_full - tank_voltage_empty)), ((-tank_capacity * tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));

  // Connect the analog inputs to Signal K output. This will publish the
  // analog input values to the Signal K server every time it changes.
  //
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/type
  // Description: The type of tank
  // Enum values:
  //   petrol
  //   fresh water
  //   greywater
  //   blackwater
  //   holding
  //   lpg
  //   diesel
  //   liveWell
  //   baitWell
  //   ballast
  //   rum
  tank_voltage->connect_to(new LambdaTransform<float, String>([](float input) -> String { return tank_type; }))
              ->connect_to(new SKOutputString("tanks.fuel.0.type"));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/capacity
  // Units: m3 (Cubic meter)
  // Description: Total capacity
  tank_voltage->connect_to(new LambdaTransform<float, float>([](float input) -> float { return tank_capacity; }))
              ->connect_to(new SKOutputFloat("tanks.fuel.0.capacity", "", new SKMetadata("m3")));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/currentLevel
  // Units: ratio (Ratio)
  // Description: Level of fluid in tank 0.0-1.0 (documentation mentions 0-100% which is incorrect)
  tank_voltage->connect_to(new MovingAverage(3))
              ->connect_to(linearTankLevelTransformer)
              ->connect_to(new SKOutputFloat("tanks.fuel.0.currentLevel", "", new SKMetadata("ratio")));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/currentVolume
  // Units: m3 (Cubic meter)
  // Description: Volume of fluid in tank
  tank_voltage->connect_to(new MovingAverage(3))
              ->connect_to(linearTankVolumeTransformer)
              ->connect_to(new SKOutputFloat("tanks.fuel.0.currentVolume", "", new SKMetadata("m3")));
  ESP_LOGD(__FILE__, "tank_voltage Connected!");


  // /vessels/<RegExp>/propulsion/<RegExp>/label
  // Description: Human readable label for the propulsion unit
  alternator_voltage->connect_to(new LambdaTransform<float, String>([](float input) -> String { return "Diesel Engine"; }))
                    ->connect_to(new SKOutputString("propulsion.main.label"));
  // /vessels/<RegExp>/propulsion/<RegExp>/alternatorVoltage
  // Units: V (Volt)
  // Description: Alternator voltage
  alternator_voltage->connect_to(new SKOutputFloat("propulsion.main.alternatorVoltage", "", new SKMetadata("V")));
  // /vessels/<RegExp>/propulsion/<RegExp>/state
  // Values (free): "started", "stopped", ...
  // Description: The current state of the engine
  alternator_voltage->connect_to(measuredAlternatorVoltageToEngineStateTransformer)
                    ->connect_to(new SKOutputString("propulsion.main.state"));
  // /vessels/<RegExp>/propulsion/<RegExp>/runTime
  // Units: s (Second)
  // Description: Total running time for engine (Engine Hours in seconds)
  alternator_voltage->connect_to(new LambdaTransform<float, float>([](float input) -> float { return (float)engine_running_time; }))
                    ->connect_to(new SKOutputFloat("propulsion.main.runTime", "", new SKMetadata("s")));
  ESP_LOGD(__FILE__, "alternator_voltage Connected!");


  // /vessels/<RegExp>/electrical/batteries/<RegExp>/voltage
  // Units: V (Volt)
  // Description: Voltage measured at or as close as possible to the device
  battery0_voltage->connect_to(new SKOutputFloat("electrical.batteries.0.voltage", "", new SKMetadata("V")));
  battery1_voltage->connect_to(new SKOutputFloat("electrical.batteries.1.voltage", "", new SKMetadata("V")));


  // Start networking, SK server connections and other SensESP internals
  //debugD("Starting sensesp_app...");
  sensesp_app->start();
  //debugD("sensesp_app Started!");

  oledDisplay.drawIcons();

  #endif // ONLY_SETUP

  ESP_LOGI(__FILE__, "Setup phase complete!");
}

void loop() {
  event_loop()->tick();
}