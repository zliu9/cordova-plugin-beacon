/*
 * Copyright (c) 2015 BlackBerry Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <json/reader.h>
#include <json/writer.h>
#include "simplexpbeaconplugin_ndk.hpp"
#include "simplexpbeaconplugin_js.hpp"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

static webworks::SimpleXpBeaconPlugin_NDK *s_btController = NULL;

namespace webworks {

/*
 * Don't use deprecated API if we're building for >= 10.3.0
 */
#if BBNDK_VERSION_AT_LEAST(10,3,0)
void btLeAdvertisementCbExt(const char *bdaddr, int8_t rssi, bt_le_advert_packet_event_t eventType, const char *data, int len, void *userData)
{
    if (s_btController) {
        s_btController->parseBeaconData(data, len, rssi, bdaddr);
    }
}
#else
void btLeAdvertisementCb(const char *bdaddr, int8_t rssi, const char *data, int len, void *userData)
{
    if (s_btController) {
        s_btController->parseBeaconData(data, len, rssi, bdaddr);
    }
}
#endif

void btEvent(int event, const char *btAddr, const char *eventData)
{
    LOGD("btEvent - event=%s, btAddr=%s, event_data=%s", Util::btEventName(event), btAddr, eventData);
}

/*
 * Don't use deprecated API if we're building for >= 10.3.0
 */
#if BBNDK_VERSION_AT_LEAST(10,3,0)
bt_le_callbacks_t leCallbacks = { NULL, NULL, NULL, btLeAdvertisementCbExt };
#else
bt_le_callbacks_t leCallbacks = { btLeAdvertisementCb, NULL, NULL };
#endif

SimpleXpBeaconPlugin_NDK::SimpleXpBeaconPlugin_NDK(SimpleXpBeaconPlugin_JS *parent)
    : m_pParent(parent)
    , m_bt_initialised(false)
    , m_monitoring(false)
    , m_monitoring_callback_id("")
{
    s_btController = this;
    m_beacon_regions_to_monitor.clear();
}

SimpleXpBeaconPlugin_NDK::~SimpleXpBeaconPlugin_NDK()
{
    LOGI("SimpleXpBeaconPlugin_NDK destructor");

    m_monitoring = false;
    m_monitoring_callback_id = "";
    m_bt_initialised = false;
    bt_device_deinit();
    s_btController = NULL;
    m_beacon_regions_to_monitor.clear();
}

// These methods are the true native code we intend to reach from WebWorks

std::string SimpleXpBeaconPlugin_NDK::initialiseBluetooth(const std::string& callbackId)
{
    LOGD("initialiseBluetooth");

    Json::FastWriter writer;
    Json::Value root;
    char temp[500];

    if (m_bt_initialised) {
        LOGD("Bluetooth Already Initialised");
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = "Bluetooth Already Initialised";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    int rc = bt_device_init(btEvent);

    if (rc == EOK) {
        LOGD("Bluetooth bt_device_init() OK");
    } else {
        snprintf(temp, sizeof(temp), "Bluetooth bt_device_init() error - rc=%d, error=%s", rc, strerror(errno));
        LOGD("%s", temp);
        m_bt_initialised = false;
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = temp;
        root[JSON_KEY_ERROR_CODE] = (int)errno;
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    // START *** TEMP workaround ***
    //
    #if BBNDK_VERSION_AT_LEAST(10,3,0)
        bt_ldev_set_filters(BT_EVT_ALL_EVENT, true); // COREOS-96977 / 825695 -- workaround for this defect in 10.3.1E
    #endif
    //
    // END  *** TEMP workaround ***

    if (!bt_ldev_get_power()) {
        LOGD("Bluetooth calling bt_ldev_set_power()");
        bt_ldev_set_power(true);
    }

    rc = bt_le_init(&leCallbacks);
    if (rc == EOK) {
        LOGD("Bluetooth bt_le_init() OK");
    } else {
        snprintf(temp, sizeof(temp), "Bluetooth bt_le_init() error - rc=%d, error=%s", rc, strerror(errno));
        LOGD("%s", temp);
        m_bt_initialised = false;
        bt_device_deinit();
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = temp;
        root[JSON_KEY_ERROR_CODE] = (int)errno;
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    bt_le_set_scan_params(1000, 100, BT_LE_ADVERT_SCAN_PASSIVE);

    m_bt_initialised = true;

    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "Bluetooth Initialised";
    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::terminateBluetooth(const std::string& callbackId)
{
    LOGD("terminateBluetooth");

    Json::FastWriter writer;
    Json::Value root;

    m_bt_initialised = false;

    bt_le_deinit();
    bt_device_deinit();

    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "Bluetooth Terminated";

    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::pluginVersion(const std::string& callbackId)
{
    LOGD("pluginVersion");

    Json::FastWriter writer;
    Json::Value root;
    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "Plugin Version";
    root[JSON_KEY_PLUGIN_VERSION] = PLUGIN_VERSION;

    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::startMonitoring(const std::string& callbackId)
{
    LOGD("startMonitoring");

    Json::FastWriter writer;
    Json::Value root;
    char temp[500];
    int rc = 0;

    if (!m_bt_initialised) {
        LOGD("Bluetooth Not initialised");
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_EVENT] = JSON_VALUE_STARTED;
        root[JSON_KEY_DESCRIPTION] = "Bluetooth Not initialised";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    if (m_monitoring) {
        LOGD("Already monitoring for iBeacons");
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_EVENT] = JSON_VALUE_STARTED;
        root[JSON_KEY_DESCRIPTION] = "Already monitoring for iBeacons";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    m_monitoring = true;
    m_monitoring_callback_id = callbackId;

    bt_le_add_scan_device(BT_LE_BDADDR_ANY, NULL);

    LOGD("Requested iBeacon Monitoring OK");
    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "Requested iBeacon Monitoring OK";
    root[JSON_KEY_EVENT] = JSON_VALUE_STARTED;
    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::stopMonitoring(const std::string& callbackId)
{
    LOGD("In stopMonitoring()");

    Json::FastWriter writer;
    Json::Value root;
    char temp[500];
    int rc = 0;

    if (!m_bt_initialised) {
        LOGD("Bluetooth Not initialised");
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = "Bluetooth Not initialised";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    if (!m_monitoring) {
        LOGD("Not monitoring for iBeacons");
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = "Not monitoring for iBeacons";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    bt_le_remove_scan_device(BT_LE_BDADDR_ANY);

    LOGD("Stopped Monitoring for iBeacons OK");
    m_monitoring = false;
    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "Stopped Monitoring for iBeacons OK";
    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::addBeaconUuidToMonitor(const std::string& callbackId, const std::string& uuid)
{
    LOGD("In addBeaconUuidToMonitor()");

    Json::FastWriter writer;
    Json::Value root;
    char temp[500];
    int rc = 0;

    LOGD("In addBeaconUuidToMonitor() - UUID = %s", uuid.c_str());

    std::list<std::string>::iterator iter = std::find (m_beacon_regions_to_monitor.begin(), m_beacon_regions_to_monitor.end(), toUpper(uuid));
    if (iter != m_beacon_regions_to_monitor.end()) {
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = "iBeacon already being monitored";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    m_beacon_regions_to_monitor.push_back(toUpper(uuid));

    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "UUID added";
    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

std::string SimpleXpBeaconPlugin_NDK::removeBeaconUuidToMonitor(const std::string& callbackId, const std::string& uuid)
{
    LOGD("In removeBeaconUuidToMonitor()");

    Json::FastWriter writer;
    Json::Value root;
    char temp[500];
    int rc = 0;

    LOGD("In removeBeaconUuidToMonitor() - UUID = %s", uuid.c_str());

    std::list<std::string>::iterator iter = std::find (m_beacon_regions_to_monitor.begin(), m_beacon_regions_to_monitor.end(), toUpper(uuid));
    if (iter == m_beacon_regions_to_monitor.end()) {
        root[JSON_KEY_STATUS] = JSON_VALUE_ERROR;
        root[JSON_KEY_DESCRIPTION] = "iBeacon UUID did not match any being monitored";
        RETURN_VIA_CALLBACK(callbackId, writer.write(root));
    }

    m_beacon_regions_to_monitor.remove(toUpper(uuid));

    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_DESCRIPTION] = "iBeacon UUID removed successfully";
    RETURN_VIA_CALLBACK(callbackId, writer.write(root));
}

void SimpleXpBeaconPlugin_NDK::parseBeaconData(const char *data, int len, int8_t rssi, const char *bdaddr)
{
    bool hasIBeaconData = false;
    uint16_t beaconMajor = 0;
    uint16_t beaconMinor = 0;
    int8_t calibratedStrength = 0;
    char beaconUuid[16];

    Json::FastWriter writer;
    Json::Value root;
    Json::Value ib;

    if (!m_monitoring) {
        return;
    }

    if (len < 27) { // quick sanity check 27 is minimal length of iBeacon advertisment
        return;
    }

    root[JSON_KEY_SOURCE] = JSON_VALUE_THREAD;
    root[JSON_KEY_STATUS] = JSON_VALUE_OK;
    root[JSON_KEY_EVENT] = JSON_VALUE_IBEACON;
    root[JSON_KEY_DESCRIPTION] = "iBeacon event";

    int i = 0;
    do {
        uint8_t entryLen = data[i];
        uint8_t entryType = data[i+1];

        if (entryType == 0xff) {
            int j = i+2;
            if (((uint8_t)data[j] == 0x4c) &&
                ((uint8_t)data[j+1] == 0x00) &&
                ((uint8_t)data[j+2] == 0x02) &&
                ((uint8_t)data[j+3] == 0x15)) {

                hasIBeaconData = true;
                for (size_t k=0; k<sizeof(beaconUuid); k++) {
                    beaconUuid[k] = data[k+j+4];
                }
                j += (sizeof(beaconUuid) + 4);
                beaconMajor = 0;
                beaconMajor = (uint16_t)((uint8_t)data[j+1] & 0xff);
                beaconMajor += (uint16_t)(((uint8_t)data[j] & 0xff) << 8);
                j += 2;
                beaconMinor = 0;
                beaconMinor = (uint16_t)((uint8_t)data[j+1] & 0xff);
                beaconMinor += (uint16_t)(((uint8_t)data[j] & 0xff) << 8);
                j += 2;
                calibratedStrength = 0;
                calibratedStrength = (int8_t) (data[j] & 0xff);
            }
        }
        i += (entryLen+1);
    } while (i < len);

    if (!hasIBeaconData) {
        return;
    }

    //8AEFB031-6C32-486F-825B-E26FA193487D

    char temp[2*sizeof(beaconUuid)+1+4];
    char *o = temp;
    for (size_t i=0; i< sizeof(beaconUuid); i++) {
        o += snprintf(o, sizeof(temp)-(o-temp), "%02X", (uint8_t)(beaconUuid[i] & 0x0ff));
        if (i == 3)
            o += snprintf(o, sizeof(temp)-(o-temp), "-");
        if (i == 5)
            o += snprintf(o, sizeof(temp)-(o-temp), "-");
        if (i == 7)
            o += snprintf(o, sizeof(temp)-(o-temp), "-");
        if (i == 9)
            o += snprintf(o, sizeof(temp)-(o-temp), "-");
    }

    ib[JSON_KEY_IBEACON_UUID] = temp;
    ib[JSON_KEY_IBEACON_MAJOR] = beaconMajor;
    ib[JSON_KEY_IBEACON_MINOR] = beaconMinor;
    ib[JSON_KEY_IBEACON_RSSI] = rssi;
    ib[JSON_KEY_IBEACON_TXPOWER] = calibratedStrength;

    root[JSON_KEY_DATA] = ib;

    if (m_beacon_regions_to_monitor.empty()) {
        m_pParent->NotifyEvent(m_monitoring_callback_id + " " + writer.write(root));
    } else {
        std::list<std::string>::iterator iter = std::find (m_beacon_regions_to_monitor.begin(), m_beacon_regions_to_monitor.end(), temp);
        if (iter != m_beacon_regions_to_monitor.end()) {
            m_pParent->NotifyEvent(m_monitoring_callback_id + " " + writer.write(root));
        }
    }
}

std::string SimpleXpBeaconPlugin_NDK::toUpper(std::string sourceString)
{
    std::transform(sourceString.begin(), sourceString.end(), sourceString.begin(), std::toupper);

    return sourceString;
}

} /* namespace webworks */
