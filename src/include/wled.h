/*
 *
 *    Copyright (c) 2023 Zack Elia
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

#include "Device.h"
#include "color-utils.h"

#define BIT_SET(n, x) (((n & (1 << x)) != 0) ? 1 : 0)
#define SUPPORTS_RGB(x) BIT_SET(x, 0)
#define SUPPORTS_WHITE_CHANNEL(x) BIT_SET(x, 1)
#define SUPPORTS_COLOR_TEMPERATURE(x) BIT_SET(x, 2)

class WLED : public DeviceExtendedColor
{
public:
    WLED(std::string_view ip, std::string szLocation)
    noexcept : DeviceExtendedColor(("WLED " + std::string(ip)).c_str(), szLocation)
    {
        websocket_addr = [&]() {
            std::string temp = "ws://";
            temp.append(ip);
            temp.append("/ws");
            return temp;
        }();

        if (connect())
        {
            std::cerr << "Could not setup websocket connection" << std::endl;
            reconnect_thread = std::jthread([=] { this->reconnect(); });
            return;
        }
        wait();
        recv();
    }

    // No copying!
    WLED(const WLED &)             = delete;
    WLED & operator=(const WLED &) = delete;

    // TODO: Add these back
    // WLED(WLED && other) noexcept
    // {
    //     this->curl      = std::exchange(other.curl, nullptr);
    //     this->led_state = std::exchange(other.led_state, { 0 });
    // }

    // WLED & operator=(WLED && other) noexcept
    // {
    //     std::swap(this->curl, other.curl);
    //     std::swap(this->led_state, other.led_state);
    //     return *this;
    // }

    virtual ~WLED() noexcept
    {
        // TODO: (void)curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
        curl_easy_cleanup(curl);
    }

    int socket() const noexcept
    {
        int sockfd   = -1;
        CURLcode res = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_getinfo(CURLINFO_ACTIVESOCKET): " << curl_easy_strerror(res) << std::endl;
            abort();
            return -1;
        }
        return sockfd;
    }

    void update() noexcept
    {
        recv();
        // TODO: Handle this a little more elegantly
        if (led_info.name.c_str())
            Device::SetName(led_info.name.c_str());
        DeviceOnOff::SetOnOff(led_state.on);
        DeviceDimmable::SetLevel(led_state.brightness);
        DeviceColorTemperature::SetMireds(cct_to_mireds(led_state.cct));
        DeviceExtendedColor::SetHue(led_state.hsv.h);
        DeviceExtendedColor::SetSaturation(led_state.hsv.s);
    }

    inline std::string GetManufacturer() override { return led_info.manufacturer; }
    inline std::string GetSerialNumber() override { return led_info.serial_number; }
    inline std::string GetModel() override { return led_info.model; }

    void SetReachable(bool reachable) override
    {
        if (!reachable && curl)
        {
            curl_easy_cleanup(curl);
            curl = 0;
        }
        Device::SetReachable(reachable);
    }

    bool IsOn() override { return on(); }
    void SetOnOff(bool aOn) override
    {
        set_on(aOn);
        DeviceOnOff::SetOnOff(aOn);
    }

    uint8_t Level() override { return brightness(); }
    void SetLevel(uint8_t aLevel) override
    {
        set_brightness(aLevel);
        DeviceDimmable::SetLevel(aLevel);
    }

    uint16_t Capabilities() override
    {
        int caps = 0;
        if (SUPPORTS_RGB(led_state.capabilities))
        {
            caps += static_cast<int>(chip::app::Clusters::ColorControl::ColorCapabilities::kHueSaturationSupported);
        }
        if (SUPPORTS_COLOR_TEMPERATURE(led_state.capabilities))
        {
            caps += static_cast<int>(chip::app::Clusters::ColorControl::ColorCapabilities::kColorTemperatureSupported);
        }
        return static_cast<uint16_t>(caps);
    }

    uint16_t Mireds() override { return cct_to_mireds(led_state.cct); }

    void SetMireds(uint16_t aMireds) override
    {
        set_cct(mireds_to_cct(aMireds));
        DeviceColorTemperature::SetMireds(aMireds);
    }

    uint8_t Hue() override { return led_state.hsv.h; }
    void SetHue(uint8_t aHue) override
    {
        set_hue(aHue);
        DeviceExtendedColor::SetHue(aHue);
    }

    uint8_t Saturation() override { return led_state.hsv.s; }
    void SetSaturation(uint8_t aSaturation) override
    {
        set_saturation(aSaturation);
        DeviceExtendedColor::SetSaturation(aSaturation);
    }

private:
    [[nodiscard]] uint8_t brightness() const noexcept { return led_state.brightness; }

    [[nodiscard]] bool on() const noexcept { return led_state.on; }

    void set_brightness(uint8_t brightness) noexcept
    {
        // Matter max level is 254, WLED is 255
        brightness = std::min(brightness, static_cast<uint8_t>(254));
        Json::Value root;
        // Matter sets brightness after setting a light to off. WLED will interpret this as turning the light back on which is
        // unintended. Send the `on` state at the same time to prevent this.
        root["on"]  = on();
        root["bri"] = brightness;
        send(writer.write(root));
        led_state.brightness = brightness;
    }

    void set_on(bool on) noexcept
    {
        Json::Value root;
        root["on"] = on;
        root["tt"] = 1;
        send(writer.write(root));
        led_state.on = on;
    }

    void set_hue(uint8_t hue) noexcept
    {
        led_state.hsv.h = hue;
        led_state.hsv.v = led_state.brightness;
        led_state.rgb   = HsvToRgb(led_state.hsv);
        Json::Value root;
        // Matter sets brightness after setting a light to off. WLED will interpret this as turning the light back on which is
        // unintended. Send the `on` state at the same time to prevent this.
        root["on"] = on();
        for (int i = 0; i < 3; i++)
            root["seg"]["col"].append(Json::arrayValue);
        root["seg"]["col"][0].insert(0, led_state.rgb.r);
        root["seg"]["col"][0].insert(1, led_state.rgb.g);
        root["seg"]["col"][0].insert(2, led_state.rgb.b);
        send(writer.write(root));
    }

    void set_saturation(uint8_t saturation) noexcept
    {
        led_state.hsv.s = saturation;
        led_state.hsv.v = led_state.brightness;
        led_state.rgb   = HsvToRgb(led_state.hsv);
        Json::Value root;
        // Matter sets brightness after setting a light to off. WLED will interpret this as turning the light back on which is
        // unintended. Send the `on` state at the same time to prevent this.
        root["on"] = on();
        for (int i = 0; i < 3; i++)
            root["seg"]["col"].append(Json::arrayValue);
        root["seg"]["col"][0].insert(0, led_state.rgb.r);
        root["seg"]["col"][0].insert(1, led_state.rgb.g);
        root["seg"]["col"][0].insert(2, led_state.rgb.b);
        send(writer.write(root));
    }

    void set_cct(uint8_t cct) noexcept
    {
        Json::Value root;
        // Matter sets brightness after setting a light to off. WLED will interpret this as turning the light back on which is
        // unintended. Send the `on` state at the same time to prevent this.
        root["on"]         = on();
        root["seg"]["cct"] = cct;
        send(writer.write(root));
        led_state.cct = cct;
    }

    int connect()
    {
        curl = curl_easy_init();
        if (!curl)
        {
            std::cerr << "curl_easy_init: failed" << std::endl;
            return -1;
        }

        curl_easy_setopt(curl, CURLOPT_URL, websocket_addr.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_perform: " << curl_easy_strerror(res) << std::endl;
            return -1;
        }

        SetReachable(true);

        return 0;
    }

    // Exponential backoff (max 5 minutes) until we can reconnect
    void reconnect()
    {
        constexpr int FIVE_MINUTES = 5 * 60;
        int sleep_seconds          = 5;

        // When connecting to the websocket immediately on boot, it appears to connect fine but the first call to recv fails.
        // Sleeping instead of immediately reconnecting seems to prevent this issue.
        std::this_thread::sleep_for(std::chrono::seconds(sleep_seconds));

        while (true)
        {
            if (!connect())
            {
                ChipLogProgress(DeviceLayer, "[%s] Reconnected!", GetName());
                wait();
                recv();
                // Alert the main thread to listen for this socket now
                extern int wled_monitor_pipe[2];
                char buf[1] = { 1 };
                write(wled_monitor_pipe[1], buf, 1);
                return;
            }

            sleep_seconds = std::min(sleep_seconds * 2, FIVE_MINUTES);
            ChipLogError(DeviceLayer, "[%s] Could not reconnect, trying again in %d seconds...", GetName(), sleep_seconds);
            std::this_thread::sleep_for(std::chrono::seconds(sleep_seconds));
        }
    }

    void close()
    {
        if (!curl)
            return;

        curl_easy_cleanup(curl);
        curl = nullptr;
    }

    int recv(bool is_response = false) noexcept
    {
        size_t rlen;
        const struct curl_ws_frame * meta;
        char buffer[MAX_WEBSOCKET_BYTES];
        CURLcode result = curl_ws_recv(curl, buffer, sizeof(buffer), &rlen, &meta);
        if (result == CURLE_GOT_NOTHING)
        {
            ChipLogProgress(DeviceLayer, "Got nothing from websocket, assuming disconnected");
            SetReachable(false);
            abort();
            return -1;
        }
        if (meta && meta->flags & CURLWS_CLOSE)
        {
            ChipLogProgress(DeviceLayer, "Websocket was closed unexpectedly");
            SetReachable(false);
            reconnect_thread = std::jthread([=] { this->reconnect(); });
            return -1;
        }
        if (result == CURLE_AGAIN)
        {
            // Multithreaded programming is hard, we probably already read the intended message
            return 0;
        }
        if (result != CURLE_OK)
        {
            std::cerr << "curl_ws_recv: " << curl_easy_strerror(result) << std::endl;
            abort();
        }

        if (is_response)
        {
            return 0;
        }

        Json::Value root;
        if (reader.parse(buffer, root) == false)
        {
            std::cerr << "reader.parse: failed to parse" << std::endl;
            abort();
            return -1;
        }

        led_state.on = root["state"]["on"].asBool();
        // Matter max level is 254, WLED is 255
        led_state.brightness   = static_cast<uint8_t>(root["state"]["bri"].asUInt());
        led_state.brightness   = std::min(led_state.brightness, static_cast<uint8_t>(254));
        led_state.capabilities = root["info"]["leds"]["lc"].asInt();

        led_info.name          = root["info"]["name"].asString();
        led_info.serial_number = root["info"]["mac"].asString();
        led_info.model         = root["info"]["arch"].asString() + " v" + root["info"]["ver"].asString();

        if (strncmp(mName, led_info.name.c_str(), sizeof(mName)) != 0)
        {
            SetName(led_info.name.c_str());
        }

        // std::cout << "RGB Support: " << SUPPORTS_RGB(led_state.capabilities) << std::endl;
        // std::cout << "White Support: " << SUPPORTS_WHITE_CHANNEL(led_state.capabilities) << std::endl;
        // std::cout << "Color temp Support: " << SUPPORTS_COLOR_TEMPERATURE(led_state.capabilities) << std::endl;

        if (SUPPORTS_RGB(led_state.capabilities))
        {
            auto colors  = root["state"]["seg"][0]["col"];
            auto primary = colors[0];

            led_state.rgb.r = static_cast<uint8_t>(primary[0].asInt());
            led_state.rgb.g = static_cast<uint8_t>(primary[1].asInt());
            led_state.rgb.b = static_cast<uint8_t>(primary[2].asInt());

            led_state.hsv = RgbToHsv(led_state.rgb);
        }

        if (SUPPORTS_COLOR_TEMPERATURE(led_state.capabilities))
        {
            uint16_t cct = static_cast<uint16_t>(root["state"]["seg"][0]["cct"].asUInt());
            if (cct >= 1900 && cct <= 10091) // Kelvin instead of relative, need to convert
            {
                // TODO: Does this ever actually happen?
                cct = static_cast<uint16_t>(255 * (cct - 1900) / (10091 - 1900));
            }
            // cct is appropriately sized now
            led_state.cct = static_cast<uint8_t>(cct);
        }

        return 0;
    }

    // TODO: Probably rename to send_and_recv or create a separate function
    int send(std::string data) noexcept
    {
        std::lock_guard lock(mutex);

        size_t sent;
        CURLcode result = curl_ws_send(curl, data.c_str(), strlen(data.c_str()), &sent, 0, CURLWS_TEXT);
        if (result != CURLE_OK)
        {
            std::cerr << "curl_ws_send: " << curl_easy_strerror(result) << std::endl;
            abort();
        }

        // TODO: I think there is a race condition here if an update comes before the response is received e.g.
        // -> we send message
        // <- we receive an external update (that we are now erroneously ignoring)
        // <- we receive the websocket ack message from the sent message (we are now erroenously setting that)
        recv(true);

        return (int) result;
    }

    int wait() const noexcept
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(socket(), &rfds);
        int ret = select(socket() + 1, &rfds, NULL, NULL, 0);
        if (ret == -1)
        {
            std::cerr << "select: " << strerror(errno) << std::endl;
            abort();
        }
        return ret;
    }

    inline uint8_t mireds_to_cct(uint16_t aMireds)
    {
        uint16_t kelvin = static_cast<uint16_t>(1000000 / aMireds);
        if (kelvin < 1900 || kelvin > 10091)
        {
            std::cerr << "Matter requested an unsupported Kelvin for WLED: " << kelvin << std::endl;
            abort();
        }
        return static_cast<uint8_t>(255 * (kelvin - 1900) / (10091 - 1900));
    }

    inline uint16_t cct_to_mireds(uint8_t aCct)
    {
        // Kelvin range for WLED is 1900 to 10091
        uint16_t kelvin = static_cast<uint16_t>((aCct * ((10091 - 1900) / 255)) + 1900);
        return static_cast<uint16_t>(1000000 / kelvin);
    }

    struct led_state
    {
        bool on;
        uint8_t brightness;
        int capabilities;
        uint8_t cct;
        RgbColor rgb;
        HsvColor hsv;
    };

    struct led_info
    {
        std::string name;
        std::string manufacturer = "Aircookie/WLED";
        std::string serial_number;
        std::string model;
    };

    std::mutex mutex;
    std::string websocket_addr;
    CURL * curl;
    led_state led_state;
    led_info led_info;
    std::jthread reconnect_thread;

    Json::Reader reader;
    Json::FastWriter writer;

    static constexpr int MAX_WEBSOCKET_BYTES = 1450;
};
