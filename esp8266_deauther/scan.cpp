/*
   Copyright (c) 2020 Stefan Kremser
   This software is licensed under the MIT License. See the license file for details.
   Source: github.com/spacehuhn/esp8266_deauther
 */

#include "scan.h"
#include "debug.h"
#include "strh.h"
#include "vendor.h"
#include "cli.h"
#include "mac.h"

#include <ESP8266WiFi.h>

extern "C" {
  #include "user_interface.h"
}

namespace scan {
    // ===== PRIVATE ===== //
    AccessPointList ap_list;
    StationList     station_list;

    void station_sniffer(uint8_t* buf, uint16_t len) {
        // drop frames that are too short to have a valid MAC header
        if (len < 28) return;

        // drop deauthentication and disassociation frames
        if ((buf[12] == 0xc0) || (buf[12] == 0xa0)) return;

        // drop beacon and probe response frames
        if ((buf[12] == 0x80) || (buf[12] == 0x50)) return;

        // only allow data frames
        // if(buf[12] != 0x08 && buf[12] != 0x88) return;

        uint8_t* mac_a = &buf[16]; // To (Receiver)
        uint8_t* mac_b = &buf[22]; // From (Transmitter)

        // drop frames with corrupted MAC addresses
        if (!mac::valid(mac_a) || !mac::valid(mac_b)) return;

        // frame from AP to station
        if (!mac::multicast(mac_a)) {
            AccessPoint* ap = ap_list.search(mac_b);
            if (ap) {
                if (station_list.registerPacket(mac_a, ap)) {
                    debug("Station ");
                    debug(strh::mac(mac_a));
                    debug(" in \"");
                    debug(ap->getSSID());
                    debugln('"');
                }
            }
        }
        // broadcast probe request from unassociated station
        else if (buf[12] == 0x40) {
            if (station_list.registerPacket(mac_b, NULL)) {
                debug("Station ");
                debugln(strh::mac(mac_b));
            }

            if (buf[12+25] > 0) {
                const char* ssid = (const char*)&buf[12+26];
                uint8_t     len  = buf[12+25];

                if ((ssid[0] != '\0') && station_list.addProbe(mac_b, ssid, len)) {
                    debug("Probe \"");

                    for (uint8_t i = 0; i<len; ++i) {
                        debug(char(ssid[i]));
                    }
                    debug("\"\n");
                }
            }
        }
    }

    // ===== PUBLIC ===== //
    void clearAPresults() {
        ap_list.clear();
    }

    void clearSTresults() {
        station_list.clear();
    }

    void search(bool ap, bool st, unsigned long time, unsigned long ch_time, uint16_t channels, bool retain) {
        { // Error check
            if (!ap && !st) {
                debugln("ERROR: Invalid scan mode");
                return;
            }

            if (st && (time <= 0)) {
                debugln("ERROR: Station scan time equals 0");
                return;
            }

            if (st && (channels == 0)) {
                debugln("ERROR: No channels specified");
                return;
            }
        }

        if (!retain) {
            if (ap) clearAPresults();
            if (st) clearSTresults();
        }

        if (ap) searchAPs();
        if (st) searchSTs(time, ch_time, channels);
    }

    void searchAPs() {
        debugln("Scanning for access points (WiFi networks)");

        WiFi.mode(WIFI_STA);
        WiFi.disconnect();

        WiFi.scanNetworks(true, true);

        int n = WiFi.scanComplete();

        for (int i = 0; i<100 && n < 0; ++i) {
            n = WiFi.scanComplete();
            delay(100);
        }

        for (int i = 0; i < n; ++i) {
            if (!ap_list.search(WiFi.BSSID(i))) {
                ap_list.push(
                    WiFi.SSID(i).c_str(),
                    WiFi.BSSID(i),
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i),
                    WiFi.channel(i)
                    );
            }
        }

        WiFi.scanDelete();

        debugln();
        printAPs();
    }

    void searchSTs(unsigned long time, unsigned long ch_time, uint16_t channels) {
        uint8_t num_of_channels = 0;

        for (uint8_t i = 0; i<14; ++i) {
            num_of_channels += ((channels >> i) & 0x01);
        }

        if (num_of_channels == 0) return;

        wifi_set_promiscuous_rx_cb(station_sniffer);
        wifi_promiscuous_enable(true);

        if (time < 1000) time = 1000;
        if (ch_time <= 0) ch_time = time/num_of_channels;

        debug("Scanning for stations (WiFi client devices) on ");
        debug(num_of_channels);
        debug(" different channels");
        debug(" in ");
        debug(time/1000);
        debugln(" seconds");

        bool running             = true;
        unsigned long start_time = millis();

        while (running) {
            for (uint8_t i = 0; i<14 && running; ++i) {
                if ((channels >> i) & 0x01) {
                    debug("Sniff channel ");
                    debug(i+1);
                    debug(" (");

                    if (ch_time < 1000) {
                        debug(ch_time);
                        debugln(" ms)");
                    } else {
                        debug(ch_time/1000);
                        debugln(" ss)");
                    }

                    wifi_set_channel(i+1);
                    unsigned long ch_start_time = millis();

                    while (running && millis() - ch_start_time < ch_time) {
                        delay(1);
                        running = !cli::read_exit() && millis() - start_time <= time;
                    }
                }
            }
        }

        wifi_promiscuous_enable(false);

        debugln();
        printSTs();
    }

    void printAPs() {
        if (ap_list.size() == 0) {
            debugln("No access points (networks) found");
        } else {
            debug("Found ");
            debug(ap_list.size());
            debugln(" access points (networks):");

            debug(strh::right(3, "ID"));
            debug(' ');
            debug(strh::left(34, "SSID (Network Name)"));
            debug(' ');
            debug(strh::right(4, "RSSI"));
            debug(' ');
            debug(strh::left(4, "Mode"));
            debug(' ');
            debug(strh::right(2, "Ch"));
            debug(' ');
            debug(strh::left(17, "BSSID (MAC Addr.)"));
            debug(' ');
            debug(strh::left(8, "Vendor"));
            debugln();

            debugln("==============================================================================");

            ap_list.begin();
            int i = 0;

            while (ap_list.available()) {
                AccessPoint* h = ap_list.iterate();

                debug(strh::right(3, String(i)));
                debug(' ');
                debug(strh::left(34, h->getSSIDString()));
                debug(' ');
                debug(strh::right(4, String(h->getRSSI())));
                debug(' ');
                debug(strh::left(4, h->getEncryption()));
                debug(' ');
                debug(strh::right(2, String(h->getChannel())));
                debug(' ');
                debug(strh::left(17, h->getBSSIDString()));
                debug(' ');
                debug(strh::left(8, h->getVendor()));
                debugln();

                ++i;
            }

            debugln("================================================================================");
            debugln("Ch   = 2.4 GHz Channel");
            debugln("RSSI = Signal strengh");
            debugln("WPA* = WPA & WPA2 auto mode");
            debugln("WPA(2) Enterprise networks are recognized as Open");
            debugln("================================================================================");
        }
    }

    void printSTs() {
        if (station_list.size() == 0) {
            debugln("No stations (clients) found");
        } else {
            debug("Found ");
            debug(station_list.size());
            debugln(" stations (clients):");

            debug(strh::right(3, "ID"));
            debug(' ');
            debug(strh::left(17, "MAC-Address"));
            debug(' ');
            debug(strh::right(4, "Pkts"));
            debug(' ');
            debug(strh::left(8, "Vendor"));
            debug(' ');
            debug(strh::left(34, "AccessPoint-SSID"));
            debug(' ');
            debug(strh::left(17, "AccessPoint-BSSID"));
            debug(' ');
            debug(strh::left(34, "Probe-Requests"));
            debugln();

            debugln("===========================================================================================================================");

            int i = 0;
            station_list.begin();

            while (station_list.available()) {
                Station* h = station_list.iterate();

                debug(strh::right(3, String(i)));
                debug(' ');
                debug(h->getMACString());
                debug(' ');
                debug(strh::right(4, String(h->getPackets())));
                debug(' ');
                debug(strh::left(8, h->getVendor()));
                debug(' ');
                debug(strh::left(34, h->getSSIDString()));
                debug(' ');
                debug(strh::left(17, h->getBSSIDString()));
                debug(' ');

                h->getProbes().begin();
                bool first = true;

                while (h->getProbes().available()) {
                    if (!first) {
                        debug("\n                                                                                         ");
                    }
                    debug(/*strh::left(32, */ '"' + h->getProbes().iterate() + '"');
                    first = false;
                }

                debugln();
                ++i;
            }

            debugln("===========================================================================================================================");
            debugln("Pkts = Recorded Packets");
            debugln("===========================================================================================================================");
        }
    }

    void printResults() {
        printAPs();
        printSTs();
    }

    AccessPointList& getAccessPoints() {
        return ap_list;
    }

    StationList& getStations() {
        return station_list;
    }
}