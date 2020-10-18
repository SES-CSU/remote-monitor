/*




*/

#include "reporter.h"

Reporter reporter;

// create a WiFi UDP object for NTP to use
WiFiUDP wifiUdp;
// create an NTP object
NTP ntp(wifiUdp);

Reporter::Reporter() {

}

void Reporter::Init() {

		if (!Connect_Wifi()) {
			// no wifi, assume radio only state
			reporter_state = RADIO_ONLY;

		}
		else {
			// get current UTC time
			getTime();

			SerialUSB.println("Getting IoT Hub host from Azure IoT DPS");

			iotc.deviceId = IOTC_DEVICEID;
		//	sharedAccessKey = IOTC_DEVICEKEY;
			char hostName[64] = {0};
			getHubHostName(IOTC_SCOPEID, IOTC_DEVICEID, IOTC_DEVICEKEY, hostName);
			iotc.iothubHost = hostName;

			// create SAS token and user name for connecting to MQTT broker
			String url = iotc.iothubHost + urlEncode(String((char*)F("/devices/") + iotc.deviceId).c_str());
			//char *devKey = IOTC_DEVICEKEY;

			long expire = ntp.epoch() + TIMETOLIVE;
			iotc.sasToken = createIotHubSASToken(IOTC_DEVICEKEY, url, expire);

			iotc.username = iotc.iothubHost + "/" + iotc.deviceId + (char*)F("/api-version=2016-11-14");

			// connect to the IoT Hub MQTT broker
			wifiClient.connect(iotc.iothubHost.c_str(), 8883);
			mqtt_client = new PubSubClient(iotc.iothubHost.c_str(), 8883, wifiClient);
			mqtt_client->setBufferSize(2048);

			if(!connectMQTT(iotc.deviceId, iotc.username, iotc.sasToken)) {
				// if we cant connect initially, assume radio only state and notify operator
				wifiClient.stop();
				delete(mqtt_client);
				reporter_state = RADIO_ONLY;
			}
			else {
				// successfully connected
				reporter_state = MQTT;
			}
		}
}

bool Reporter::Connect_Wifi() {

	int status = WL_IDLE_STATUS;
	int retry = 0;
		while (status != WL_CONNECTED && retry < 5) {

				SerialUSB.print("Attempting to connect to Wi-Fi SSID: ");
				SerialUSB.println(WIFI_SSID);

				status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
				delay(1000);
				if (status == WL_CONNECTED) return true;
				retry++;
		}
		return false;
}

void Reporter::report_data(int mains_status, float battery_avg) {

	// INTERNET
	if (reporter_state == MQTT) {

		// send telemetry values
		if (mqtt_client->connected()) {
				SerialUSB.println(F("Sending telemetry ..."));

				String topic = (String)IOT_EVENT_TOPIC;
				topic.replace(F("{device_id}"), iotc.deviceId);

				char buff[10];
				String payload = F("{\"MainsStatus\": \"{mains}\", \"BatteryVoltage\": {battery}}");

				payload.replace(F("{mains}"), mains_status ? "ON" : "OFF");//);
				payload.replace(F("{battery}"), String(battery_avg,1));

				SerialUSB.println(payload.c_str());
				mqtt_client->publish(topic.c_str(), payload.c_str());
		}
		else {
			// radio if levels low

			try_reconnect();
		}

	}
	// RADIO -- RADIO_ONLY or MQTT_LOST states
	else {
		// radio if levels low

		if (reporter_state == MQTT_LOST) {
			try_reconnect();
		}
	}
}

void Reporter::mqtt_loop() {
	if (reporter_state == MQTT) mqtt_client->loop();
}

/* called if reporter has lost connection to wifi or mqtt broker */
void Reporter::try_reconnect() {

	if (WiFi.status() != WL_CONNECTED) {
		if (!Connect_Wifi()) {
			reporter_state = MQTT_LOST;
			return;
		}
	}
	wifiClient.stop();
	wifiClient.connect(iotc.iothubHost.c_str(), 8883);
	//mqtt_client = new PubSubClient(iotc.iothubHost.c_str(), 8883, wifiClient);

	if(!connectMQTT(iotc.deviceId, iotc.username, iotc.sasToken)) {
		reporter_state = MQTT_LOST;
	}
	else {
		reporter_state = MQTT;
	}

}

bool Reporter::connectMQTT(String deviceId, String username, String password) {
		mqtt_client->disconnect();

		SerialUSB.println(F("Starting IoT Hub connection"));
		int retry = 0;

		while(retry < 10 && !mqtt_client->connected() && WiFi.status() == WL_CONNECTED) {

				if (mqtt_client->connect(deviceId.c_str(), username.c_str(), password.c_str())) {
					SerialUSB.println(F("===> mqtt connected"));
					return true;
				}
				else {
					SerialUSB.print(F("---> mqtt failed, rc="));
					SerialUSB.println(mqtt_client->state());
					delay(1000);
					retry++;
				}
		}
		return false;
}

String Reporter::createIotHubSASToken(const char *key, String url, long expire) {
		url.toLowerCase();
		String stringToSign = url + "\n" + String(expire);
		int keyLength = strlen(key);

		int decodedKeyLength = base64_dec_len((char*)key, keyLength);
		char decodedKey[decodedKeyLength];

		base64_decode(decodedKey, (char*)key, keyLength);

		Sha256 *sha256 = new Sha256();
		sha256->initHmac((const uint8_t*)decodedKey, (size_t)decodedKeyLength);
		sha256->print(stringToSign);
		char* sign = (char*) sha256->resultHmac();
		int encodedSignLen = base64_enc_len(HASH_LENGTH);
		char encodedSign[encodedSignLen];
		base64_encode(encodedSign, sign, HASH_LENGTH);
		delete(sha256);

		return (char*)F("SharedAccessSignature sr=") + url + (char*)F("&sig=") \
		+ urlEncode((const char*)encodedSign) + (char*)F("&se=") + String(expire);
}

void Reporter::getTime() {
		SerialUSB.println(F("Getting the time from time service: "));

		ntp.begin();
		ntp.update();
		SerialUSB.print(F("Current time: "));
		SerialUSB.print(ntp.formattedTime("%d. %B %Y - "));
		SerialUSB.println(ntp.formattedTime("%A %T"));
}


//-----------------------------------


int Reporter::getDPSAuthString(const char* scopeId, const char* deviceId, const char* key, char *buffer, int bufferSize, size_t &outLength) {
	// update the time
	ntp.update();
	unsigned long expiresSecond = ntp.epoch() + 7200;
	assert(expiresSecond > 7200);

	String deviceIdEncoded = urlEncode(deviceId);
	char dataBuffer[AUTH_BUFFER_SIZE] = {0};
	size_t size = snprintf(dataBuffer, AUTH_BUFFER_SIZE, "%s%%2Fregistrations%%2F%s", scopeId, deviceIdEncoded.c_str());
	assert(size < AUTH_BUFFER_SIZE); dataBuffer[size] = 0;
	String sr = dataBuffer;
	size = snprintf(dataBuffer, AUTH_BUFFER_SIZE, "%s\n%lu000", sr.c_str(), expiresSecond);
	const size_t dataBufferLength = size;
	assert(dataBufferLength < AUTH_BUFFER_SIZE); dataBuffer[dataBufferLength] = 0;

	char keyDecoded[AUTH_BUFFER_SIZE] = {0};
	size = base64_decode(keyDecoded, (char*)key, strlen(key));
	assert(size < AUTH_BUFFER_SIZE && keyDecoded[size] == 0);
	const size_t keyDecodedLength = size;

	Sha256 *sha256 = new Sha256();
	sha256->initHmac((const uint8_t*)keyDecoded, (size_t)keyDecodedLength);
	sha256->print(dataBuffer);
	char* sign = (char*) sha256->resultHmac();
	int encodedSignLen = base64_enc_len(HASH_LENGTH);
	char encodedSign[encodedSignLen];
	base64_encode(encodedSign, sign, HASH_LENGTH);
	delete(sha256);

	String auth = urlEncode(encodedSign);
	outLength = snprintf(buffer, bufferSize, "authorization: SharedAccessSignature sr=%s&sig=%s&se=%lu000&skn=registration", sr.c_str(), auth.c_str(), expiresSecond);
	buffer[outLength] = 0;

	return 0;
}

int Reporter::_getOperationId(const char* scopeId, const char* deviceId, char* authHeader, char *operationId) {
	WiFiSSLClient client;
	if (client.connect(AZURE_IOT_CENTRAL_DPS_ENDPOINT, 443)) {
		char tmpBuffer[TEMP_BUFFER_SIZE] = {0};
		String deviceIdEncoded = urlEncode(deviceId);
		size_t size = snprintf(tmpBuffer, TEMP_BUFFER_SIZE,
			"PUT /%s/registrations/%s/register?api-version=2018-11-01 HTTP/1.0", scopeId, deviceIdEncoded.c_str());
		assert(size != 0); tmpBuffer[size] = 0;
		client.println(tmpBuffer);
		client.println("Host: global.azure-devices-provisioning.net");
		client.println("content-type: application/json; charset=utf-8");
		client.println("user-agent: iot-central-client/1.0");
		client.println("Accept: */*");
		size = snprintf(tmpBuffer, TEMP_BUFFER_SIZE,
			"{\"registrationId\":\"%s\"}", deviceId);
		assert(size != 0); tmpBuffer[size] = 0;
		String regMessage = tmpBuffer;
		size = snprintf(tmpBuffer, TEMP_BUFFER_SIZE,
			"Content-Length: %d", regMessage.length());
		assert(size != 0); tmpBuffer[size] = 0;
		client.println(tmpBuffer);

		client.println(authHeader);
		client.println("Connection: close");
		client.println();
		client.println(regMessage.c_str());

		delay(2000); // give 2 secs to server to process
		memset(tmpBuffer, 0, TEMP_BUFFER_SIZE);
		int index = 0;
		while (client.available() && index < TEMP_BUFFER_SIZE - 1) {
			tmpBuffer[index++] = client.read();
		}
		tmpBuffer[index] = 0;
		const char* operationIdString= "{\"operationId\":\"";
		index = indexOf(tmpBuffer, TEMP_BUFFER_SIZE, operationIdString, strlen(operationIdString), 0);
		if (index == -1) {
error_exit:
			Serial.println("ERROR: Error from DPS endpoint");
			Serial.println(tmpBuffer);
			return 1;
		} else {
			index += strlen(operationIdString);
			int index2 = indexOf(tmpBuffer, TEMP_BUFFER_SIZE, "\"", 1, index + 1);
			if (index2 == -1) goto error_exit;
			tmpBuffer[index2] = 0;
			strcpy(operationId, tmpBuffer + index);
			// Serial.print("OperationId:");
			// Serial.println(operationId);
			client.stop();
		}
	} else {
		Serial.println("ERROR: Couldn't connect AzureIOT DPS endpoint.");
		return 1;
	}

	return 0;
}

int Reporter::_getHostName(const char *scopeId, const char*deviceId, char *authHeader, char*operationId, char* hostName) {
	WiFiSSLClient client;
	if (!client.connect(AZURE_IOT_CENTRAL_DPS_ENDPOINT, 443)) {
		Serial.println("ERROR: DPS endpoint GET call has failed.");
		return 1;
	}
	char tmpBuffer[TEMP_BUFFER_SIZE] = {0};
	String deviceIdEncoded = urlEncode(deviceId);
	size_t size = snprintf(tmpBuffer, TEMP_BUFFER_SIZE,
		"GET /%s/registrations/%s/operations/%s?api-version=2018-11-01 HTTP/1.1", scopeId, deviceIdEncoded.c_str(), operationId);
	assert(size != 0); tmpBuffer[size] = 0;
	client.println(tmpBuffer);
	client.println("Host: global.azure-devices-provisioning.net");
	client.println("content-type: application/json; charset=utf-8");
	client.println("user-agent: iot-central-client/1.0");
	client.println("Accept: */*");
	client.println(authHeader);
	client.println("Connection: close");
	client.println();
	delay(5000); // give 5 secs to server to process
	memset(tmpBuffer, 0, TEMP_BUFFER_SIZE);
	int index = 0;
	while (client.available() && index < TEMP_BUFFER_SIZE - 1) {
		tmpBuffer[index++] = client.read();
	}
	tmpBuffer[index] = 0;
	const char* lookFor = "\"assignedHub\":\"";
	index = indexOf(tmpBuffer, TEMP_BUFFER_SIZE, lookFor, strlen(lookFor), 0);
	if (index == -1) {
		Serial.println("ERROR: couldn't get assignedHub. Trying again..");
		Serial.println(tmpBuffer);
		return 2;
	}
	index += strlen(lookFor);
	int index2 = indexOf(tmpBuffer, TEMP_BUFFER_SIZE, "\"", 1, index + 1);
	memcpy(hostName, tmpBuffer + index, index2 - index);
	hostName[index2-index] = 0;
	client.stop();
	return 0;
}

int Reporter::getHubHostName(const char *scopeId, const char* deviceId, const char* key, char *hostName) {
	char authHeader[AUTH_BUFFER_SIZE] = {0};
	size_t size = 0;
	//Serial.println("- iotc.dps : getting auth...");
	if (getDPSAuthString(scopeId, deviceId, key, (char*)authHeader, AUTH_BUFFER_SIZE, size)) {
		Serial.println("ERROR: getDPSAuthString has failed");
		return 1;
	}
	//Serial.println("- iotc.dps : getting operation id...");
	char operationId[AUTH_BUFFER_SIZE] = {0};
	if (_getOperationId(scopeId, deviceId, authHeader, operationId) == 0) {
		delay(4000);
		//Serial.println("- iotc.dps : getting host name...");
		while( _getHostName(scopeId, deviceId, authHeader, operationId, hostName) == 2) delay(5000);
		return 0;
	}
}
