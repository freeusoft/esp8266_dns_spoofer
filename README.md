# esp8266_dns_spoofer

1. Compile with PlatformIO and Flash ESP8266 (or flash precompiled firmware.bin from releases with any available esp8266 online flasher like https://esptool.spacehuhn.com/)
2. Boot ESP8266, connect your smartphone or PC to AccessPoint: spoofer/spoofer123456
3. Open Captive Portal on http://192.168.4.1
4. Configure WiFi network and reboot ESP8266
5. Open http://spoofer.local or by assigned ip
6. Configure Upstream DNS Server (8.8.8.8 by default)
7. Configure spoofed dns records (max 10 records)
8. Configure DNS server on desired device (PC or Game console) with served DNS server by esp8266