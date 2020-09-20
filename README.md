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


