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

// *************************************************************************************
// ************************************ PARAMETERS *************************************
// *************************************************************************************

// Replace with your network credentials
const char *wifi_ssid = "your-wifi-devices-ssid";
const char *wifi_password = "your-wifi-devices-password";
const char *hostname = "esp32-enbafu-meter"; // ESP32 Engine, Battery & Fuel Meter (or whatever name you think is valid)

// Replace with your level meter measurements and tank volume
float tank_voltage_full = 3.134;   // 3.165; // Measured input voltage at full tank (maximum level, use debug info)
float tank_voltage_empty = 0.142;  // Measured input voltage at empty tank (minimum level, use debug info)
const char *tank_type = "diesel";  // Type of tank (diesel, petrol, rum, ...)
const float tank_capacity = 0.060; // Tank capacity in m3

// Replace with your alternator/battery measurements
// Note: It can be the case that there is a certain voltage on the alternator while the Engine is stopped. This
// happens for instance when the Alternator is connected to a Victron ArgoFET or similar. When determining
// dc_source_measured_voltage_min disconnect everything from the analog input of the ESP, connect the analog
// input of the ESP to ground and determine the dc_source_measured_voltage_min value from the debug info.
float dc_source_real_voltage_max = 14.33;      // Battery output voltage when fully charged and charger connected or alternator (use a decent multimeter)
float dc_source_measured_voltage_max = 3.134;  // Measured input voltage with dc_source_real_voltage_max source connected (use debug info)
float dc_source_measured_voltage_min = 0.142;  // Measured input voltage without anything connected and input shortened to ground (DO NOT CONNECT BATTERY OR ALTERNATOR TO GROUND, use debug info)
unsigned long initial_engine_running_time = 0; // Set this to whatever value you like the runtime counter to start at (only needed
                                               // at the first use or when flash has been cleared). You can safely keep it at 0.

// Signal K client config
// Important note: when changing the Signal K server address/connecting to a different Signal K server
// and therefore changing the sk_server_address value below, you need to CLEAR THE FLASH from the ESP
// as well because SensESP stores this value in its config path and prefers that value over whatever
// you feed the builder constructor below. After clearing flash, you might want to re-enter the last known
// engine runTime as the initial_engine_running_time value above to omit the counter starting from 0 again.
const char *sk_server_address = "192.168.20.1";
const uint32_t sk_server_port = 80;             // Keep this at 80 as the current version of SensESP does not support SSL

// *************************************************************************************
// ********************************** END PARAMETERS ***********************************
// *************************************************************************************

using namespace sensesp;

ReactESP app;
Preferences preferences;

unsigned long previous_time = 0;
unsigned long engine_running_time = 0;

// The setup function performs one-time application initialization.
void setup() {
  #ifndef SERIAL_DEBUG_DISABLED
    SetupSerialDebug(115200);
  #endif

  // Construct the global SensESPApp() object
  SensESPAppBuilder builder;
  auto sensesp_app = builder.set_hostname(hostname)
                           ->set_wifi(wifi_ssid, wifi_password)
                           ->set_sk_server(sk_server_address, sk_server_port)
                           ->get_app();

  // GPIO numbers to use for the analog inputs
  #ifdef SEEED_XIAO_ESP32C3
    const uint8_t tank_level_input_pin = 2;       // ADC1_CH0
    const uint8_t engine_runtime_input_pin = 3;   // ADC1_CH1
    const uint8_t battery0_voltage_input_pin = 4; // ADC1_CH2
    const uint8_t battery1_voltage_input_pin = 5; // ADC2_CH0 <= DISABLED IN CURRENT ESP VERSION
  #elif NODEMCU_32S
    const uint8_t tank_level_input_pin = 36;       // ADC1_CH0
    const uint8_t engine_runtime_input_pin = 39;   // ADC1_CH3
    const uint8_t battery0_voltage_input_pin = 34; // ADC1_CH6
    const uint8_t battery1_voltage_input_pin = 35; // ADC1_CH7
  #elif WEMOS_D1_MINI32
    const uint8_t tank_level_input_pin = 34;       // ADC0
    const uint8_t engine_runtime_input_pin = 35;   // ADC1
    const uint8_t battery0_voltage_input_pin = 36; // ADC2
    const uint8_t battery1_voltage_input_pin = 39; // ADC3
  #endif
  // Define how often (in milliseconds) new samples are acquired
  const unsigned int analog_input_read_interval = 5000;
  // Define the produced value at the maximum input voltage (3.3V).
  // A value of 3.3 gives output equal to the input voltage.
  const float analog_input_scale = 3.3;

  // Retrieve the last known engine_running_time value
  if (preferences.begin("propulsion", false)) {
    // When opening the preferences returns a false, the value of engine_running_time will remain 0
    // which will "block" the storage of the parameter further on. Also it will be kept at 0 and shown
    // as such (and the text "parameter error") in the displays so the user will know there is something
    // wrong with the device.
    engine_running_time = preferences.getULong("main_runTime", 1); // Set to default at 1 to get it off the "error" value of 0
  }

  // Create a new Analog Input Sensor that reads an analog input pin periodically.
  auto* tank_level = new AnalogInput(tank_level_input_pin, analog_input_read_interval, "", analog_input_scale);
  auto* engine_runtime = new AnalogInput(engine_runtime_input_pin, analog_input_read_interval, "", analog_input_scale);
  auto* battery0_voltage = new AnalogInput(battery0_voltage_input_pin, analog_input_read_interval, "", analog_input_scale);
  auto* battery1_voltage = new AnalogInput(battery1_voltage_input_pin, analog_input_read_interval, "", analog_input_scale);
  debugD("tank_level, engine_runtime and batteryx_voltage Instantiated!");

  // Add observers that print out the current value of the analog inputs and some calculations every time it changes.
  tank_level->attach([tank_level]() {
    debugD("\n\nAvailable tank_level values:\ntank_voltage_empty = %f\ntank_voltage_full = %f\nvoltage measured = %f\ncalculated level = %f\n", tank_voltage_empty, tank_voltage_full, tank_level->get(), ((tank_level->get() - tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));
  });
  engine_runtime->attach([engine_runtime]() {
    debugD("\n\nAvailable engine_runtime values:\ndc_source_measured_voltage_min = %f\ndc_source_measured_voltage_max = %f\nvoltage measured = %f\ncalculated alternator voltage = %f\n", dc_source_measured_voltage_min, dc_source_measured_voltage_max, engine_runtime->get(), (dc_source_real_voltage_max * (engine_runtime->get() - dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)));
  });
  battery0_voltage->attach([battery0_voltage]() {
    debugD("\n\nAvailable battery0_voltage values:\ndc_source_measured_voltage_min = %f\ndc_source_measured_voltage_max = %f\nvoltage measured = %f\ncalculated battery0 voltage = %f\n", dc_source_measured_voltage_min, dc_source_measured_voltage_max, battery0_voltage->get(), (dc_source_real_voltage_max * (battery0_voltage->get() - dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)));
  });
  battery1_voltage->attach([battery1_voltage]() {
    debugD("\n\nAvailable battery1_voltage values:\ndc_source_measured_voltage_min = %f\ndc_source_measured_voltage_max = %f\nvoltage measured = %f\ncalculated battery1 voltage = %f\n", dc_source_measured_voltage_min, dc_source_measured_voltage_max, battery1_voltage->get(), (dc_source_real_voltage_max * (battery1_voltage->get() - dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)));
  });
  debugD("tank_level, engine_runtime and batteryx_voltage Attached!");

  // Lambda transformer for registering and storing the Engine Running Time and returning the Engine State
  auto measuredVoltageToEngineStateTransformer = new LambdaTransform<float, String>([](float input) -> String {
    unsigned long current_time = millis();
    if (input < 1.0) {
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
    } else {
      previous_time = current_time;
      return "parameter error";
    }
  });

  // Linear transformers for calibrating output values
  // The linearMeasuredXYZVoltageTransformer objects are identical but do not allow re-using them (connecting them)
  // from different input objects. In such case, readings would become similar as if the ESP32 inputs are connected.
  Linear *linearTankLevelTransformer = new Linear((1.0 / (tank_voltage_full - tank_voltage_empty)), ((-1.0 * tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));
  Linear *linearTankVolumeTransformer = new Linear((tank_capacity / (tank_voltage_full - tank_voltage_empty)), ((-tank_capacity * tank_voltage_empty) / (tank_voltage_full - tank_voltage_empty)));
  Linear *linearMeasuredAlternatorVoltageTransformer = new Linear(
    (dc_source_real_voltage_max / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)),
    ((-dc_source_real_voltage_max * dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min))
  );
  Linear *linearMeasuredBattery0VoltageTransformer = new Linear(
    (dc_source_real_voltage_max / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)),
    ((-dc_source_real_voltage_max * dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min))
  );
  Linear *linearMeasuredBattery1VoltageTransformer = new Linear(
    (dc_source_real_voltage_max / (dc_source_measured_voltage_max - dc_source_measured_voltage_min)),
    ((-dc_source_real_voltage_max * dc_source_measured_voltage_min) / (dc_source_measured_voltage_max - dc_source_measured_voltage_min))
  );

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
  tank_level->connect_to(new LambdaTransform<float, String>([](float input) -> String { return tank_type; }))
            ->connect_to(new SKOutputString("tanks.fuel.0.type"));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/capacity
  // Units: m3 (Cubic meter)
  // Description: Total capacity
  tank_level->connect_to(new LambdaTransform<float, float>([](float input) -> float { return tank_capacity; }))
            ->connect_to(new SKOutputFloat("tanks.fuel.0.capacity", "", new SKMetadata("m3")));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/currentLevel
  // Units: ratio (Ratio)
  // Description: Level of fluid in tank 0.0-1.0 (documentation mentions 0-100% which is incorrect)
  tank_level->connect_to(new MovingAverage(3))
            ->connect_to(linearTankLevelTransformer)
            ->connect_to(new SKOutputFloat("tanks.fuel.0.currentLevel", "", new SKMetadata("ratio")));
  // /vessels/<RegExp>/tanks/fuel/<RegExp>/currentVolume
  // Units: m3 (Cubic meter)
  // Description: Volume of fluid in tank
  tank_level->connect_to(new MovingAverage(3))
            ->connect_to(linearTankVolumeTransformer)
            ->connect_to(new SKOutputFloat("tanks.fuel.0.currentVolume", "", new SKMetadata("m3")));
  debugD("tank_level Connected!");


  // /vessels/<RegExp>/propulsion/<RegExp>/label
  // Description: Human readable label for the propulsion unit
  engine_runtime->connect_to(new LambdaTransform<float, String>([](float input) -> String { return "Diesel Engine"; }))
                ->connect_to(new SKOutputString("propulsion.main.label"));
  // /vessels/<RegExp>/propulsion/<RegExp>/alternatorVoltage
  // Units: V (Volt)
  // Description: Alternator voltage
  engine_runtime->connect_to(linearMeasuredAlternatorVoltageTransformer)
                ->connect_to(new SKOutputFloat("propulsion.main.alternatorVoltage", "", new SKMetadata("V")));
  // /vessels/<RegExp>/propulsion/<RegExp>/state
  // Values (free): "started", "stopped", ...
  // Description: The current state of the engine
  engine_runtime->connect_to(measuredVoltageToEngineStateTransformer)
                ->connect_to(new SKOutputString("propulsion.main.state"));
  // /vessels/<RegExp>/propulsion/<RegExp>/runTime
  // Units: s (Second)
  // Description: Total running time for engine (Engine Hours in seconds)
  engine_runtime->connect_to(new LambdaTransform<float, float>([](float input) -> float { return (float)engine_running_time; }))
                ->connect_to(new SKOutputFloat("propulsion.main.runTime", "", new SKMetadata("s")));
  debugD("engine_runtime Connected!");


  // /vessels/<RegExp>/electrical/batteries/<RegExp>/voltage
  // Units: V (Volt)
  // Description: Voltage measured at or as close as possible to the device
  battery0_voltage->connect_to(linearMeasuredBattery0VoltageTransformer)
                  ->connect_to(new SKOutputFloat("electrical.batteries.0.voltage", "", new SKMetadata("V")));
  battery1_voltage->connect_to(linearMeasuredBattery1VoltageTransformer)
                  ->connect_to(new SKOutputFloat("electrical.batteries.1.voltage", "", new SKMetadata("V")));


  // Start networking, SK server connections and other SensESP internals
  debugD("Starting sensesp_app...");
  sensesp_app->start();
  debugD("sensesp_app Started!");
}

void loop() {
  app.tick();
}
