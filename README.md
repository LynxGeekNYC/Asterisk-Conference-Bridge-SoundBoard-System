# Asterisk Conference Bridge SoundBoard System
Asterisk Conference Bridge Sound Board system in C++ that injects audio into conference bridges through AMI

Asterisk ConfBridge Soundboard

Asterisk ConfBridge Soundboard is a Linux-native, operator-grade soundboard utility for Asterisk ConfBridge conferences. It allows administrators and operators to inject audio into live conference bridges using the Asterisk Manager Interface (AMI), with automatic discovery of active rooms, a browsable sound library, favorites, recents, and batch playback.

The project is designed to be easy to install, safe to operate, and portable across Asterisk versions, with intelligent feature detection and optional fallback mechanisms.

Key Features
Conference Control

Enumerates active ConfBridge rooms in real time

Displays room metadata (participants, marked users, lock and mute state)

Lists participants per room with caller ID and channel information

Sound Playback

Injects audio directly into a live ConfBridge

Supports WAV, ULAW, GSM, ALAW, SLN, SLN16

Automatically resolves filesystem paths to Asterisk sound names

Works with multiple sound root directories

Sound Library Management

Recursive indexing of sound directories

Fast text search

Favorites and recent sounds

Exact name selection for power users

Batch playback with configurable delays

Intelligent Runtime Detection

Detects whether ConfbridgePlaySound is supported by the running Asterisk build

Warns clearly when unsupported instead of failing silently

Designed to support future fallback injection methods

Installation & Operations

One-command installer script

Idempotent AMI configuration

Secure defaults for permissions and ownership

Clean separation between configuration, state, and binaries

Architecture Overview
┌─────────────────────────┐
│  Operator / Admin       │
│  (Terminal UI)          │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│  Soundboard Binary      │
│  (C++, Linux-native)    │
└───────────┬─────────────┘
            │ AMI (TCP 5038)
            ▼
┌─────────────────────────┐
│  Asterisk Manager       │
│  Interface (AMI)        │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│  ConfBridge Module      │
│  (Live Conferences)    │
└─────────────────────────┘

Requirements

Linux (x86_64)

Asterisk with ConfBridge enabled

AMI enabled and reachable (default TCP 5038)

C++17 compatible compiler (for building)

Root access for installation

Installation
Build
g++ -O2 -std=c++17 -Wall -Wextra \
  -o asterisk-soundboard \
  asterisk_soundboard_adv.cpp

Install
sudo ./install.sh


The installer will:

Install the binary to /usr/local/bin/asterisk-soundboard

Create /etc/asterisk/asterisk-soundboard.conf

Create sound and state directories

Add an AMI user block to manager.conf

Reload AMI

Validate basic ConfBridge functionality

Configuration

Default configuration file:

/etc/asterisk/asterisk-soundboard.conf


Example:

host=127.0.0.1
port=5038
username=soundboard
secret=StrongSecretHere

sound_roots=/var/lib/asterisk/sounds,/usr/share/asterisk/sounds
extensions=wav,ulaw,gsm,alaw,sln,sln16
favorites=custom/airhorn,custom/applause

Adding Sounds

Copy sound files into:

/var/lib/asterisk/sounds/custom/soundboard/


Ensure proper format (recommended):

8 kHz

Mono

Reference sounds by Asterisk sound name, not file path:

custom/soundboard/airhorn

Usage
asterisk-soundboard


Interactive features:

Select active conference

Browse or search sounds

Play instantly

Batch play sequences

Quick-play favorites

Review recent sounds

Security Considerations

Uses least-privilege AMI credentials

Configuration file is restricted to root:asterisk

No network listeners exposed by default

No modification of dialplan unless explicitly enabled

Designed to run interactively or via controlled automation

Operational Notes

Conference must be active to receive injected audio

Playback is non-blocking and does not join as a speaking participant

Sound names must resolve in Asterisk’s sound path

Feature availability depends on Asterisk build and modules

Compatibility

Tested against:

Asterisk 16, 18, 20

PJSIP-only systems

Localhost and remote AMI connections

Designed to degrade gracefully on older builds.

Roadmap

ncurses full-screen UI

Web UI (local-only)

Dialplan-based fallback injection

systemd service mode

RPM and DEB packaging

Multi-conference broadcast mode

Operator role separation

License

MIT License

Disclaimer

This software is intended for administrative and operational use in environments where you are authorized to control conference audio. Always comply with applicable laws and organizational policies regarding call monitoring and audio injection.
