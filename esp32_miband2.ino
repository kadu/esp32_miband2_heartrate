#include <BLEDevice.h>
#include <HardwareSerial.h>
#include <XBee.h>
#include <mbedtls/aes.h>
#include "uuid.h"
#include "utilize.h"


#define LED_B_PIN	22		// Pin of LED
#define SW_PIN_1	2		// Switch 1, connect GND to start one-shot mode
#define XBEE_SLEEP	15		// Output to control the XBee's sleep
// #define SW_PIN_2 15			// Switch 2, connect GND to start continuous mode

const std::string MI_LAB	= "f7:f3:ef:13:b1:3d";	// MAC of the target band
const char * dev_name		= "MiBand2";			// Name of the band, will be sent to XBee with HRM data and ctr


// v--- Info of XBee ---v
HardwareSerial XBeeSerial(1);
XBee xbee = XBee();
XBeeAddress64 ntrAddr64;

// AT Command
uint8_t dlCmd[] = {'D','L'};
uint8_t slCmd[] = {'S','L'};
AtCommandRequest atRequest;
AtCommandResponse atResponse;
uint8_t rtadl[4];
uint32_t rtAddressL = 0;
uint8_t ntradl[4];
uint32_t ntrAddressL = 0;
// ^--- Info of XBee ---^


// v--- Commands of MiBand 2 ---v
// For more information, please visit
// https://github.com/creotiv/MiBand2
// https://leojrfs.github.io/writing/miband2-part1-auth/
// Once _KEY is changed, MiBand 2 will see your device as a new client
static uint8_t	_KEY [18]			= {0x01, 0x00, 0x82, 0xb6, 0x5c, 0xd9, 0x91, 0x95, 0x9a, 0x72, 0xe5, 0xcc, 0xb7, 0xaf, 0x62, 0x33, 0xee, 0x35};
static uint8_t	encrypted_num[18]	= {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t	_send_rnd_cmd[2]	= {0x02, 0x00};
static uint8_t	none[2]				= {0, 0};
static uint8_t	auth_key[18];
// ^--- Commands of MiBand 2 ---^


// v--- Global variate ---v
uint8_t		f_start					= 0xff;		// 1: one-shot; 2: continuous
// bool		f_hrm					= false;	// A flag in continuous mode
bool		f_hrmc					= false;	// Is continuous mode start
// uint32_t	t_start, t_now;						// Time counter in continuous mode (heart beat packet every 16s)
uint8_t		hrm						= 0xff;		// Heart rate data from the band
uint16_t	ctr						= 0;		// Counter of HRM data
uint8_t		error_code				= 0xff;		// 0x01: devide not found; 0x02: device lost
uint8_t		crash_ctr				= 0;		// Use for the watchdog
uint16_t	watchdog_TO;						// Time-out of watchdog
// bool		f_isSD					= false;	// Abandoned since SD is not used
// ^--- Global variate ---^


// v--- Hard to explain ---v
// But necessary and NOTHING CAN BE CHANGED
enum authentication_flags {
	send_key = 0,
	require_random_number = 1,
	send_encrypted_number = 2,
	auth_failed, auth_success = 3,
	waiting = 4
};
enum dflag {
	error = -1,
	idle = 0,
	scanning = 1,
	connecting2Dev = 2,
	connecting2Serv = 3,
	established = 4,
	waiting4data = 5
};
authentication_flags	auth_flag;
mbedtls_aes_context		aes;
dflag					status = idle;
// ^--- Hard to explain ---^


// v--- Watchdog run in Core 1 ---v
void watchdog (void *parameter)
{
	while (1) {
		delay(watchdog_TO);
		if (crash_ctr == 0) {
			digitalWrite(XBEE_SLEEP, 0);
			delay(50);
			char crash_info[32];
			sprintf(crash_info, "%d,-1\r\n", ctr++);
			// Serial.println(crash_info);
			ZBTxRequest zbTx = ZBTxRequest(ntrAddr64, (uint8_t *)crash_info, strlen(crash_info));
			xbee.send(zbTx);
			digitalWrite(XBEE_SLEEP, 1);
			ESP.restart();
		} else {
			crash_ctr = 0;
		}
	}
}
// ^--- Watchdog run in Core 1 ---^


// *********************************
// ********* W A R N I N G *********
// No one can explain the codes here,
// YOU'D BETTER NOT TOUCH !!
// *********************************
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv

static void notifyCallback_auth(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
	switch (pData[1]) {
		case 0x01:
			if (pData[2] == 0x01) {
				auth_flag = require_random_number;
			}
			else {
				auth_flag = auth_failed;
			}
			break;
		case 0x02:
			if (pData[2] == 0x01) {
				mbedtls_aes_init(&aes);
				mbedtls_aes_setkey_enc(&aes, (auth_key + 2), 128);
				mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, pData + 3, encrypted_num + 2);
				mbedtls_aes_free(&aes);
				auth_flag = send_encrypted_number;
			} else {
				auth_flag = auth_failed;
			}
			break;
		case 0x03:
			if (pData[2] == 0x01) {
				auth_flag = auth_success;
			}
			else if (pData[2] == 0x04) {
				auth_flag = send_key;
			}
			break;
		default:
			auth_flag = auth_failed;
	}
}

static void notifyCallback_heartrate(BLERemoteCharacteristic* pHRMMeasureCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
	status = idle;
	hrm = pData[1];
	// Serial.printf("Get Heart Rate: ");
	// Serial.printf("%d\n", pData[1]);
	led_blink(LED_B_PIN, 500, 1);
	sendHRM2Xbee();
	crash_ctr++;
}


void sendHRM2Xbee() {
	digitalWrite(XBEE_SLEEP, 0);
	delay(50);
	char hrm_info[32];
	sprintf(hrm_info, "%d,%d\r\n", ctr++, hrm);
	log2(hrm_info);
	ZBTxRequest zbTx = ZBTxRequest(ntrAddr64, (uint8_t *)hrm_info, strlen(hrm_info));
	xbee.send(zbTx);
	digitalWrite(XBEE_SLEEP, 1);
}


void sendAtCommand_ADR(uint8_t content[4]) {
	xbee.send(atRequest);
	if (xbee.readPacket(5000)) {
		if (xbee.getResponse().getApiId() == AT_COMMAND_RESPONSE) {
			xbee.getResponse().getAtCommandResponse(atResponse);
			if (atResponse.isOk()) {
				if (atResponse.getValueLength() > 0) {
					for (int i = 0; i < atResponse.getValueLength(); i++) {
						content[i] = atResponse.getValue()[i];
					}
				}       
			}   
		}
	}
}


// Get router 64 low address (AT command: SL)
void getRT64adl () {
	atRequest.setCommand(slCmd);
	sendAtCommand_ADR(rtadl);
	atRequest.clearCommandValue();
	rtAddressL = rtadl[0]<<24 | rtadl[1]<<16 | rtadl[2]<<8 | rtadl[3];
}


// Get coordinator 64 low address (AT command: DL)
void getNTR64adl () {
	atRequest.setCommand(dlCmd);
	sendAtCommand_ADR(ntradl);
	atRequest.clearCommandValue();
	ntrAddressL = ntradl[0]<<24 | ntradl[1]<<16 | ntradl[2]<<8 | ntradl[3];
}


class DeviceSearcher: public BLEAdvertisedDeviceCallbacks {
public:
	void setDevAddr(std::string addr) {
		target_addr = addr;
	}
	
	void onResult (BLEAdvertisedDevice advertisedDevice) {
		std::string addr_now = advertisedDevice.getAddress().toString();
		if (addr_now.compare(target_addr) == 0) {
			pServerAddress = new BLEAddress(advertisedDevice.getAddress());
			advertisedDevice.getScan()->stop();
			f_found = true;
		}
	}
	
	bool isFound() {
		return f_found;
	}
	
	BLEAddress * getServAddr() {
		return pServerAddress;
	}
	
private:
	bool			f_found = false;
	std::string		target_addr;
	BLEAddress		* pServerAddress;
};


class MiBand2 {
public:
	MiBand2(std::string addr, const uint8_t * key) {
		dev_addr = addr;
		memcpy(auth_key, key, 18);
	}
	
	~MiBand2() {
		pClient->disconnect();
		log2("# Operation finished.");
	}
	
	bool scan4Device(uint8_t timeout) {
		DeviceSearcher * ds = new DeviceSearcher();
		ds->setDevAddr(dev_addr);
		
		BLEScan* pBLEScan = BLEDevice::getScan();
		pBLEScan->setAdvertisedDeviceCallbacks(ds);
		pBLEScan->setActiveScan(true);
		pBLEScan->start(timeout);
		
		if (!ds->isFound()) {
			return false;
		} else {
			pServerAddress = ds->getServAddr();
			pClient = BLEDevice::createClient();
			return true;
		}
	}
	
	bool connect2Server(BLEAddress pAddress) {
		pClient->connect(pAddress);
		log2("Connected to the device.");
		
		// ====================================================================
		// Get useful s/c/d of MI BAND 2
		// --------------------------------------------------------------------
		BLERemoteService * pRemoteService = pClient->getService(service2_uuid);
		if (pRemoteService == nullptr)
			return false;
		log2("MIBAND2");
		pRemoteCharacteristic = pRemoteService->getCharacteristic(auth_characteristic_uuid);
		log2(" |- CHAR_AUTH");
		if (pRemoteCharacteristic == nullptr)
			return false;
		
		pRemoteService = pClient->getService(alert_sev_uuid);
		log2("SVC_ALERT");
		pAlertCharacteristic = pRemoteService->getCharacteristic(alert_cha_uuid);
		log2(" |- CHAR_ALERT");
		
		pRemoteService = pClient->getService(heart_rate_sev_uuid);
		log2("SVC_HEART_RATE");
		pHRMControlCharacteristic = pRemoteService->getCharacteristic(UUID_CHAR_HRM_CONTROL);
		log2(" |- UUID_CHAR_HRM_CONTROL");
		pHRMMeasureCharacteristic = pRemoteService->getCharacteristic(UUID_CHAR_HRM_MEASURE);
		log2(" |- CHAR_HRM_MEASURE");
		cccd_hrm = pHRMMeasureCharacteristic->getDescriptor(CCCD_UUID);
		log2("   |- CCCD_HRM");
		f_connected = true;
		// ====================================================================

		// ====================================================================
		// Bind notification
		// --------------------------------------------------------------------
		pRemoteCharacteristic->registerForNotify(notifyCallback_auth);
		pHRMMeasureCharacteristic->registerForNotify(notifyCallback_heartrate);
		// ====================================================================
		return true;
	}
	
	void authStart() {
		auth_flag = require_random_number;
		BLERemoteDescriptor* pauth_descripter;
		pauth_descripter = pRemoteCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
		log2("   |- CCCD_AUTH");
		pauth_descripter->writeValue(auth_key, 2, true);
		// Serial.println("# Sent {0x01, 0x00} to CCCD_AUTH");
		while (auth_flag != auth_success) {
			// Serial.printf("# AUTH_FLAG: %d\n", auth_flag);
			authentication_flags seaved_flag = auth_flag;
			auth_flag = waiting;
			switch (seaved_flag) {
				case send_key:
					pRemoteCharacteristic->writeValue(auth_key, 18);
					// Serial.println("# Sent KEY to CCCD_AUTH");
					break;
				case require_random_number:
					pRemoteCharacteristic->writeValue(_send_rnd_cmd, 2);
					// Serial.println("# Sent RND_CMD to CCCD_AUTH");
					break;
				case send_encrypted_number:
					pRemoteCharacteristic->writeValue(encrypted_num, 18);
					// Serial.println("# Sent ENCRYPTED_NUM to CCCD_AUTH");
					break;
				default:
				;
			}
			if (auth_flag == seaved_flag) {
				auth_flag = waiting;
			}
			delay(100);
		}
		pauth_descripter->writeValue(none, 2, true);
		// Serial.println("# Sent NULL to CCCD_AUTH. AUTH process finished.");
		while (!f_connected && (auth_flag == auth_success));
		log2("# Auth succeed.");
		cccd_hrm->writeValue(HRM_NOTIFICATION, 2, true);
		log2("# Listening on the HRM_NOTIFICATION.");
	}
	
	void startHRM() {
		// Serial.println("# Sending HRM command...");
		pHRMControlCharacteristic->writeValue(HRM_CONTINUOUS_STOP, 3, true);
		pHRMControlCharacteristic->writeValue(HRM_CONTINUOUS_START, 3, true);
		// Serial.println("# Sent.");
	}
	
	void startHRM_oneshot() {
		if (status != waiting4data) {
			// Serial.println("# Sending HRM-OS command...");
			pHRMControlCharacteristic->writeValue(HRM_ONESHOT_STOP, 3, true);
			pHRMControlCharacteristic->writeValue(HRM_ONESHOT_START, 3, true);
			// Serial.println("# Sent.");
			status = waiting4data;
		} else {
			delay(20);
		}
	}

	void hrm_heartbeat() {
		pHRMControlCharacteristic->writeValue(HRM_HEARTBEAT, 1, true);
		// Serial.println("# Heart beat packet sent.");
	}
	
	void init(uint8_t timeout) {
		log2(dev_addr);
		log2("Scanning for device...");
		if (!scan4Device(timeout)) {
			log2("Device not found");
			return;
		}
		led_blink(LED_B_PIN, 50, 3);
		log2("Device found");
		log2("Connceting to services...");
		if (!connect2Server(*pServerAddress)) {
			log2("! Failed to connect to services");
			return;
		}
		led_blink(LED_B_PIN, 100, 5);
		authStart();
		status = established;
	}
	
	void deinit() {
		pHRMControlCharacteristic->writeValue(HRM_CONTINUOUS_STOP, 3, true);
		pHRMControlCharacteristic->writeValue(HRM_ONESHOT_STOP, 3, true);
		pClient->disconnect();
		log2("# Operation finished.");
	}
	
private:
	bool					f_found		= false;
	bool					f_connected	= false;

	std::string				dev_addr;
	BLEClient				* pClient;
	BLEAddress				* pServerAddress;
	BLERemoteCharacteristic	* pRemoteCharacteristic;
	BLERemoteCharacteristic	* pAlertCharacteristic;
	BLERemoteCharacteristic	* pHRMMeasureCharacteristic;
	BLERemoteCharacteristic	* pHRMControlCharacteristic;
	BLERemoteDescriptor		* cccd_hrm;
};

// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
// ********* W A R N I N G *********
// No one can explain the codes here,
// YOU'D BETTER NOT TOUCH !!
// *********************************
// *********************************

MiBand2		dev(MI_LAB, _KEY);					// Instance of the band

void setup() {
	pinMode(LED_B_PIN, OUTPUT);
	pinMode(XBEE_SLEEP, OUTPUT);
	pinMode(SW_PIN_1, INPUT);
	// pinMode(SW_PIN_2, INPUT);
	
	digitalWrite(LED_B_PIN, 1);
	
	// Serial.begin(115200);
	XBeeSerial.begin(115200, SERIAL_8N1, 3, 1);
	xbee.setSerial(XBeeSerial);
	digitalWrite(XBEE_SLEEP, 0);
	
	// v--- Read address from config in XBee ---v
	while (rtAddressL==0||rtAddressL==ntrAddressL) {
		delay(500);
		// Serial.println("Wait for RT");
		getRT64adl();
	}
	while (ntrAddressL==0||rtAddressL==ntrAddressL) {
		delay(500);
		// Serial.println("Wait for NTR");
		getNTR64adl();
	}
	ntrAddr64 = XBeeAddress64(0x0013a200, ntrAddressL);
	// Serial.println("Xbee connection test passed.");
	digitalWrite(XBEE_SLEEP, 1);
	
	// ^--- Read address from config in XBee ---^

	// Serial.printf("Connect PIN%d to start one-shot\n", SW_PIN_1);
	//// Serial.printf("Connect PIN%d to start one-shot\n", SW_PIN_2);

	while (1) {
		if (!digitalRead(SW_PIN_1)) {
			f_start = 1;
			log2("One-shot mode loading...");
			break;
		}/* else if (!digitalRead(SW_PIN_2)) {
			f_start = 2;
			log2("Continuous mode loading...");
			break;
		}*/
		delay(20);
	}
	
	digitalWrite(LED_B_PIN, 0);

	// Change config of watchdog to device-searching mode
	// *** If ESP-32 cannot find & connect to the BLE device in 20s, reboot
	watchdog_TO = 20000;
	error_code = 0x01;
	xTaskCreate(
		watchdog,
		"genericTask",
		10000,
		NULL,
		2,
		NULL);
	
	// Search & connect to the BLE device
	BLEDevice::init("ESP-WROOM-32");
	dev.init(10);
	
	// BLE device init successed
	crash_ctr++;
	
	// Change config of watchdog to data-collection mode
	// *** If there is no data comes in 30s, device lost, reboot
	watchdog_TO = 30000;
	error_code = 0x02;
}


void loop() {
	if (f_start == 1) {
		dev.startHRM_oneshot();
		delay(10000);
	}
	
	// It seems that nobody likes continuous mode...
	/*
	} else if (f_start == 2) {
		if (!f_hrmc) {
			dev.startHRM();
			delay(500);
			f_hrmc = true;
			t_start = millis();
		} else {
			t_now = millis();
			if (t_now - t_start >= 12000) {
				dev.hrm_heartbeat();
				t_start = t_now;
			}
		}
	}
	*/
	
	// if (digitalRead(SW_PIN_1) && digitalRead(SW_PIN_2)) {
	if (digitalRead(SW_PIN_1)) {
		f_start = 0;
		dev.deinit();
		delay(3000);
		esp_deep_sleep_start();
	}
}