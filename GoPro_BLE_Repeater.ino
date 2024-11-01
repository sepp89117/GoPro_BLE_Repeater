/*
 *	Project: GoPro_BLE_Repeater
 *	GitHub: https://github.com/sepp89117/GoPro_BLE_Repeater
 *	Author: sepp89117
 *	Date: 2024-11-01
 */

#include <NimBLEDevice.h>

#define REPEATER_BLE_NAME "GoPro 0000" // Do NOT change!

#define DEBUG ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_DEBUG

// ** Custom GoPro services
#define GOPRO_SERVICE_UUID (uint16_t)0xfea6
#define WIFI_SERVICE_UUID "b5f90001-aa8d-11e3-9046-0002a5d5c51b"
#define NW_MANAGE_SERVICE_UUID "b5f90090-aa8d-11e3-9046-0002a5d5c51b"

// ** Custom GoPro characteristics
#define COMMAND_UUID "b5f90072-aa8d-11e3-9046-0002a5d5c51b"
#define COMMAND_RESP_UUID "b5f90073-aa8d-11e3-9046-0002a5d5c51b"
#define SETTINGS_UUID "b5f90074-aa8d-11e3-9046-0002a5d5c51b"
#define SETTINGS_RESP_UUID "b5f90075-aa8d-11e3-9046-0002a5d5c51b"
#define QUERY_UUID "b5f90076-aa8d-11e3-9046-0002a5d5c51b"
#define QUERY_RESP_UUID "b5f90077-aa8d-11e3-9046-0002a5d5c51b"
#define SENSOR_DATA_UUID "b5f90078-aa8d-11e3-9046-0002a5d5c51b"
#define SENSOR_DATA_RESP_UUID "b5f90079-aa8d-11e3-9046-0002a5d5c51b"

#define WIFI_SSID_UUID "b5f90002-aa8d-11e3-9046-0002a5d5c51b"
#define WIFI_PW_UUID "b5f90003-aa8d-11e3-9046-0002a5d5c51b"
#define WIFI_AP_POWER_UUID "b5f90004-aa8d-11e3-9046-0002a5d5c51b"
#define WIFI_STATE_UUID "b5f90005-aa8d-11e3-9046-0002a5d5c51b"
#define WIFI_KEY_UUID "b5f90006-aa8d-11e3-9046-0002a5d5c51b" // I don't know anything about it

#define NW_MANAGE_UUID "b5f90091-aa8d-11e3-9046-0002a5d5c51b"
#define NW_MANAGE_RESP_UUID "b5f90092-aa8d-11e3-9046-0002a5d5c51b"

#define READABLE_CHRS_LEN 14
struct ReadableChr
{
	NimBLEUUID srvUUID;
	NimBLEUUID chrUUID;
	std::string lastVal;
	bool readAgain = true;
} readableChrs[READABLE_CHRS_LEN];

ReadableChr *toRead = nullptr;

const uint8_t nw_pairing_completed[] = {0x0A, 0x03, 0x01, 0x08, 0x00, 0x12, 0x04, 'G', 'P', 'R', 'P'};

NimBLEServer *pServer = nullptr;
NimBLEClient *pClient = nullptr;
NimBLEClientCallbacks *clientCallbacks;
NimBLEAddress advertisedDeviceAddress;

SemaphoreHandle_t readSemaphore;

uint8_t goProModelID = 0x13; // HERO_5_BLACK as default
bool goProConnected = false;

void respCharacteristicCallback(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)
{
#if DEBUG
	Serial.print("Received data from GoPro Response Characteristic ");
	Serial.println(pRemoteCharacteristic->getUUID().toString().c_str());
#endif

	if (pServer == nullptr)
		return;

	NimBLECharacteristic *pLocalCharacteristic = pServer->getServiceByUUID(pRemoteCharacteristic->getRemoteService()->getUUID())
													 ->getCharacteristic(pRemoteCharacteristic->getUUID());
	if (pLocalCharacteristic)
	{
		pLocalCharacteristic->setValue(pData, length);
		pLocalCharacteristic->notify();
#if DEBUG
		Serial.println("Value is set to our repeation response characteristic and notified");
#endif
	}
	else
	{
		Serial.println("Local characteristic not found in our server!");
	}
}

void subscribeChrs()
{
	std::vector<NimBLERemoteService *> *goProServices = pClient->getServices(false);
	for (auto goProService : *goProServices)
	{
		// Retrieve all characteristics of this service
		std::vector<NimBLERemoteCharacteristic *> *goProCharacteristics = goProService->getCharacteristics();

		for (auto goProCharacteristic : *goProCharacteristics)
		{
			// Notify Characteristic if possible
			if (goProCharacteristic->canNotify())
			{
				goProCharacteristic->subscribe(true, respCharacteristicCallback);
#if DEBUG
				Serial.printf("Subscribed to GoPro characteristic '%s'\n", goProCharacteristic->getUUID().toString().c_str());
#endif
			}
		}
	}
}

void readChr(ReadableChr *readableChr, bool initialRead = false)
{
	if (!readableChr->readAgain && !initialRead)
		return;

#if DEBUG
	Serial.printf("Read characteristic '%s'\n", readableChr->chrUUID.toString().c_str());
#endif

	NimBLERemoteService *pRemoteService = pClient->getService(readableChr->srvUUID);
	if (!pRemoteService)
	{
		Serial.println("Remote service not found on GoPro.");
		return;
	}

	NimBLERemoteCharacteristic *pRemoteChar = pRemoteService->getCharacteristic(readableChr->chrUUID);
	if (!pRemoteChar)
	{
		Serial.println("Remote characteristic not found on GoPro.");
		return;
	}

	if (pRemoteChar->canRead())
	{
		readableChr->lastVal = pRemoteChar->readValue();
	}
	else
	{
		Serial.println("Remote characteristic not readable.");
	}
}

void readChrs(bool initialRead = false)
{
	for (uint8_t i = 0; i < READABLE_CHRS_LEN; i++)
	{
		readChr(&readableChrs[i], initialRead);
	}
}

void readGoProValueTask(void *parameter)
{
	while (true)
	{
		// Warte auf Signal zum Starten des Lesens
		if (xSemaphoreTake(readSemaphore, portMAX_DELAY) == pdTRUE)
		{
			if (toRead != nullptr)
			{
				readChr(toRead);
				toRead = nullptr; // Zurücksetzen
			}
		}
	}
}

bool scanForGoPro()
{
	Serial.println("Start BLE scan for 5 sec...");
	NimBLEScan *pBLEScan = NimBLEDevice::getScan();
	pBLEScan->setActiveScan(true);
	NimBLEScanResults results = pBLEScan->start(5, false);

	for (int i = 0; i < results.getCount(); i++)
	{
		NimBLEAdvertisedDevice advertisedDevice = results.getDevice(i);
		if (advertisedDevice.isAdvertisingService(NimBLEUUID(GOPRO_SERVICE_UUID)))
		{
			// get goProModelID from advertised manufacturer data
			const char *manufacturerData = advertisedDevice.getManufacturerData().c_str();
			uint8_t modelID = manufacturerData[4];

			Serial.printf("Found GoPro with model ID: 0x%02X\n", modelID);

			goProModelID = modelID;

			startAdv();
			advertisedDeviceAddress = advertisedDevice.getAddress();
			return true;
		}
	}

	Serial.println("No GoPro found!\n");
	return false;
}

bool connectToGoPro()
{
	Serial.println("Connect GoPro...");
	bool wasBonded = NimBLEDevice::isBonded(advertisedDeviceAddress);

	pClient = NimBLEDevice::createClient();
	pClient->setClientCallbacks(clientCallbacks);
	pClient->setConnectionParams(6, 6, 0, 500);
	pClient->setConnectTimeout(5);

	if (pClient->connect(advertisedDeviceAddress, false))
	{
#if DEBUG
		Serial.println("Secure connection...");
#endif
		if (!pClient->secureConnection())
		{
			Serial.println("Secure connection failed!");
			pClient->disconnect();
			return false;
		}

#if DEBUG
		Serial.println("Discover attributes...");
#endif
		if (!pClient->discoverAttributes())
		{
			Serial.println("Discover attributes failed!");
			pClient->disconnect();
			return false;
		}

		// makeServerCode(); // Used to build an plot code for initBLEServer()

		// Subscribe all notification Characteristic
		subscribeChrs();

		// Initial read all readable Characteristics
		readChrs(true);

		pClient->setConnectionParams(24, 32, 0, 500);

		if (!wasBonded)
		{
			Serial.println("Send paring completed to cam...");
			pClient->getService(NW_MANAGE_SERVICE_UUID)->getCharacteristic(NW_MANAGE_UUID)->writeValue(nw_pairing_completed, 11, false);
		}
#if DEBUG
		else
		{
			Serial.println("GoPro was already bonded. No pairing needed!");
		}
#endif
		Serial.println("GoPro BLE connected!");

		return true;
	}

	Serial.println("Connect GoPro failed!");
	stopAdv();

	Serial.println("NimBLEDevice delete All Bonds!");
	NimBLEDevice::deleteAllBonds();

	return false;
}

void writeCharacteristicToGoPro(NimBLECharacteristic *pLocalCharacteristic, std::string value)
{
	if (!pClient || !pClient->isConnected())
	{
		Serial.println("GoPro client not connected.");
		return;
	}

	NimBLERemoteService *pRemoteService = pClient->getService(pLocalCharacteristic->getService()->getUUID());
	if (!pRemoteService)
	{
		Serial.println("Remote service not found on GoPro.");
		return;
	}

	NimBLERemoteCharacteristic *pRemoteChar = pRemoteService->getCharacteristic(pLocalCharacteristic->getUUID());
	if (!pRemoteChar)
	{
		Serial.println("Remote characteristic not found on GoPro.");
		return;
	}

	if (pRemoteChar->canWrite())
	{
		pRemoteChar->writeValue(value);
#if DEBUG
		Serial.print("Value written to GoPro characteristic: ");
		Serial.println(pLocalCharacteristic->getUUID().toString().c_str());
#endif
	}
	else
	{
		Serial.println("Remote characteristic not writable.");
	}
}

class MyLocaleCharacteristicCallbacks : public NimBLECharacteristicCallbacks
{
	void onWrite(NimBLECharacteristic *pCharacteristic) override
	{
		std::string value = pCharacteristic->getValue();
#if DEBUG
		Serial.print("Write request received for local characteristic: ");
		Serial.println(pCharacteristic->getUUID().toString().c_str());
#endif
		writeCharacteristicToGoPro(pCharacteristic, value); // Daten an GoPro weiterleiten
	}

	void onRead(NimBLECharacteristic *pCharacteristic) override
	{
#if DEBUG
		Serial.print("Read request received for local characteristic: ");
		Serial.println(pCharacteristic->getUUID().toString().c_str());
#endif
		for (uint8_t i = 0; i < READABLE_CHRS_LEN; i++)
		{
			if (readableChrs[i].chrUUID != pCharacteristic->getUUID())
				continue;
#if DEBUG
			Serial.println("Write value for local characteristic...");
#endif
			pCharacteristic->setValue(readableChrs[i].lastVal);

			toRead = &readableChrs[i];
			xSemaphoreGive(readSemaphore);
			return;
		}

		Serial.print("Local characteristic not found in readableChrs!");
		pCharacteristic->setValue("");
	}
};

class MyServerCallbacks : public NimBLEServerCallbacks
{
	void onConnect(NimBLEServer *pCBServer)
	{
		Serial.println("App connected to server.");
	}

	void onDisconnect(NimBLEServer *pCBServer)
	{
		Serial.println("App disconnected from server.");
		if (goProConnected)
		{
			Serial.println("GoPro is still connected, so restart the advertising...");
			NimBLEDevice::startAdvertising();
		}
	}

	uint32_t onPassKeyRequest()
	{
		Serial.println("Server Passkey Request");
		return 123456;
	};

	bool onConfirmPIN(uint32_t pass_key)
	{
		Serial.println("Server Confirm PIN");
		return true;
	};

	void onAuthenticationComplete(ble_gap_conn_desc *desc)
	{
		if (!desc->sec_state.encrypted)
		{
			NimBLEDevice::getServer()->disconnect(desc->conn_handle);
			Serial.println("Encrypt connection failed - disconnecting client");
			return;
		}
#if DEBUG
		Serial.println("Encrypt connection successfully!");
#endif
	};
};

class MyGoProClientCallback : public NimBLEClientCallbacks
{
public:
	void onConnect(NimBLEClient *pClient)
	{
	}

	void onDisconnect(NimBLEClient *pClient)
	{
		Serial.println("GoPro disconnected!");
		// GoPro disconnected -> stop advertising and disconnect peer from server
		goProConnected = false;
		stopAdv();
		if (pServer != nullptr && pServer->getConnectedCount() > 0)
		{
			std::vector<uint16_t> peerDevices = pServer->getPeerDevices();
			pServer->disconnect(peerDevices[0]);
		}
	}

	bool onConnParamsUpdateRequest(NimBLEClient *pClient, const ble_gap_upd_params *params)
	{
		if (params->itvl_min < 24)
		{ /** 1.25ms units */
			return false;
		}
		else if (params->itvl_max > 40)
		{ /** 1.25ms units */
			return false;
		}
		else if (params->latency > 2)
		{ /** Number of intervals allowed to skip */
			return false;
		}
		else if (params->supervision_timeout > 100)
		{ /** 10ms units */
			return false;
		}

		return true;
	}

	uint32_t onPassKeyRequest()
	{
		return 123456;
	}

	bool onConfirmPIN(uint32_t pass_key)
	{
		return true;
	}

	void onAuthenticationComplete(ble_gap_conn_desc *desc)
	{
		if (!desc->sec_state.encrypted)
		{
			Serial.println("Encrypt connection failed -> disconnect");
			NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
			return;
		}
#if DEBUG
		Serial.println("Encrypt connection success.");
#endif
	}
};

static MyLocaleCharacteristicCallbacks localeCharacteristicCallbacks;

void initBLEDevice()
{
	// Init BLE
	NimBLEDevice::init(REPEATER_BLE_NAME);
	NimBLEDevice::setPower(ESP_PWR_LVL_P9);
	NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM, false); // BLE_OWN_ADDR_PUBLIC

	// Pairing parameters
	NimBLEDevice::setSecurityAuth(true, true, true);
	NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
	NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN | BLE_SM_PAIR_KEY_DIST_LINK);
	NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_SIGN | BLE_SM_PAIR_KEY_DIST_LINK);

	clientCallbacks = new MyGoProClientCallback();
}

void initBLEServer()
{
	uint8_t i = 0;
	pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());
	pServer->advertiseOnDisconnect(false); // We handle this ourselves, depending on whether a GoPro is connected or not.

	NimBLEService *localService1 = pServer->createService(NimBLEUUID((uint16_t)0x1804));
	NimBLECharacteristic *localCharacteristic1 = localService1->createCharacteristic(NimBLEUUID((uint16_t)0x2a07), 0x0002);
	localCharacteristic1->setCallbacks(&localeCharacteristicCallbacks);
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x1804), NimBLEUUID((uint16_t)0x2a07), "", false};

	NimBLEService *localService2 = pServer->createService(NimBLEUUID((uint16_t)0x180f));										   // Battery service
	/* NimBLECharacteristic *localCharacteristic2 =  */ localService2->createCharacteristic(NimBLEUUID((uint16_t)0x2a19), 0x0012); // Battery level
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180f), NimBLEUUID((uint16_t)0x2a19), "", true};

	NimBLEService *localService3 = pServer->createService(NimBLEUUID((uint16_t)0x180a)); // Device information
	NimBLECharacteristic *localCharacteristic3 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a29), 0x0002);
	localCharacteristic3->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic4 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a24), 0x0002);
	localCharacteristic4->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic5 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a25), 0x0002);
	localCharacteristic5->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic6 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a27), 0x0002);
	localCharacteristic6->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic7 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a26), 0x0002);
	localCharacteristic7->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic8 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a28), 0x0002);
	localCharacteristic8->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic9 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a23), 0x0002);
	localCharacteristic9->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic10 = localService3->createCharacteristic(NimBLEUUID((uint16_t)0x2a50), 0x0002);
	localCharacteristic10->setCallbacks(&localeCharacteristicCallbacks);
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a29), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a24), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a25), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a27), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a26), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a28), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a23), "", false};
	readableChrs[i++] = {NimBLEUUID((uint16_t)0x180a), NimBLEUUID((uint16_t)0x2a50), "", false};

	NimBLEService *localService4 = pServer->createService(WIFI_SERVICE_UUID);
	NimBLECharacteristic *localCharacteristic11 = localService4->createCharacteristic(WIFI_SSID_UUID, 0x000A);
	localCharacteristic11->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic12 = localService4->createCharacteristic(WIFI_PW_UUID, 0x000A);
	localCharacteristic12->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic13 = localService4->createCharacteristic(WIFI_AP_POWER_UUID, 0x0008);
	localCharacteristic13->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic14 = localService4->createCharacteristic(WIFI_STATE_UUID, 0x0022);
	localCharacteristic14->setCallbacks(&localeCharacteristicCallbacks);
	NimBLECharacteristic *localCharacteristic15 = localService4->createCharacteristic(WIFI_KEY_UUID, 0x0002);
	localCharacteristic15->setCallbacks(&localeCharacteristicCallbacks);
	readableChrs[i++] = {NimBLEUUID(WIFI_SERVICE_UUID), NimBLEUUID(WIFI_SSID_UUID), "", true};
	readableChrs[i++] = {NimBLEUUID(WIFI_SERVICE_UUID), NimBLEUUID(WIFI_PW_UUID), "", true};
	readableChrs[i++] = {NimBLEUUID(WIFI_SERVICE_UUID), NimBLEUUID(WIFI_STATE_UUID), "", true};
	readableChrs[i++] = {NimBLEUUID(WIFI_SERVICE_UUID), NimBLEUUID(WIFI_KEY_UUID), "", true};

	NimBLEService *localService5 = pServer->createService(NimBLEUUID(GOPRO_SERVICE_UUID));
	NimBLECharacteristic *localCharacteristic16 = localService5->createCharacteristic(COMMAND_UUID, 0x0008);
	localCharacteristic16->setCallbacks(&localeCharacteristicCallbacks);
	/* NimBLECharacteristic *localCharacteristic17 =  */ localService5->createCharacteristic(COMMAND_RESP_UUID, 0x0010);
	NimBLECharacteristic *localCharacteristic18 = localService5->createCharacteristic(SETTINGS_UUID, 0x0008);
	localCharacteristic18->setCallbacks(&localeCharacteristicCallbacks);
	/* NimBLECharacteristic *localCharacteristic19 =  */ localService5->createCharacteristic(SETTINGS_RESP_UUID, 0x0010);
	NimBLECharacteristic *localCharacteristic20 = localService5->createCharacteristic(QUERY_UUID, 0x0008);
	localCharacteristic20->setCallbacks(&localeCharacteristicCallbacks);
	/* NimBLECharacteristic *localCharacteristic21 =  */ localService5->createCharacteristic(QUERY_RESP_UUID, 0x0010);
	NimBLECharacteristic *localCharacteristic22 = localService5->createCharacteristic(SENSOR_DATA_UUID, 0x0008);
	localCharacteristic22->setCallbacks(&localeCharacteristicCallbacks);
	/* NimBLECharacteristic *localCharacteristic23 =  */ localService5->createCharacteristic(SENSOR_DATA_RESP_UUID, 0x0010);

	NimBLEService *localService6 = pServer->createService(NW_MANAGE_SERVICE_UUID);
	NimBLECharacteristic *localCharacteristic24 = localService6->createCharacteristic(NW_MANAGE_UUID, 0x0008);
	localCharacteristic24->setCallbacks(&localeCharacteristicCallbacks);
	/* NimBLECharacteristic *localCharacteristic25 =  */ localService6->createCharacteristic(NW_MANAGE_RESP_UUID, 0x0010);

	Serial.println("Starting all added services...");
	localService1->start();
	localService2->start();
	localService3->start();
	localService4->start();
	localService5->start();
	localService6->start();
}

NimBLEAddress getEspRndAddress()
{
	ble_addr_t addr{};

	addr.type = BLE_ADDR_RANDOM;
	ble_hs_id_copy_addr(BLE_ADDR_RANDOM, addr.val, NULL);

	return NimBLEAddress(addr);
}

void initAdv()
{
	NimBLEAdvertisementData data = NimBLEAdvertisementData();
	NimBLEAdvertisementData data2 = NimBLEAdvertisementData();

	Serial.println("AdvData.add(advFlags)");
	uint8_t advFlags[] = {0x02, 0x01, 0x06};
	data.addData(std::string((char *)&advFlags[0], sizeof(advFlags)));

	Serial.println("AdvData.add(advService)");
	uint8_t advService[] = {0x03, 0x02, 0xa6, 0xfe};
	data.addData(std::string((char *)&advService[0], sizeof(advService)));

	Serial.println("AdvData.add(manufacturerData)");
	const uint8_t *bleAddr = getEspRndAddress().getNative();

	// Set the model ID so that the app recognizes the camera model
	uint8_t manufacturerData[] = {0x0f, 0xff, 0xf2, 0x02, 0x01, 0x07, goProModelID, 0x3F, 0x00, bleAddr[0], bleAddr[1], bleAddr[2], bleAddr[3], bleAddr[4], bleAddr[5], 0x1e};
	data.addData(std::string((char *)&manufacturerData[0], sizeof(manufacturerData)));

	Serial.println("AdvData.add(localName)");
	uint8_t localName[] = {0x0b, 0x09, 0x47, 0x6f, 0x50, 0x72, 0x6f, 0x20, 0x30, 0x30, 0x30, 0x30};
	data2.addData(std::string((char *)&localName[0], sizeof(localName)));

	Serial.println("AdvData.add(serviceData)");
	uint8_t serviceData[] = {0x09, 0x16, 0xa6, 0xfe, 0x69, 0x5e, 0x4a, 0x33, 0x06, 0x36};
	data2.addData(std::string((char *)&serviceData[0], sizeof(serviceData)));

	NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
	pAdvertising->setAdvertisementData(data);
	pAdvertising->setScanResponseData(data2);
	pAdvertising->setScanResponse(true);
}

void startAdv()
{
	Serial.println("Init BLE Advertisement...");
	initAdv();

	Serial.println("Start our BLE advertising...");
	NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

	pAdvertising->start();
	Serial.println("BLE-Server is ready and waiting for connection.");
}

void stopAdv()
{
	Serial.println("Stop our servers advertising...");
	NimBLEDevice::stopAdvertising();
}

void makeServerCode()
{
	// Retrieve all services from cam
	std::vector<NimBLERemoteService *> *goProServices = pClient->getServices(false);

	Serial.println("<CODE>");

	uint8_t srvI = 0;
	uint8_t chrI = 0;
	for (auto goProService : *goProServices)
	{
		// Skip services that are automatically added to the server
		std::string goProServiceUUID = goProService->getUUID().toString();
		if (goProServiceUUID == "0x1800" || goProServiceUUID == "0x1801")
			continue;

		srvI++;
		const char *serviceUUID = goProService->getUUID().toString().c_str();
		if (serviceUUID[0] == '0' && serviceUUID[1] == 'x')
			Serial.printf("NimBLEService *localService%u = pServer->createService(NimBLEUUID((uint16_t)%s));\n", srvI, serviceUUID);
		else
			Serial.printf("NimBLEService *localService%u = pServer->createService(\"%s\");\n", srvI, goProService->getUUID().toString().c_str());

		// Retrieve all characteristics of this service
		std::vector<NimBLERemoteCharacteristic *> *goProCharacteristics = goProService->getCharacteristics();
		for (auto goProCharacteristic : *goProCharacteristics)
		{
			chrI++;
			uint16_t properties = goProCharacteristic->canBroadcast() ? NIMBLE_PROPERTY::BROADCAST : 0;
			properties |= goProCharacteristic->canIndicate() ? NIMBLE_PROPERTY::INDICATE : 0;
			properties |= goProCharacteristic->canNotify() ? NIMBLE_PROPERTY::NOTIFY : 0;
			properties |= goProCharacteristic->canRead() ? NIMBLE_PROPERTY::READ : 0;
			properties |= goProCharacteristic->canWrite() ? NIMBLE_PROPERTY::WRITE : 0;
			properties |= goProCharacteristic->canWriteNoResponse() ? NIMBLE_PROPERTY::WRITE_NR : 0;

			const char *characteristicUUID = goProCharacteristic->getUUID().toString().c_str();
			if (characteristicUUID[0] == '0' && characteristicUUID[1] == 'x')
				Serial.printf("NimBLECharacteristic *localCharacteristic%u = localService%u->createCharacteristic(NimBLEUUID((uint16_t)%s), 0x%04X);", chrI, srvI, characteristicUUID, properties);
			else
				Serial.printf("NimBLECharacteristic *localCharacteristic%u = localService%u->createCharacteristic(\"%s\", 0x%04X);", chrI, srvI, goProCharacteristic->getUUID().toString().c_str(), properties);

			if (goProCharacteristic->canNotify())
				Serial.println(" // this Characteristic has to be subscribed from GoPro");
			else
				Serial.printf("\nlocalCharacteristic%u->setCallbacks(&localeCharacteristicCallbacks);\n", chrI);
		}
	}

	Serial.println("// Start all services");
	srvI = 0;
	for (auto goProService : *goProServices)
	{
		// Skip services that are automatically added to the server
		std::string goProServiceUUID = goProService->getUUID().toString();
		if (goProServiceUUID == "0x1800" || goProServiceUUID == "0x1801")
			continue;

		srvI++;
		NimBLEService *existingService = pServer->getServiceByUUID(goProService->getUUID());
		if (existingService != nullptr)
			Serial.printf("localService%u.start();\n", srvI);
	}

	Serial.println("</CODE>");
}

void setup()
{
	Serial.begin(115200);
	while (!Serial)
		;

	Serial.println("Init BLE Device...");
	initBLEDevice();

	Serial.println("Init BLE Server...");
	initBLEServer();

	readSemaphore = xSemaphoreCreateBinary();
	xTaskCreate(readGoProValueTask, "ReadGoProTask", 4096, NULL, 1, NULL);

	Serial.println("Setup done.");
}

void loop()
{
	if (!goProConnected && scanForGoPro())
		goProConnected = connectToGoPro();

	vTaskDelay(100);
}
