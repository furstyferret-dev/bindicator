# bindicator
## Overview
A bin collection notifier partly inspired by [this Twitter post](https://twitter.com/tarbard/status/1002464120447397888?lang=en) by Darren Tabard. Significant changes include collection schedule download from a Google Calendar, OLED display support, use of two NeoPixel Rings for greater brightness, and a capacitive touch sensor to cancel the reminder.

### Example video
[![Watch the video](https://img.youtube.com/vi/CMq2k3gbSQc/maxresdefault.jpg)](https://youtu.be/CMq2k3gbSQc)

## How it works
This is a project in two parts - simple Arduino code running on an ESP8266 handles the WiFi connection, drives the NeoPixel, and regularly downloads calendar events in JSON format from a Google Calendar. On first start it will launch a captive portal access point (SSID "Bindicator"), where you must supply a Google Scripts App ID for your calendar.  

Once connected any collections will cause the Bindicator to light up 24 hours before. Tapping the top of the "bin" will cancel the reminder. Multiple collections (three maximum) on the same day will show as the colour cycling. Colour is determined by the colour chosen for the event in Google Calendar. If the reminder is ignored, it will auto-cancel at 8.00am on the day of collection.

Calendar data is refreshed every 20 minutes.

[![Watch the video](https://img.youtube.com/vi/yja5Ed7apo8/maxresdefault.jpg)](https://youtu.be/yja5Ed7apo8)

## Make your own
### Parts list
1. [ESP8266 development board with OLED (any ESP8266 will work)](https://www.amazon.co.uk/gp/product/B076S8S6HL/ref=ox_sc_act_title_1?smid=A1QGN06QN25C35&psc=1) OR [Makerfire D1 Mini or clone](https://www.amazon.co.uk/Makerfire-NodeMcu-ESP8266-Internet-Development/dp/B07KYFZD17/ref=sr_1_1_sspa?crid=2KMR2ZYY6J68T)
2. [NeoPixel 7-LED ring (or generic equivalent) * 2](https://www.amazon.co.uk/gp/product/B07L82MSC9/ref=ox_sc_act_title_2?smid=A3TQ6TJY5HYALR&psc=1)
3. [Generic capacitive touch sensor](https://www.amazon.co.uk/ARCELI-2-5-5-5V-Capacitive-Self-Lock-Arduino/dp/B07BVN4CNH/)

### Arduino code
The Arduino code is plug-and-play. Further information on customising the program is available below.

> If you're using a plain ESP8266 board you should comment out the DISPLAY definition on line 18.

### Google script
Instead of trying to download and parse an entire calendar, the heavy lifting is handled by a Google Script. Go to [Google Scripts](https://script.google.com/home) and create a new project. Copy and paste the code from the GoogleScript.gs file, save it, and publish it.

**You need to save the API ID at this point**. This is the section of the URL from /macros/s through to /exec (see below in bold):
> https://script.google.com/macros/s/**AKfycbzcUsdfsdfsdltDtBHlbgde_9fXQvYMuddsvGhHFIGcSl3wr_5k**/exec

You'll need to supply this ID when you set up the Bindicator for the first time, otherwise it'll just show you my bin collection calendar.

### Soldering
Minimal soldering required. You'll need to solder wires to the NeoPixels and touch sensor. The NeoPixels support loop-in-and-out for data, saving a pin on the ESP8266. Dupont jumper wires can be used to connect to the board. I removed the plastic connector and replaced it with thin heat shrink tube to allow more flexibility when squashing everything into the base. 

NeoPixels and the button are happy on either 3.3 or 5V. I would not recommend running both from 3.3V as the on-board regulator will struggle, but the 5V side should be fine, as is a mixture of both.

Important: The D1 Mini only has one ground. Either loop through both NeoPixels and then on to the button, or solder a couple of header pins horizontally to the top of the USB port (common ground). 

### 3D print
I've modified a [fantastic model of a wheelie bin / trash can](https://www.thingiverse.com/thing:1935572) by DrLex. You'll need to download the wheels and axles from Thingiverse because I've not edited them. I've changed the bin model to include a mounting ring for the NeoPixel and a housing for the capacitive touch switch. I'd recommend printing with extra solid layers both on the sides and the top. I've used transparent PLA and white. White is best. Grey definitely doesn't work. :-)

If you use transparent PLA you may wish to print a diffuser which drops on top of the LED ring. This makes the light much easier to see in daylight, and prevents colour fringing effects from the layer lines.

I hot-glued the button and NeoPixel to the lid. 

[Here's the Thingiverse remix](https://www.thingiverse.com/thing:4147342)

[![Watch the video](https://img.youtube.com/vi/6T-8IIQMk4Q/maxresdefault.jpg)](https://youtu.be/6T-8IIQMk4Q)

## LED colours
* **Pulsing White**: Connecting to WiFi
* **Solid White**: Downloading calendar data
* **Magenta (at startup)**: WiFi portal running
* **Magenta**: Reminder cancelled
* **Red**: Unable to connect to WiFi

## Troubleshooting
**Can't connect to WiFi.**
The ESP8266 will only connect to 2.4GHz networks. If the captive portal isn't working properly, you can easily hard-code the SSID / password / Google Script ID into the Arduino program.

**Corrupted names on the OLED display.**
I think this is a memory leak from the Linked List which handles events. The reality is that I'm not sure why it's happening. If you don't like it just reboot the Bindicator.

**Colours aren't distinctive.**
I'd recommend sticking with strong primary colours (green / blue / orange etc) instead of the paler equivalents. The brightness variable may help here, as would implementing the gamma correction function available from Adafruit. If you don't like them, they're easily corrected (8 bit RGB values).

**Generic "it's not working!"**
The ESP8266 is quite particular over pins used for GPIO, especially at boot. Check the various pin-outs on Google for recommendations. The numbers silk-screened onto the board do not correspond with the Arduino IDE assignment. 

You could also uncomment the DEBUG flag in the DebugMacros.h file which will provide more information through the serial monitor.

## Improvements
Reducing the reliance on hot-glue would be a start. I wasn't aware of NeoPixel linear boards when I built this, which could be used to create some very interesting effects if mounted on the inside back wall of the model.

## Thanks
* Darren Tabard [for the inspiration](https://twitter.com/tarbard/status/1002464120447397888?lang=en).
* DrLex for the [incredible 3D model](https://www.thingiverse.com/thing:1935572).
* Adafruit for their NeoPixel Library.
* Arkhipenko for the [TaskScheduler Library](https://github.com/arkhipenko/TaskScheduler) library.
* ElectronicsGuy for his [HTTPSRedirect library](https://github.com/electronicsguy/ESP8266/tree/master/HTTPSRedirect), without which this wouldn't be possible.
* Olikraus's [u8g2 display library and fonts](https://github.com/olikraus/u8g2).
* [ArduinoJSON](https://arduinojson.org/).
