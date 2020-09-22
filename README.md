# PressForMusic

ESP8266 code to build a "Press For Music" sign for use in a Christmas Light Display.

## Background

What in the world is this?  So, there are people out there who are REALLY obsessed with Christmas light shows and put up big displays with lots of individually controllable lights synchronized to music.  I'm one of these people.

This project is intended to work with [Falcon Pi Player](https://github.com/FalconChristmas/fpp), which is a sequence player designed to work with the xLights sequencing software and various light controller modules.  

Most light displays with music rely primarily on an FM transmitter so that people driving by can tune in and hear the music for a display.  This project takes that a step further and builds in an ability for people walking by the light show to press a button (or in my case step on a pedal) to activate outdoor speakers.

So why not just use a simple timer relay?  I've done that in the past, and it worked great.  But as is characteristic of people who do these kinds of light shows there is always something to tweak.  So in that spirit I build out this little ESP8266 module.  What it does is listen to FPP's MQTT topics to determine:
1. When music is playing.
2. When songs transition.

When the show viewer activates the speakers (by pressing a button or stepping on a pedal) the ESP8226 turns on the relay to play music on the outdoor speakers.  The code activates the speaker for the remainder of the currently playing song and the next song in the playlist.  When that completes, the speakers are shut off.

See my [video on YouTube](https://youtu.be/F76B2gxWJc4) for a visual demo on how this all works.

## Wiring

![wiring diagram](https://github.com/tygunn/PressForMusic/blob/master/img/ESP8266Wiring.png?raw=true)

### Parts List

* [3V optoisolated relay](https://www.amazon.com/gp/product/B07ZM84BVX)
* [Foot Switch](https://www.amazon.com/gp/product/B00EF9D2DY)
* [ESP8266](https://www.amazon.com/gp/product/B081CSJV2V)
* 1K resistor
* 10k resistor
* [DC powered amplifier](https://www.amazon.com/Lepy-LP-2020A-Class-D-Digital-Amplifier/dp/B01FZKA28Y/ref=sr_1_5?dchild=1&keywords=lepai+amplifier&qid=1600754764&sr=8-5)

### Hooking up the relay

As illustrated in the diagram, connect the "VCC" terminal of the relay board to one of the 3V3 pins on the ESP8266.  Connect the GND on the relay board to one of the GND pins on the ESP8266.  Finally, connect pin D1 to IN on the relay board.

### Hooking up the switch

Connecting the switch is not quite as simple as it would seem.  You need to build a simple "pull down" circuit with a couple resistors.  Basically, the 10k resistor allows the D5 pin to see the ground reference when the switch is open.  However, when the switch is closed, the 3V power source from the ESP8266 is allowed to flow into the D5 pin.  The 1K resistor is primarily to protect the D5 pin from being accidentally overloaded if the pins are misconfigured.

Inside the foot switch you want to hook up the two wires to the COM and NO terminals.  COM is "common", and NO means "normally open".  Choosing "normally open" means that when the foot switch is not being pressed that the connection between COM and NO is open.  You don't need to worry about which wire goes to COM or NO; they're interchangeable.

### Hooking up the amplifier

I simply cut the wires for amplifier's 12v power supply.  Hook the red (+) wire to the "COM" terminal on the relay and hook the NO terminal on the relay to the amplifier (i.e. since you cut the cable on the amplifier, hook it up to the red wire in the part that plugs into the amp).  The black wire can just be soldered back together.

### Powering the ESP8266

The ESP8266 has a mini-usb port on it; that's the easiest way to power the ESP8266.

## Compiling the code

To compile the code, you will need to install a few dependencies into the Arduino IDE.  Open the downlaoded PressForMusic source file in the Arduino IDE.

Download [ESPAsyncTCP](https://github.com/me-no-dev/ESPAsyncTCP/archive/master.zip).  From the Arduino IDE choose: Sketch > Include Library > Add ZIP Library.  Choose the downloaded ESPAsyncTCP zip file you just downloaded.

First, download [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client/releases/latest).  From the Arduino IDE choose: Sketch > Include Library > Add ZIP Library.  Choose the downloaded AsyncMqttClient zip file you just downloaded.

Next, in the source code, replace WIFI_SSID and WIFI_PASSWORD with your credentials.  Also change MQTT_SERVER_IP to the IP address of the MQTT server on your network.  If you don't have one set up already, this is a good use for another Raspberry PI.  I have another PI on my network and followed [instructions like these](https://appcodelabs.com/introduction-to-iot-build-an-mqtt-server-using-raspberry-pi) to install mosquitto, an MQTT server on the PI.  

## Setting up FPP

On your FPP go to Status/Control > FPP Settings.

Switch to the MQTT tab.

Enter the IP address of your Mosquitto server on your raspberry pi.  In my case it's 192.168.0.125.
For the topic prefix use christmas (it is case sensitive).


