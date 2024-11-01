# GoPro_BLE_Repeater
Repeats BLE communication between GoPro app and GoPro camera with an ESP32 to bridge larger distances

## Features
Works with both the GoPro Quick App and the [GoEasyPro_Android](https://github.com/sepp89117/GoEasyPro_Android) app

## Get started
You need an ESP32 board, the Arduino IDE (I use version 2.3.3) with the ESP32 Boards package (I use version 3.0.7) and the [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) lib from h2zero (I use version 1.4.2).

## Using
- Compile and upload the project to your ESP32
- Turn on the GoPro
- If the camera was not already connected to this ESP32 or reset, put it into app pairing mode
- The ESP32 will automatically connect to the camera within a few seconds
- Now you can open the app to control the camera and control your camera via the repeater
- You may need to pair the repeater with the app like a normal camera

## Special Thanks
Special thanks to h2zero for the great work with [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) and the great support!

## Known Issues
- Hero 5 Black cannot be paired with the Quick App via the repeater
- The repeater does not repeat WiFi connection
