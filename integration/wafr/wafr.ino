/*
 * Authors: Aaron Lim
 *          Justin Fraumeni
 *          Jen Becerra
 *          Christian Reidelsheimer
 *          
 * Project: WAFR - Wearable Alert for First Responders
 * 
 * Description: Wearable heart rate and spo2 monitor with motion detection
 * 
 * Organization: University of Rochester
 *               Department of Electrical & Computer Engineering
 *               Senior Design              
 * 
 * Date: 5 April 2017
 * 
 * Version: 1.0.1 BETA
 * 
 */

#include <Arduino.h>
#include <TinyWireM.h>
#include <USI_TWI_Master.h>
#include <Wire.h>

#include <Adafruit_MMA8451.h>
#include <Adafruit_Sensor.h>

#include "max30102_wire_driver.h"
#include "max30102_processing.h"

#include "BluefruitConfig.h"
#include <Adafruit_BLE.h>
#include <Adafruit_BluefruitLE_UART.h>
#include <inttypes.h>

#include <SoftwareSerial.h>

#define MAX30102_INTR 10 // The MAX30102 interrupt line will go to pin 10 on the Flora
//#define MMA8451_INTR 9 // The MMA8451 interrupt line will go to pin 9 on the Flora

#define ABN_HR_UPPER     90 // just for testing, actual value will be like 145 or something
#define ABN_HR_LOWER     70 // just for testing, actual value will be like 40 or something
#define ABN_SPO2_LOWER   95 // below this value spo2 is considered abnormal
#define ABN_LOOPS_THRESH 5  // Number of loops a value can be abnormal before it triggers a transmission

#define PACKET_SIZE 60 // Size of char array (string of data) to send over Bluetooth

// ------------------------ GLOBAL VARIABLE DECLARATIONS ------------------------

// MMA8451 variables
Adafruit_MMA8451 mma = Adafruit_MMA8451();
uint16_t xyz_buffer[3];

// MAX30102 DATA COLLECTION
uint32_t red_buffer[BUFFER_SIZE]; // red LED sensor data
uint32_t ir_buffer[BUFFER_SIZE]; // infrared LED sensor data

// HR CALCULATION
uint32_t num_peaks_arr[INITIAL_SAMPLE_SIZE];
uint32_t heart_rate_arr[INITIAL_SAMPLE_SIZE];
uint32_t total_num_peaks = 0;
int32_t heart_rate, avg_hr; // heart rate value
bool hr_valid; // indicator if heart rate calculation is valid

// SPO2 CALCULATION
uint32_t spo2_ratio_arr[INITIAL_SAMPLE_SIZE];
uint32_t spo2_arr[INITIAL_SAMPLE_SIZE];
float avg_ratio = 0;
uint32_t avg_spo2 = 0;
int32_t spo2; // SPO2 value
bool spo2_valid; // indicator to show if the SPO2 calculation is valid

uint16_t timer = 0;

// Distress Flags
int hr_abnormal = 0, spo2_abnormal = 0;
int horizontal = 0, orientation_change = 0;

// BLE variables
Adafruit_BluefruitLE_UART ble(Serial1, BLUEFRUIT_UART_MODE_PIN);
char update_packet[PACKET_SIZE + 1];

//---------------------------------------------------------------------------------

 void setup() {
  
  Wire.begin(); // Join the bus

  // SERIAL COMMUNICATION USED FOR DEBUGGING
  Serial.begin(115200);
  while(!Serial);

  // SETUP THE BLE BOARD
  if(!ble.begin(VERBOSE_MODE)) {
    Serial.println("Couldn't find Bluefruit. Reset WAFR...");
    while(1);
  }
  ble.factoryReset();
  ble.echo(false);
  // Change the device name
  ble.sendCommandCheckOK(F("AT+GAPDEVNAME=WAFR"));

  // INITIALIZE THE MMA8451 ACCELEROMETER
  mma.begin();
  mma.setRange(MMA8451_RANGE_2_G);
  mma.setDataRate(MMA8451_DATARATE_1_56_HZ);
  // pinMode(MMA8451_INTR, INPUT); // This may or may not be used...

  // INITIALIZE the MAX30102 PULSE OXIMETER
  max30102_reset();
  pinMode(MAX30102_INTR, INPUT);
  // Read/Clear the interrupt status registers
  max30102_clear_interrupt_status_regs();
  // Setup all max30102 registers
  max30102_init();

Serial.println("LOADING");
//--------------------- MAX30102 FIRST BUFFER LOAD ------------------------
  // Put 5 num_peak samples in num_peaks_arr
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) { // Collect 15s worth of peaks (heartbeats)
    // Collect 75 samples
    for(uint32_t s = 0; s < BUFFER_SIZE; s++) {
      while(digitalRead(MAX30102_INTR)==1); // Wait until the interrupt pin asserts
      // Read from max30102 FIFO
      // If there is nothing to read decrement the incrementor and try again.
      if(!max30102_read_fifo(red_buffer, ir_buffer, s)) { s--; }
    }
    num_peaks_arr[r] = get_num_peaks(ir_buffer);
    spo2_ratio_arr[r] = get_spo2_ratio(red_buffer, ir_buffer);
    Serial.println("...");
  }
//-------------------------------------------------------------------------

  num_peaks_arr[0] = num_peaks_arr[4]; // First read is always erroneous, so replace it                 

//---------------------- CALCULATE HR AND SPO2 -------------------------
  total_num_peaks = 0;
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) { total_num_peaks += num_peaks_arr[r]; }

  // CALCULATE HR
  heart_rate = PEAKS_TO_HR(total_num_peaks);
  if(heart_rate < 225 && heart_rate > 30) {
      avg_hr = 0;
      for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE - 1; r++) { // Shift heart rate array left by one
        heart_rate_arr[r] = heart_rate_arr[r+1];
        avg_hr += heart_rate_arr[r];
      }
      heart_rate_arr[INITIAL_SAMPLE_SIZE - 1] = heart_rate; // Add new heart rate to heart rate array
      avg_hr = (avg_hr + heart_rate) / INITIAL_SAMPLE_SIZE; // Calculate average HR from heart rate array
      hr_valid = 1;
  }
  else { hr_valid = 0; }

  // CALCULATE SPO2
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) { avg_ratio += spo2_ratio_arr[r]; }
  avg_ratio = (avg_ratio / (100 * INITIAL_SAMPLE_SIZE));
  spo2 = (uint32_t) ((-55.426 * avg_ratio * avg_ratio) + (50.129 * avg_ratio) + 89.612);
  if(spo2 <= 100 && spo2 >= 80) {
      avg_spo2 = 0;
      for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE-1; r++){
        spo2_arr[r] = spo2_arr[r+1];
        avg_spo2 += spo2_arr[r];
      }
      spo2_arr[INITIAL_SAMPLE_SIZE-1] = spo2;
      avg_spo2 = (avg_spo2 + spo2) / INITIAL_SAMPLE_SIZE;
      spo2_valid = 1;
  }
  else {spo2_valid = 0;}  
// ----------------------------------------------------------------------

  // Initialize HR and SPO2 buffers
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) {
    heart_rate_arr[r] = heart_rate; 
    spo2_arr[r] = spo2;
  }
  avg_hr = heart_rate;
  avg_hr = spo2;

  mma.read();
  
  Serial.println("COMPLETE\n");
 }

 void loop() {

  // Get a new sample from the MMA8451
  xyz_buffer[0] = mma.x;
  xyz_buffer[1] = mma.y;
  xyz_buffer[2] = mma.z;
  mma.read();

//--------------------- CHECK FOR CONDITIONS OF STRESS -------------------

  // Check if HR is too high or too low
  if( avg_hr < ABN_HR_LOWER || avg_hr > ABN_HR_UPPER ) { hr_abnormal++;} 
  else {hr_abnormal = 0;}
  
  // Check if spo2 is too low
  if( avg_spo2 < ABN_SPO2_LOWER ) {spo2_abnormal++;}
  else {spo2_abnormal = 0; }

  // Check if horizontal
  if(abs(mma.z) > 3200 || abs(mma.y) > 3200) {horizontal++;}
  else {horizontal = 0;}
  
  // Check if sudden orientation change
  // STILL NEED TO FIGURE THIS OUT

//------------------------------------------------------------------------

//-------------------- DISPLAY DATA FOR DEBUGGING ------------------------

// NOTE:
// Do not uncomment all of these Serial.print lines at once!
// You will run out of RAM and the code will not function properly.

//  Serial.print("HR: "); Serial.print(avg_hr);
//  Serial.print(" -- SPO2: "); Serial.print(avg_spo2);
//  Serial.print(" -- X: "); Serial.print(mma.x);
//  Serial.print(" -- Y: "); Serial.print(mma.y);
//  Serial.print(" -- Z: "); Serial.println(mma.z);
//  Serial.print(" -- HR_ABNORMAL: "); Serial.print(hr_abnormal);
//  Serial.print(" -- SPO2_ABNORMAL: "); Serial.print(spo2_abnormal);
//  Serial.print(" -- HZL: "); Serial.print(horizontal);
//  Serial.print(" -- OR_CHANGE: "); Serial.print(orientation_change);

//------------------------------------------------------------------------

//---------------------- DECIDE TO TRANSMIT DATA -------------------------

  if(hr_abnormal >= ABN_LOOPS_THRESH || 
     spo2_abnormal >= ABN_LOOPS_THRESH  || 
     horizontal >= ABN_LOOPS_THRESH || 
     orientation_change || 
     timer >= 5) {

    // Create data packet
    sprintf(update_packet, "%" PRIu32 ",%" PRIu32 ",%" PRId16 ",%" PRId16 ",%" PRId16 ",%" PRId16 ",%" PRId16
                          ",%" PRId16 ",%d ,%d ,%d ,%d", avg_hr, avg_spo2, 
                          xyz_buffer[0], xyz_buffer[1], xyz_buffer[2], mma.x, mma.y, mma.z, 
                          hr_abnormal, spo2_abnormal, horizontal, orientation_change);
    
    // Send data packet to the BLE board for transmission
    ble.print("AT+BLEUARTTX=");
    ble.println(update_packet);
    
    timer = 0;
    
  }

//------------------------------------------------------------------------

//----------------- GET A NEW SAMPLE FROM MAX30102 --------------------

  // Collect 75 new samples (3 seconds)
  for(uint32_t r = 0; r < BUFFER_SIZE; r++) {
    while(digitalRead(MAX30102_INTR) == 1); // Wait until the interrupt pin asserts
    if(!max30102_read_fifo(red_buffer, ir_buffer, r)) {
      r--; // If there is nothing to read decrement and try again
    }
  }

  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE - 1; r++) {
    num_peaks_arr[r] = num_peaks_arr[r+1];
    spo2_ratio_arr[r] = spo2_ratio_arr[r+1];
  }
  num_peaks_arr[INITIAL_SAMPLE_SIZE - 1] = get_num_peaks(ir_buffer);
  spo2_ratio_arr[INITIAL_SAMPLE_SIZE - 1] = get_spo2_ratio(red_buffer, ir_buffer);
  
//---------------------------------------------------------------------
  
//------------------- CALCULATE HR AND SPO2 ---------------------------
  total_num_peaks = 0;
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) { total_num_peaks += num_peaks_arr[r]; }

  // CALCULATE HR
  heart_rate = PEAKS_TO_HR(total_num_peaks);
  if(heart_rate < 225 && heart_rate > 30) {
      avg_hr = 0;
      for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE - 1; r++) { // Shift heart rate array left by one
        heart_rate_arr[r] = heart_rate_arr[r+1];
        avg_hr += heart_rate_arr[r];
      }
      heart_rate_arr[INITIAL_SAMPLE_SIZE - 1] = heart_rate; // Add new heart rate to heart rate array
      avg_hr = (avg_hr + heart_rate) / INITIAL_SAMPLE_SIZE; // Calculate average HR from heart rate array
      hr_valid = 1;
  }
  else { hr_valid = 0; }

  // CALCULATE SPO2
  for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE; r++) { avg_ratio += spo2_ratio_arr[r]; }
  avg_ratio = (avg_ratio / (100 * INITIAL_SAMPLE_SIZE));
  spo2 = (uint32_t) ((-55.426 * avg_ratio * avg_ratio) + (50.129 * avg_ratio) + 89.612);
  if(spo2 <= 100 && spo2 >= 80) {
      avg_spo2 = 0;
      for(uint32_t r = 0; r < INITIAL_SAMPLE_SIZE-1; r++){
        spo2_arr[r] = spo2_arr[r+1];
        avg_spo2 += spo2_arr[r];
      }
      spo2_arr[INITIAL_SAMPLE_SIZE-1] = spo2;
      avg_spo2 = (avg_spo2 + spo2) / INITIAL_SAMPLE_SIZE;
      spo2_valid = 1;
  }
  else {spo2_valid = 0;}
//---------------------------------------------------------------------
  timer++;
 }

