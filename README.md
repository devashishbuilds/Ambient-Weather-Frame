# Ambient Weather Frame

A physical IoT wall frame that turns daylight and weather data into ambient motion and light.

The illuminated sun moves vertically through the artwork using the **real sunrise and sunset times** for a configurable location. The current prototype uses an **ESP32-C3**, a continuous-rotation servo, a rack-and-pinion mechanism, two endpoint touch sensors, and addressable LEDs.

The longer-term goal is to make useful weather information visible near the exit of a home: if rain or thunderstorms are expected, a cloud / raindrop / lightning layer can make that visible before you leave.

## Why I built it

I often noticed rain only after leaving home, even when a forecast notification had already appeared on my phone. I wanted to explore a different interaction model: instead of another screen or notification, make the forecast part of the physical environment.

Placed near an entrance or shoe rack, the frame can catch your attention naturally while you are leaving. The moving sun communicates the progress of the day, while future rain and thunder indicators can act as a subtle reminder to take an umbrella.

## How it works

1. The ESP32-C3 connects to Wi-Fi and synchronizes UTC time with NTP.
2. A location string is geocoded to latitude and longitude with Open-Meteo.
3. Open-Meteo provides today's sunrise, sunset, precipitation probability and WMO weather code.
4. The firmware calculates where the sun should be between sunrise and sunset.
5. A continuous-rotation servo drives a rack-and-pinion mechanism to move the illuminated sun vertically.
6. Endpoint sensors provide absolute physical references so timing drift cannot accumulate indefinitely.
7. The ESP32 periodically recalculates the target and performs only the small movement needed.

## Mechanical system

The mechanism converts servo rotation into linear travel using a rack and pinion.

Measured physical travel between the two reference sensors is approximately **263 mm**.

Calibration from repeated full-travel tests:

| Direction | Average full travel | Effective rate |
|---|---:|---:|
| Bottom → Top | 44.604 s | ~169.6 ms/mm |
| Top → Bottom | 47.397 s | ~180.2 ms/mm |

The normal software operating range is kept about **5 mm away from each sensor**, giving roughly **253 mm** of usable travel.

Rather than running the servo extremely slowly for several hours, the controller updates the target periodically and moves in short bursts. This is simpler and more reliable for a continuous-rotation servo.

## Sensor fusion / fail-safe positioning

A continuous-rotation servo does not report its shaft position, so motor runtime alone is only an estimate.

The frame combines two sources of position information:

- **Dead reckoning:** calibrated milliseconds-per-millimetre values estimate intermediate motion.
- **Physical references:** upper and lower touch sensors provide known absolute endpoints.

At startup, the frame homes toward the lower sensor. Once detected, the position estimate is reset to the known endpoint and the carriage backs away by approximately 5 mm.

The same recovery logic is active during normal movement. If either endpoint is reached unexpectedly, the motor stops immediately, the software corrects its internal position estimate, backs away from the sensor, and can continue toward the requested target.

This makes the mechanism tolerant of small variations in servo speed, missed motion, power cycles and accumulated timing error.

## Sun position model

The visible motion is intentionally simple:

- **Before sunrise:** sun remains hidden at the bottom.
- **Sunrise → daylight midpoint:** sun moves from the bottom to the highest position.
- **Daylight midpoint → sunset:** sun moves back toward the bottom.
- **After sunset:** sun remains hidden and the LEDs are switched off.

The midpoint is calculated from that day's sunrise and sunset, so the motion automatically changes through the seasons.

## Weather data

The firmware currently requests:

- sunrise
- sunset
- maximum precipitation probability
- WMO weather code

The current code marks rain as expected above a configurable precipitation threshold and identifies thunderstorm weather codes **95, 96 and 99**.

Open-Meteo is used for both geocoding and forecast data. See the official API documentation for usage and attribution requirements.

## Future weather layer

The next version will add a physical cloud with illuminated weather indicators.

Planned behaviour:

- **Cloud / raindrops:** activate when rain probability exceeds the configured threshold.
- **Lightning:** activate when thunderstorm weather codes are forecast.
- **Optional animation:** number, brightness or animation speed of raindrops could represent precipitation probability or intensity.

The forecast data is already retrieved by the ESP32, so this extension mainly requires the additional physical lighting elements and output logic.

## Hardware

- ESP32-C3
- Continuous-rotation micro servo
- Rack-and-pinion linear mechanism
- 2 × digital touch / endpoint sensors
- 10 × WS2811 / WS2812 addressable LEDs for the sun
- 5 V power supply sized for the servo and LEDs
- Framed layered artwork / diffuser

### Current pin mapping

| Function | GPIO |
|---|---:|
| Servo signal | 10 |
| Sun LED data | 9 |
| Bottom sensor | 21 |
| Top sensor | 20 |

> Check the pin capabilities of your specific ESP32-C3 board before reproducing the wiring.

## Software dependencies

Install through the Arduino Library Manager:

- **ArduinoJson**
- **Adafruit NeoPixel**

The remaining libraries used by the sketch are included with the ESP32 Arduino core.

## Setup

1. Open `SunFrame.ino`.
2. Replace the placeholder Wi-Fi credentials.
3. Set `LOCATION_QUERY` to your city / location.
4. Confirm the GPIO assignments match your hardware.
5. Confirm your endpoint sensors output `HIGH` when triggered. If not, invert the sensor logic.
6. Upload to the ESP32-C3.
7. Open Serial Monitor at **115200 baud**.

The firmware homes the mechanism after boot, retrieves the current forecast, calculates the current target and moves the sun into position.

## Safety notes

- Power the servo from a suitable 5 V supply rather than relying on a small development-board regulator.
- Connect ESP32 and servo supply grounds together.
- Keep physical end stops in addition to software limits where possible.
- Recalibrate travel times if the mechanism, supply voltage, load or servo changes.

## Privacy / repository hygiene

The repository intentionally contains no personal Wi-Fi credentials, private addresses or unique network identifiers.

Do not commit real SSIDs or passwords. For a public repository, keep credentials as local placeholders or move them to an ignored `secrets.h` file.

## Repository

```text
Ambient-Weather-Frame/
├── README.md
└── SunFrame.ino
```

## License

Choose a license appropriate for your intended use before publishing the repository (for example MIT for permissive reuse).
