# fpp-kfmt

RDS and audio control for the KulpLights **K-FMT** FM transmitter and other QN8027-based devices,
for [Falcon Player (FPP)](https://github.com/FalconChristmas/fpp).

Keeps the transmitter's RDS text in step with whatever FPP is playing, so listeners see the current
song on their car radio.

## Supported hardware

- KulpLights K-FMT FM transmitter.
- Other QN8027-based transmitters behind a Silicon Labs CP2112 USB-to-I2C bridge.

The plugin talks to the QN8027 tuner over I2C via the CP2112's USB HID interface, so no I2C wiring
to the host board is required.

## Features

- RDS station name, station ID, and station URL, updated as the playlist advances.
- Configurable interval for how often the station ID is shown.
- Transmit frequency and transmit power control.
- Preemphasis selection and RDS program type (North America / Europe tables).
- Audio input impedance, TX digital gain, and TX input buffer gain.
- Configurable idle behaviour for when no playlist is running — leave the carrier alone, mute, or
  disable the carrier.
- A saved transmitter state that forces the carrier on or off regardless, settable from the page or
  from an FPP command, for shutting the transmitter down outside show hours.

## Commands

The plugin registers FPP commands, so a playlist entry, a scheduled event or the REST API can
change the broadcast without opening the settings page.

- *KFMT Transmitter* — Follow Idle Setting, Force On or Force Off. Takes effect immediately whether
  or not a playlist is running, and **is saved**: Force Off keeps the transmitter off until it is set
  back, including across a restart. Use it to shut the transmitter down overnight. It sets the
  *Transmitter* setting, so the settings page always shows what is in force.
- *KFMT Station ID* — temporarily replace the RDS station ID.
- *KFMT RDS Text* — temporarily replace the RDS RadioText.

The two RDS commands are temporary: they override the configuration rather than saving over it,
running one with an empty value puts the configured text back, and a restart clears them. While one
is in force the settings page shows a note on the field it overrides, with a button to clear it.

## Installation

Install from **Content Setup → Plugins** in the FPP web UI, then restart FPPD.

## Configuration

**Input/Output Setup → KulpLights K-FMT** in the FPP web UI.

**Radio Settings**

- *Transmitter* — Follow Idle Setting (normal), Force On, or Force Off. Overrides the idle behaviour
  below; saved, so a forced state survives a restart.
- *FM Frequency* — transmit frequency.
- *Preemphasis* — preemphasis time constant.
- *Playlist Idle Behavior* — Leave Alone, Mute, or Disable Carrier when nothing is playing.
- *Transmit Power* — RF output level.
- *Audio Input Impedance*, *TX Digital Gain*, *TX Input Buffer Gain* — audio input staging.

**RDS Settings**

- *Program Type* — RDS program type code (North America or Europe table).
- *Station Code*, *Station ID* — call sign and identifier.
- *Station ID Display Time* — how often to show the station ID.
- *Station Name*, *Station URL* — additional RDS fields.

## License

GPLv3 — see [LICENSE](LICENSE).
