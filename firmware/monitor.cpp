
#include "monitor.h"

Monitor::Monitor(int main_sensor_pin, int battery_volts_pin) {

	this->main_sensor_pin = main_sensor_pin;
	this->battery_volts_pin = battery_volts_pin;

	battery_value = 0;
	main_value = false;
 
}

void Monitor::Init() {

	pinMode(main_sensor_pin, INPUT_PULLUP);
	pinMode(battery_volts_pin, INPUT);

}

void Monitor::read_values() {
	static uint16_t main_filter = 0;
	int current_mval = 0;

	battery_value = analogRead(battery_volts_pin);

	// a low signal on the input pin means mains is present
	if (main_value) current_mval = !digitalRead(main_sensor_pin);
	else current_mval = digitalRead(main_sensor_pin);

	// 0xec00 leaves 10 bits for filter
	main_filter = (main_filter<<1) | current_mval | 0xec00;

	// mains status toggles when signal has been stable for 10 iterations
	if (main_filter == 0xfc00) {
		main_filter = 0;
		main_value = !main_value;
	}

	//SerialUSB.print("FILTER: "); SerialUSB.println(main_filter, BIN);
}


float Monitor::get_battery_volts() {
	return (float)battery_value / 1240.91;
}

String Monitor::get_main_status() {
	return main_value ? "ON" : "OFF";
}
