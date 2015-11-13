#ampCtl
##Control for an Hypex amplifier

The aim of this piece of software is to control a music server build around a [Wandboard](http://www.wandboard.org/) and an [Hypex UcD36](http://www.hypex.nl/docs/UcD3xMP_Datasheet.pdf) for the hardware part, [MPD](http://www.musicpd.org/) and ecasound for the software part.

###Setup Overview
This setup is enabling multi amplification : the UcD36 is coming with three separate amplifiers for bass, medium and tweeter. 
The [ecasound](http://ecasound.seul.org/ecasound/) software together with plugins computes the filtering to separate the different signals for the different drivers.

The UcD36 amplifier offers a stand by mode where it produces a 5V feed used to feed the wandboard. 
To switch on the audio part, the corresponding amplifier input as to be brought to +5V. 
On top, UcD36 offers a mute functionality which is triggered by bring an input to ground.

For implemeting these functionalities, a small board ([Wandboard](http://www.wandboard.org/) but could also be a raspberry PI) has been implemented with 
relays in order to keep things isolated between the audio and the computing part. 
Two relays are managing the two inputs for switching on the audio and muting the amplifier.

In order to control this feature, a  switch button has been implemented on the front panel. 
This button is working the following way :
  1. short press when off : switch on the audio
  1. short press when on : mute
  1. short press when on and mute : unmute
  1. long press : switch off

This is nice but needs to physically move and actionate the button. 
In order to remote control the amplifier [MPDroid](https://play.google.com/store/apps/details?id=com.namelessdev.mpdroid) has been used. MPDroid uses the regular MPD API to communicate via the network 
with the MPD server. The idea here is to trap this communication between the client and the server in order to catch the 
commands like play which will trigger the amp switch on.

Additionally, this amplifier features a volume control on the front panel which is implemented with a rotary encoder 
(which optionally combines the on/off switch). We need a bit of software to adjust the MPD volume when the volume control 
is changed.

###Software overview
So, in summary this piece of software manages events coming from :
  - A switch : on-off 
  - A volume control
  - Events coming from MPD : start, pause and stop playing
  
Those events are managed in the following way :
  - Switch on - off : start or stop mpd and the amplifier
  - Volume control : change mpd volume
  - Mpd events : start, stop or mute the amplifier
  
This program listens to 3 gpios ports (switch, vol+, vol-) and connects to mpd to gather notifications about the changes
It uses then two port to pilot the amplifier switch and the mute switch and another connection to mpd to send commands
(start, stop, pause, volume).
Another execution thread within the program listens to mpd events and treat them accordingly

There are some temporisation features protecting the amplifier : when the amplifier switches on, it first muted and unmutes after a small temporisation
Additionally when the mpd is paused or when the amplifier is muted, it switches off after few minutes (configurable)

###Configuration
ampCtl uses a [configuration file](https://github.com/PhilippeMeyer/ampCtl/blob/master/conf/ampCtl.conf) to set the Gpios ports to be used (default ampCtl.conf). It may contain the following informations:

Parameter | Description | Default 
--- | --- | ---
button |the gpio port where the switch is connected| **Required**
encoderA |the gpio port where the first encoder input is connected| **Required**
encoderB |the gpio port where the second encoder input is connected| **Required**
switch |the gpio port where the switch relay is connected (to switch on the amplifier)| **Required**
mute |the gpio port where the mute relay is connected| **Required**
pauseTimeout|the timeout before the amplifier switches off when left in mute mode|5 mn
driverProtect |the timeout before unmuting the amplifier after switch on to protect the drivers|1.5 s
gpioPath |path to the gpios in the unix user space|/sys/class/gpio
logFile |path to the log file|ampCtl.conf
mpdCmd |Command to restart mpd|service mpd restart

Additionally ampCtl supports some command switches:

Switch | Description | Default 
--- | --- | ---
--verbose (-v)|more verbose logs|Only errors are displayed
--debug (-d)|even more verbose logs|Only errors are displayed
--help (-h)|some help|
--logfile (-l)|log file to use|stdout
--config (-c)|select a specific config file|ampCtl.conf

The process should be launched as a service adding the [provided configuration file](https://github.com/PhilippeMeyer/ampCtl/blob/master/conf/ampCtlService.conf) in /etc/init

###Install
The packaging has not been done yet. Therefore after cloning this repo, build the execuatble with make and then create a directory /etc/ampCtl where ampCtl and ampCtl.conf should be copied
```shell
git clone https://github.com/PhilippeMeyer/ampCtl.git
cd ampCtl
make
sudo mkdir /etc/ampCtl
sudo cp ampCtl /etc/ampCtl
sudo cp ./conf/ampCtl.conf /etc/ampCtl
sudo cp ./conf/ampCtlService.conf /etc/init
```
###Ecasound
For configuring ecasound, please refer to the [awesome post from Richard Taylor](http://rtaylor.sites.tru.ca/2013/06/25/digital-crossovereq-with-open-source-software-howto/) -Thanks to him for this amazing contribution
